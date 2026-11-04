/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "agent_config.h"
#include "channels/mqtt_channel.h"
#include "agent_compat.h"
#include "infra/config_store.h"
#include "core/message_bus.h"
#include "core/session_mgr.h"   /* 故事记录回传要用（决赛加） */
#include "infra/cron_service.h" /* 家长留言要排一个定时任务（决赛加） */
#include "cJSON.h"
#include "mbedtls/base64.h"     /* 切片走 base64，避开 JSON 转义和 UTF-8 断字 */

#include <mqtt.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>

static const char *TAG = "mqtt";

/* Bounded timeout for the TCP connect in tcp_connect(). A blocking
 * connect() can stall for tens of seconds on an unreachable broker;
 * on_reconnect() runs while holding s_client.mutex, so a long stall there
 * also blocks the outbound dispatch task (mqtt_publish waits on the same
 * mutex). Keep it short enough to avoid stalling dispatch. */
#define MQTT_TCP_CONNECT_TIMEOUT_MS 5000

/* ── Configuration ─────────────────────────────────────────── */

static char s_broker_host[128];
static int  s_broker_port;
static char s_client_id[64];
static char s_topic_in[128];
static char s_topic_out[128];
static char s_username[64];
static char s_password[128];

/* ── MQTT-C state ──────────────────────────────────────────── */

static struct mqtt_client s_client;
static uint8_t s_sendbuf[AGENT_MQTT_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t s_recvbuf[AGENT_MQTT_BUF_SIZE];
static int     s_sockfd = -1;
static volatile bool s_running;
static volatile bool s_sync_thread_exited = true;
static bool s_client_inited = false;

/* ── 出站可观测性（决赛加） ────────────────────────────────────────
 *
 * 2026-09-15 出过一次「板子收得到、一条都发不出」，当晚重烧固件后自愈，
 * 根因至今没查明。当时最难受的一点是：**发送失败在日志里完全看不见**。
 * 出站派发是单线程，被某个慢连接一卡，所有通道（语音、CLI、MQTT）会同时
 * 哑掉，而日志上一片安静 —— 只能靠猜。
 *
 * 下面这几个计数器和 mqtt_sync_thread 里那条看门狗就是为了下次再犯时，
 * 日志能直接回答：从哪一秒开始、卡在什么状态、之前一共发出去过几条。
 *
 * 全部是只增不减的计数，读它们不加锁 —— 只是给人看个趋势，不需要精确。 */

static volatile uint32_t s_tx_ok   = 0;  /* 成功 publish 的次数 */
static volatile uint32_t s_tx_fail = 0;  /* mqtt_publish 返回错的次数 */
static volatile uint32_t s_tx_drop = 0;  /* 还没到 publish 就被拦掉的次数 */
static volatile int      s_last_err = 0; /* 最近一次失败的错误码 */
static volatile time_t   s_last_ok_sec = 0;  /* 最近一次成功发出的时刻 */
static volatile bool     s_silent_warned = false; /* 「很久没发出去」只报一次 */

/* ── 回执延后发送（v2.93） ────────────────────────────────────────
 *
 * MQTT-C 在 __mqtt_recv() 里是**拿着 client->mutex** 调 on_publish() 的，
 * 而这把锁不是递归锁（mqtt_pal.h: pthread_mutex_init(mtx, NULL)）。所以
 * 收消息的回调里只要再 publish 一次，就会自己等自己：sync 线程永久卡死，
 * 之后收不到任何消息、也发不出任何东西，串口一条错误都不打。
 *
 * 睡觉时间 / 孩子名单 / 起名字 / 留言这四种回执原来都是这么发的。
 * 2026-11-04 真机踩到：家长端点一次「设定」，板子的 MQTT 从此不动。
 * 现在回调只把回执排进这个小队列，由 sync 线程在 mqtt_sync() 返回之后
 * 发出去 —— 那时锁已经放开。 */

#define MQTT_ACK_QUEUE  4
#define MQTT_ACK_LEN    192

static pthread_mutex_t s_ack_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_ack_to[MQTT_ACK_QUEUE][64];
static char s_ack_text[MQTT_ACK_QUEUE][MQTT_ACK_LEN];
static int  s_ack_head;
static int  s_ack_tail;

static void mqtt_ack_defer(const char *to, const char *text)
{
    int next;

    if (to == NULL || to[0] == '\0' || text == NULL || text[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&s_ack_lock);

    next = (s_ack_head + 1) % MQTT_ACK_QUEUE;
    if (next == s_ack_tail) {
        pthread_mutex_unlock(&s_ack_lock);
        syslog(LOG_WARNING, "[%s] ack 队列满，丢掉一条回执\n", TAG);
        return;
    }

    strncpy(s_ack_to[s_ack_head], to, sizeof(s_ack_to[0]) - 1);
    s_ack_to[s_ack_head][sizeof(s_ack_to[0]) - 1] = '\0';
    strncpy(s_ack_text[s_ack_head], text, sizeof(s_ack_text[0]) - 1);
    s_ack_text[s_ack_head][sizeof(s_ack_text[0]) - 1] = '\0';
    s_ack_head = next;

    pthread_mutex_unlock(&s_ack_lock);
}

/* 只在 sync 线程里调，且必须在 mqtt_sync() 返回之后。 */
static void mqtt_ack_pump(void)
{
    char to[64];
    char text[MQTT_ACK_LEN];

    for (;;) {
        pthread_mutex_lock(&s_ack_lock);
        if (s_ack_tail == s_ack_head) {
            pthread_mutex_unlock(&s_ack_lock);
            return;
        }
        strcpy(to, s_ack_to[s_ack_tail]);
        strcpy(text, s_ack_text[s_ack_tail]);
        s_ack_tail = (s_ack_tail + 1) % MQTT_ACK_QUEUE;
        pthread_mutex_unlock(&s_ack_lock);

        mqtt_channel_send(to, text);
    }
}

/* ── 故事记录回传（决赛加） ──
 * 实现放在文件后半（要在 sync 线程旁边），但 on_publish 里就要用，
 * 所以先在这里声明。 */
static void mqtt_hist_request(const char *to, const char *target);
static void mqtt_hist_pump(void);

/* 家长留言（决赛加）：同样定义在后半，on_publish 里要用 */
static void mqtt_note_handle(const char *to, const char *from,
                             const char *content, long delay_s);

/* 家长端给板子起名字（决赛加） */
static void mqtt_petname_handle(const char *to, const char *name);

/* 家长端设睡觉时间（决赛加） */
static void mqtt_sleep_handle(const char *to, int from_h, int to_h);

/* 家长端设孩子名单（决赛加） */
static void mqtt_kid_handle(const char *to, const char *list);

/* ── TCP connect helper ────────────────────────────────────── */

static int tcp_connect(const char *host, int port)
{
    char port_str[8];
    int fd;
    int flags;

    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        syslog(LOG_ERR, "[%s] DNS resolve failed: %s\n", TAG, host);
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* Non-blocking connect with a bounded timeout (see the comment on
     * MQTT_TCP_CONNECT_TIMEOUT_MS for why this matters). */
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0
        && errno != EINPROGRESS) {
        syslog(LOG_ERR, "[%s] TCP connect failed: %s:%d (%s)\n",
               TAG, host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {
        .tv_sec = MQTT_TCP_CONNECT_TIMEOUT_MS / 1000,
        .tv_usec = (MQTT_TCP_CONNECT_TIMEOUT_MS % 1000) * 1000
    };

    if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
        syslog(LOG_ERR, "[%s] TCP connect timeout: %s:%d\n",
               TAG, host, port);
        close(fd);
        return -1;
    }

    int sock_err = 0;
    socklen_t sock_errlen = sizeof(sock_err);

    getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_errlen);
    if (sock_err != 0) {
        syslog(LOG_ERR, "[%s] TCP connect failed: %s:%d (%s)\n",
               TAG, host, port, strerror(sock_err));
        close(fd);
        return -1;
    }

    /* Keep the socket NON-BLOCKING for subsequent send/recv.
     *
     * This is the actual fix for the reminder being silent: MQTT-C's
     * mqtt_pal_recvall()/mqtt_pal_sendall() are written for non-blocking
     * sockets (EAGAIN == "nothing to do"), and mqtt_sync's usleep() paces the
     * loop. A *blocking* recv() here blocks while holding client->mutex inside
     * __mqtt_recv(), which both starves mqtt_publish() on the outbound
     * dispatch thread AND stops __mqtt_send() from ever flushing the queue
     * (the sync thread never reaches __mqtt_send while stuck in recv). On a
     * flaky WiFi (half-dead TCP, no FIN/RST) SO_RCVTIMEO does not even fire,
     * so the block is effectively forever. Non-blocking recv() always returns
     * EAGAIN immediately. */
    /* (leave fd in O_NONBLOCK from the connect above) */

    /* Keep SO_RCVTIMEO as a fallback in case the stack ignores O_NONBLOCK on
     * recv — worst case it bounds the block to 1s. */
    struct timeval tv_rcv;

    tv_rcv.tv_sec = 1;
    tv_rcv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));

    syslog(LOG_INFO, "[%s] TCP connected to %s:%d (fd=%d, non-blocking)\n",
           TAG, host, port, fd);
    return fd;
}

/* ── Publish callback (incoming messages from broker) ──────── */

static void on_publish(void **state, struct mqtt_response_publish *pub)
{
    (void)state;

    if (pub->application_message_size == 0) {
        return;
    }

    /* NUL-terminate the payload (MQTT-C does not guarantee it) */
    size_t len = pub->application_message_size;
    char *payload = malloc(len + 1);

    if (!payload) {
        return;
    }

    memcpy(payload, pub->application_message, len);
    payload[len] = '\0';

    syslog(LOG_INFO, "[%s] Recv [%.*s]: %.64s\n",
           TAG, (int)pub->topic_name_size,
           (const char *)pub->topic_name, payload);

    /* Parse JSON: {"type":"message","content":"...","chat_id":"..."} */
    cJSON *root = cJSON_Parse(payload);

    if (!root) {
        /* Plain text fallback — treat entire payload as content */
        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_MQTT, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "mqtt_default", sizeof(msg.chat_id) - 1);
        msg.content = payload; /* bus takes ownership on success */
        if (message_bus_push_inbound(&msg) != OK) {
            free(payload);
        }
        return;
    }

    /* ── 家长端要故事记录：本地读、切片回发，不经过 Agent ──────────
     * 走 Agent 的话要绕一次 LLM（慢、花钱、而且模型会把它改写一遍），
     * 而这件事是纯粹的「取数据」，直接回更合适，也省一次往返。
     * 请求格式：{"type":"history","target":"kid_buddy:story","chat_id":"parent"} */
    {
        cJSON *type_item = cJSON_GetObjectItem(root, "type");

        if (cJSON_IsString(type_item)
            && strcmp(type_item->valuestring, "history") == 0) {
            cJSON *tgt = cJSON_GetObjectItem(root, "target");
            cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
            const char *tgt_s = (cJSON_IsString(tgt) && tgt->valuestring[0])
                                ? tgt->valuestring : NULL;
            const char *cid_s = (cJSON_IsString(cid) && cid->valuestring[0])
                                ? cid->valuestring : "mqtt_default";

            if (tgt_s != NULL) {
                mqtt_hist_request(cid_s, tgt_s);
            } else {
                syslog(LOG_WARNING,
                       "[%s] history: 请求里没带 target，忽略\n", TAG);
            }

            cJSON_Delete(root);
            free(payload);
            return;
        }
    }

    /* ── 家长留言：排一个定时任务，到点让孩子听见爸妈的原话 ──────
     * 请求格式：{"type":"note","content":"...","from":"妈妈",
     *            "delay":3600,"chat_id":"parent"}
     * delay 是「多少秒之后」，0 或省略 = 马上。 */
    {
        cJSON *type_item = cJSON_GetObjectItem(root, "type");

        if (cJSON_IsString(type_item)
            && strcmp(type_item->valuestring, "note") == 0) {
            cJSON *content_j = cJSON_GetObjectItem(root, "content");
            cJSON *from_j    = cJSON_GetObjectItem(root, "from");
            cJSON *delay_j   = cJSON_GetObjectItem(root, "delay");
            cJSON *cid       = cJSON_GetObjectItem(root, "chat_id");
            const char *cid_s = (cJSON_IsString(cid) && cid->valuestring[0])
                                ? cid->valuestring : "mqtt_default";
            long delay_s = (cJSON_IsNumber(delay_j))
                           ? (long)delay_j->valuedouble : 0;

            if (cJSON_IsString(content_j) && content_j->valuestring[0]) {
                mqtt_note_handle(cid_s,
                                 (cJSON_IsString(from_j))
                                     ? from_j->valuestring : NULL,
                                 content_j->valuestring,
                                 delay_s);
            } else {
                syslog(LOG_WARNING, "[%s] note: 留言是空的，忽略\n", TAG);
            }

            cJSON_Delete(root);
            free(payload);
            return;
        }
    }

    /* ── 家长端给板子起名字 ──────────────────────────────────────
     * 请求格式：{"type":"petname","name":"小豆丁","chat_id":"parent"} */
    {
        cJSON *type_item = cJSON_GetObjectItem(root, "type");

        if (cJSON_IsString(type_item)
            && strcmp(type_item->valuestring, "petname") == 0) {
            cJSON *name_j = cJSON_GetObjectItem(root, "name");
            cJSON *cid    = cJSON_GetObjectItem(root, "chat_id");
            const char *cid_s = (cJSON_IsString(cid) && cid->valuestring[0])
                                ? cid->valuestring : "mqtt_default";

            if (cJSON_IsString(name_j) && name_j->valuestring[0]) {
                mqtt_petname_handle(cid_s, name_j->valuestring);
            }

            cJSON_Delete(root);
            free(payload);
            return;
        }
    }

    /* ── 家长端设睡觉时间 ────────────────────────────────────────
     * 请求格式：{"type":"sleep","from":21,"to":7,"chat_id":"parent"}
     * from == to 表示关掉这个功能（不睡）。 */
    {
        cJSON *type_item = cJSON_GetObjectItem(root, "type");

        if (cJSON_IsString(type_item)
            && strcmp(type_item->valuestring, "sleep") == 0) {
            cJSON *from_j = cJSON_GetObjectItem(root, "from");
            cJSON *to_j   = cJSON_GetObjectItem(root, "to");
            cJSON *cid    = cJSON_GetObjectItem(root, "chat_id");
            const char *cid_s = (cJSON_IsString(cid) && cid->valuestring[0])
                                ? cid->valuestring : "mqtt_default";
            int from_h = cJSON_IsNumber(from_j) ? (int)from_j->valuedouble : -1;
            int to_h   = cJSON_IsNumber(to_j)   ? (int)to_j->valuedouble   : -1;

            mqtt_sleep_handle(cid_s, from_h, to_h);

            cJSON_Delete(root);
            free(payload);
            return;
        }
    }

    /* ── 家长端设孩子名单 ────────────────────────────────────────
     * 请求格式：{"type":"kid","list":"小明|小红","chat_id":"parent"}
     * 第一个名字是当前那个；留空 = 取消档案，回到单孩子那条路。 */
    {
        cJSON *type_item = cJSON_GetObjectItem(root, "type");

        if (cJSON_IsString(type_item)
            && strcmp(type_item->valuestring, "kid") == 0) {
            cJSON *list_j = cJSON_GetObjectItem(root, "list");
            cJSON *cid    = cJSON_GetObjectItem(root, "chat_id");
            const char *cid_s = (cJSON_IsString(cid) && cid->valuestring[0])
                                ? cid->valuestring : "mqtt_default";

            mqtt_kid_handle(cid_s,
                            (cJSON_IsString(list_j)) ? list_j->valuestring : "");

            cJSON_Delete(root);
            free(payload);
            return;
        }
    }

    cJSON *content_item = cJSON_GetObjectItem(root, "content");
    cJSON *chat_id_item = cJSON_GetObjectItem(root, "chat_id");

    if (cJSON_IsString(content_item) && content_item->valuestring[0]) {
        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_MQTT, sizeof(msg.channel) - 1);

        if (cJSON_IsString(chat_id_item) && chat_id_item->valuestring[0]) {
            strncpy(msg.chat_id, chat_id_item->valuestring,
                    sizeof(msg.chat_id) - 1);
        } else {
            strncpy(msg.chat_id, "mqtt_default", sizeof(msg.chat_id) - 1);
        }

        msg.content = strdup(content_item->valuestring);
        if (msg.content) {
            if (message_bus_push_inbound(&msg) != OK) {
                free(msg.content);
            }
        }
    }

    cJSON_Delete(root);
    free(payload);
}

/* ── Reconnect callback ────────────────────────────────────── */

static void on_reconnect(struct mqtt_client *client, void **state)
{
    (void)state;

    /* Close old socket if any */
    if (s_sockfd >= 0) {
        close(s_sockfd);
        s_sockfd = -1;
    }

    syslog(LOG_INFO, "[%s] Reconnecting to %s:%d...\n",
           TAG, s_broker_host, s_broker_port);

    s_sockfd = tcp_connect(s_broker_host, s_broker_port);
    if (s_sockfd < 0) {
        syslog(LOG_ERR, "[%s] Reconnect TCP failed\n", TAG);
        /* Reinit with invalid fd so MQTT-C stays in error state
         * and will call reconnect_callback again on next mqtt_sync. */
        mqtt_reinit(client, -1,
                    s_sendbuf, sizeof(s_sendbuf),
                    s_recvbuf, sizeof(s_recvbuf));
        return;
    }

    mqtt_reinit(client, s_sockfd,
                s_sendbuf, sizeof(s_sendbuf),
                s_recvbuf, sizeof(s_recvbuf));

    uint8_t flags = MQTT_CONNECT_CLEAN_SESSION;
    const char *user = s_username[0] ? s_username : NULL;
    const char *pass = s_password[0] ? s_password : NULL;

    enum MQTTErrors err = mqtt_connect(client, s_client_id,
                                       NULL, NULL, 0,
                                       user, pass, flags,
                                       AGENT_MQTT_KEEPALIVE);
    if (err != MQTT_OK) {
        syslog(LOG_ERR, "[%s] mqtt_connect failed: %d\n", TAG, (int)err);
        return;
    }

    /* Subscribe to inbound topic */
    mqtt_subscribe(client, s_topic_in, 1);
    syslog(LOG_INFO, "[%s] Subscribed to %s\n", TAG, s_topic_in);
}

/* ── Sync thread (calls mqtt_sync periodically) ───────────── */

/* ── 家长留言（决赛加） ────────────────────────────────────────────
 *
 * 家长在手机上写一句话、挑个时间，板子到点用 TTS 念给孩子，脸显高兴。
 *
 * 这件事其实**不需要新造轮子** —— 它就是把「家长从远端建的提醒」那条现成的
 * 链路，绕开大模型直接用一次：
 *
 *   · agent_loop.c 里已经规定好：从远端家长频道建的提醒会被强制转到
 *     channel=local_client / chat_id=kid_buddy，也就是交给 kid_buddy 去念；
 *     同时把 report_channel/report_chat_id 设成家长那边 —— 孩子说「知道了」
 *     时家长会收到回执。我们照抄这个约定即可。
 *   · cron_service 负责到点触发、而且落盘（重启不丢），一次性任务
 *     靠 delete_after_run 自动删除。
 *
 * **为什么必须绕开大模型**：提醒走模型时，模型要把「十分钟后喝水」这种自然语言
 * 解析成结构化参数，顺带会把内容重写一遍。而留言是家长的**原话**，
 * 被改写就走味了 —— 妈妈说的话不该经过第三个人的嘴。
 *
 * **时间怎么算**：家长端只发「多少秒之后」，板子自己算 at = now + delay。
 * 这样哪怕板子的时钟还没校准（这块板子没有 RTC），时长也是准的 ——
 * 时钟的绝对值可能偏，但走一秒还是一秒。
 */

#define MQTT_NOTE_MAX     180   /* 正文上限（cron_job_t.message 是 256 字节，
                                 * 得给「XX给你留了句话：」留出余地） */
#define MQTT_NOTE_WHO_MAX  24

/* 按 UTF-8 边界截断 —— 别把一个汉字劈成两半，那样念出来是乱码 */
static size_t mqtt_utf8_cap(const char *s, size_t cap)
{
    size_t n = strlen(s);

    if (n <= cap) {
        return n;
    }

    n = cap;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }

    return n;
}

static void mqtt_note_handle(const char *to, const char *from,
                             const char *content, long delay_s)
{
    cron_job_t job;
    char       ack[160];
    size_t     clen;
    size_t     wlen;
    const char *who;

    if (to == NULL || to[0] == '\0' || content == NULL || content[0] == '\0') {
        return;
    }

    who  = (from != NULL && from[0] != '\0') ? from : "爸爸妈妈";
    wlen = mqtt_utf8_cap(who, MQTT_NOTE_WHO_MAX);
    clen = mqtt_utf8_cap(content, MQTT_NOTE_MAX);

    memset(&job, 0, sizeof(job));

    strncpy(job.name, "家长留言", sizeof(job.name) - 1);
    snprintf(job.message, sizeof(job.message), "%.*s给你留了句话：%.*s",
             (int)wlen, who, (int)clen, content);

    job.kind       = CRON_KIND_AT;
    job.at_epoch   = (int64_t)time(NULL) + (delay_s > 0 ? delay_s : 1);
    job.delete_after_run = true;
    job.enabled    = true;

    /* 交给 kid_buddy 去念，和远端提醒走同一条路 */
    strncpy(job.channel, AGENT_CHAN_LOCAL_CLIENT, sizeof(job.channel) - 1);
    strncpy(job.chat_id, "kid_buddy", sizeof(job.chat_id) - 1);

    /* 孩子听完说「知道了」时，回执发回给这位家长 */
    strncpy(job.report_channel, AGENT_CHAN_MQTT, sizeof(job.report_channel) - 1);
    strncpy(job.report_chat_id, to, sizeof(job.report_chat_id) - 1);

    if (cron_add_job(&job) != OK) {
        syslog(LOG_ERR, "[%s] note: 排任务失败（可能定时任务满了）\n", TAG);
        mqtt_ack_defer(to, "留言没排上（板子上的定时任务满了），过一会儿再试试。");
        return;
    }

    syslog(LOG_INFO, "[%s] note: %ld 秒后念给孩子的留言（%.*s）\n",
           TAG, delay_s, (int)wlen, who);

    if (delay_s >= 3600) {
        snprintf(ack, sizeof(ack), "留言收到，%.1f 小时后念给孩子。",
                 (double)delay_s / 3600.0);
    } else if (delay_s >= 60) {
        snprintf(ack, sizeof(ack), "留言收到，%ld 分钟后念给孩子。",
                 delay_s / 60);
    } else if (delay_s > 0) {
        snprintf(ack, sizeof(ack), "留言收到，%ld 秒后念给孩子。", delay_s);
    } else {
        snprintf(ack, sizeof(ack), "留言收到，马上就念给孩子。");
    }

    mqtt_ack_defer(to, ack);
}

/* ── 家长端给板子起名字（决赛加） ──────────────────────────────────
 *
 * 孩子自己喊一句「你叫小豆丁」那条路最好玩，但语音识别名字本来就不靠谱
 * （转写会把名字认成别的词），所以**家长端这条可靠的路必须有**。
 *
 * 走的是和提醒回执同一套约定：往 local_client/kid_buddy 推一条带前缀的
 * 消息，由 kid_buddy 的 notify 回调认领 —— 那边看到 [KID_PETNAME:...]
 * 就设名字，不当成要念的话。
 */

/* 把一个「控制行」推给板子上的 kid_buddy。它认得这些方括号前缀，
 * 不会把它们当成要念出来的话。家长的这几项设置都走这条路。 */
static int mqtt_push_control(const char *line)
{
    agent_msg_t msg;

    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, AGENT_CHAN_LOCAL_CLIENT, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "kid_buddy", sizeof(msg.chat_id) - 1);
    msg.content = strdup(line);

    if (msg.content == NULL) {
        return ERROR;
    }

    if (message_bus_push_outbound(&msg) != OK) {
        free(msg.content);
        return ERROR;
    }

    return OK;
}

/* 必须和 kid_buddy.c 里的 PETNAME_MAX 一致 */
#define MQTT_PETNAME_MAX  12

static void mqtt_petname_handle(const char *to, const char *name)
{
    char   line[MQTT_PETNAME_MAX + 24];
    size_t n;

    if (name == NULL || name[0] == '\0') {
        return;
    }

    n = strlen(name);

    if (n > MQTT_PETNAME_MAX) {
        syslog(LOG_WARNING, "[%s] petname: 名字太长（%u 字节），拒绝\n",
               TAG, (unsigned)n);
        if (to != NULL && to[0] != '\0') {
            mqtt_ack_defer(to, "这个名字太长了，换短一点的吧。");
        }
        return;
    }

    /* 名字里不能有 ] —— 那是前缀的结束符，混进去会把协议撑破 */
    if (strchr(name, ']') != NULL) {
        syslog(LOG_WARNING, "[%s] petname: 名字里有 ']'，拒绝\n", TAG);
        return;
    }

    snprintf(line, sizeof(line), "[KID_PETNAME:%s]", name);

    if (mqtt_push_control(line) != OK) {
        syslog(LOG_WARNING, "[%s] petname: 推送失败\n", TAG);
        return;
    }

    syslog(LOG_INFO, "[%s] petname: 家长把板子命名为「%s」\n", TAG, name);

    if (to != NULL && to[0] != '\0') {
        char ack[96];
        snprintf(ack, sizeof(ack), "好，板子以后就叫「%s」了。", name);
        mqtt_ack_defer(to, ack);
    }
}

/* 家长端设孩子名单。list 形如 "小明|小红"，第一个是当前那个。 */
static void mqtt_kid_handle(const char *to, const char *list)
{
    char line[96];
    char ack[128];

    if (list == NULL || list[0] == '\0') {
        return;
    }

    if (strlen(list) > 60) {
        syslog(LOG_WARNING, "[%s] kid: 名单太长，拒绝\n", TAG);
        return;
    }

    /* 和前缀协议冲突的字符一律挡掉 */
    if (strchr(list, ']') != NULL || strchr(list, '[') != NULL) {
        syslog(LOG_WARNING, "[%s] kid: 名单里有方括号，拒绝\n", TAG);
        return;
    }

    snprintf(line, sizeof(line), "[KID_KIDS:%s]", list);

    if (mqtt_push_control(line) != OK) {
        syslog(LOG_WARNING, "[%s] kid: 推送失败\n", TAG);
        return;
    }

    syslog(LOG_INFO, "[%s] kid: 名单设为 %s\n", TAG, list);

    if (to != NULL && to[0] != '\0') {
        snprintf(ack, sizeof(ack), "好，孩子名单已更新：%s（第一个是当前的）。", list);
        mqtt_ack_defer(to, ack);
    }
}

/* 家长端设睡觉时间。from == to 表示关掉这个功能。 */
static void mqtt_sleep_handle(const char *to, int from_h, int to_h)
{
    char line[48];
    char ack[128];

    if (from_h < 0 || from_h > 23 || to_h < 0 || to_h > 23) {
        syslog(LOG_WARNING, "[%s] sleep: 钟点不合法（%d %d）\n",
               TAG, from_h, to_h);
        if (to != NULL && to[0] != '\0') {
            mqtt_ack_defer(to, "钟点要在 0 到 23 之间。");
        }
        return;
    }

    snprintf(line, sizeof(line), "[KID_SLEEP:%d|%d]", from_h, to_h);

    if (mqtt_push_control(line) != OK) {
        syslog(LOG_WARNING, "[%s] sleep: 推送失败\n", TAG);
        return;
    }

    syslog(LOG_INFO, "[%s] sleep: 睡觉时间设为 %02d:00 → %02d:00\n",
           TAG, from_h, to_h);

    if (to != NULL && to[0] != '\0') {
        if (from_h == to_h) {
            snprintf(ack, sizeof(ack), "好，不设睡觉时间了。");
        } else {
            snprintf(ack, sizeof(ack), "好，板子 %02d:00 睡、%02d:00 醒。",
                     from_h, to_h);
        }
        mqtt_ack_defer(to, ack);
    }
}

/* ── 故事记录回传（决赛加） ────────────────────────────────────────
 *
 * 家长在手机端点「同步」，板子把孩子的角色扮演会话记录发回去，存在手机本地。
 * 让家长能看见孩子在故事里做过哪些选择 —— 这是这个作品里唯一一条
 * 「给家长看内容，而不是看状态」的链路。
 *
 * 为什么不能干脆一次发完：
 *   1. MQTT 的发送缓冲只有 AGENT_MQTT_BUF_SIZE 字节，历史轻轻松松超；
 *   2. mqtt_publish 是往缓冲里写，背靠背连发几条就会撞上
 *      MQTT_ERROR_SEND_BUFFER_IS_FULL。
 * 所以这里切成小片，**由已有的 sync 线程每 200ms 发一片**，天然限速，
 * 不需要新线程，也不会把缓冲撑爆。
 *
 * 2026-10-27 实板教训：光切小还不够。QoS1 的消息要等 PUBACK 才从队列里
 * 释放，而 sync 线程的顺序是「先 mqtt_sync 把上一片发出去，紧接着
 * mqtt_hist_pump 发下一片」—— 上一片的 PUBACK 根本还没回来。
 * v2.81 每片 384 字节原始数据（打包后 622 字节），1024 的缓冲装不下两片，
 * 于是第 2/4/6 片全部 publish failed(err=-2147483632 = SEND_BUFFER_IS_FULL)；
 * 更糟的是失败的那一片当时已经推进了 offset，直接永久丢失。
 * 家长端只收到 3/6 片，拼不齐就不导入 —— 故事书当然一片空白。
 *
 * 现在的三条约束：
 *   a. 每片 256 字节原始（打包后约 454 字节），两片 988 字节；
 *   b. AGENT_MQTT_BUF_SIZE 拉到 2048，给错车进来的其它消息留余量；
 *   c. 发失败**不推进** offset，下一拍重发同一片；连续失败到上限才放弃整批。
 *
 * 为什么走 base64：
 *   历史本身就是 JSON 文本，塞进另一个 JSON 字符串要转义一堆引号和反斜杠
 *   （体积翻倍）；切片又是按字节切的，直接塞进 JSON 字符串还会撞上非法
 *   UTF-8（正好切在汉字中间）。base64 之后是纯 ASCII，两个问题一起消失 ——
 *   代价只是 4/3 的体积，切在汉字中间也就无所谓了（家长端按字节拼回去）。
 *
 * 板子这边只保留最近 AGENT_SESSION_MAX_MSGS（20）条并滚动截断，
 * 所以**长期档案存在家长手机那侧**：每次同步把新出现的合并进去，
 * 按 (role, content, ts) 去重。板子是窗口，手机是档案。
 */

#define MQTT_HIST_RAW     256     /* 每片原始字节数（base64 后 344 字符） */
#define MQTT_HIST_MAX     16384   /* 一次读进来的历史上限 */
#define MQTT_HIST_MAXSEQ  96      /* 片数上限（16384/256 最多 64 片） */
#define MQTT_HIST_RETRY   150     /* 同一片最多重试 150 拍（200ms × 150 = 30s） */

/* 外接灯带的「正在回传故事记录」位。真正的实现和位值在
 * app/kid_buddy/led_strip.c / led_strip.h；FLAT 构建下两个 app 在同一个
 * 地址空间，可以直接调。声明成 weak 是为了 ai_agent 单独编译/运行时
 * 不会因为没有 kid_buddy 而链接失败。位值必须和 LED_STRIP_ACT_BOOK 一致。 */
#define MQTT_LED_ACT_BOOK (1u << 1)

extern void led_strip_set_activity(unsigned int activity, int on)
    __attribute__((weak));

static void mqtt_hist_led(int on)
{
    if (led_strip_set_activity != NULL) {
        led_strip_set_activity(MQTT_LED_ACT_BOOK, on);
    }
}

static pthread_mutex_t s_hist_lock = PTHREAD_MUTEX_INITIALIZER;

static char  *s_hist_buf   = NULL;   /* 整个历史 JSON（malloc 出来的） */
static size_t s_hist_len   = 0;
static size_t s_hist_off   = 0;      /* 已经发到哪了 */
static int    s_hist_seq   = 0;      /* 已发片数（0 基） */
static int    s_hist_total = 0;      /* 一共几片 */
static int    s_hist_retry = 0;      /* 当前这片已经连续失败几次 */
static char   s_hist_to[64];         /* 回给哪个 chat_id（家长） */
static char   s_hist_target[64];     /* 读的是谁的会话 */

/* 返回 MQTT_OK 才算这一片进了发送队列。失败时调用方**不能**推进 offset。 */
static enum MQTTErrors mqtt_hist_publish(const char *to, const char *target,
                                         int seq, int total, const char *b64)
{
    cJSON *o = cJSON_CreateObject();
    enum MQTTErrors err;

    if (!o) {
        return MQTT_ERROR_MALFORMED_REQUEST;
    }

    cJSON_AddStringToObject(o, "type", "story_log");
    cJSON_AddStringToObject(o, "chat_id", to);
    cJSON_AddStringToObject(o, "target", target);
    cJSON_AddNumberToObject(o, "seq", seq + 1);   /* 从 1 开始，给人看的 */
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddStringToObject(o, "data", b64);

    char *json = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (!json) {
        return MQTT_ERROR_MALFORMED_REQUEST;
    }

    err = mqtt_publish(&s_client, s_topic_out,
                       json, strlen(json), MQTT_PUBLISH_QOS_1);

    free(json);

    if (err != MQTT_OK) {
        s_tx_fail++;
        s_last_err = (int)err;
    } else {
        s_tx_ok++;
        s_last_ok_sec = time(NULL);
        s_silent_warned = false;
    }

    return err;
}

/* 每 tick 调一次：发一片。发完就收工。 */
static void mqtt_hist_pump(void)
{
    char   raw[MQTT_HIST_RAW + 4];
    char   b64[(MQTT_HIST_RAW * 4) / 3 + 8];
    size_t n, b64len = 0;
    size_t off;
    int    seq, total;
    enum MQTTErrors perr;
    char   to[sizeof(s_hist_to)];
    char   target[sizeof(s_hist_target)];

    pthread_mutex_lock(&s_hist_lock);

    if (s_hist_buf == NULL) {
        pthread_mutex_unlock(&s_hist_lock);
        return;
    }

    /* 发完了（或者片数到了上限）：释放，收工 */
    if (s_hist_off >= s_hist_len || s_hist_seq >= s_hist_total) {
        syslog(LOG_INFO, "[%s] history: 发送完成，共 %d 片\n", TAG, s_hist_seq);
        mqtt_hist_led(0);
        free(s_hist_buf);
        s_hist_buf = NULL;
        pthread_mutex_unlock(&s_hist_lock);
        return;
    }

    n = MQTT_HIST_RAW;
    if (s_hist_off + n > s_hist_len) {
        n = s_hist_len - s_hist_off;
    }

    /* 这里**故意**不按 UTF-8 字符回退。家长端是把每片 base64 各自解成字节、
     * 再把字节流拼起来做一次 UTF-8 解码的（见 phone-app/index.html 的
     * partsToUtf8），所以切在汉字中间完全没问题。
     * 反过来，回退会让某一片变短，len/RAW 算出来的 total 就少一片，
     * 最后几个字节被静默丢掉 —— JSON 直接不完整。 */
    if (n == 0) {
        mqtt_hist_led(0);
        free(s_hist_buf);
        s_hist_buf = NULL;
        pthread_mutex_unlock(&s_hist_lock);
        return;
    }

    off   = s_hist_off;
    seq   = s_hist_seq;
    total = s_hist_total;
    memcpy(to, s_hist_to, sizeof(to));
    memcpy(target, s_hist_target, sizeof(target));

    memcpy(raw, s_hist_buf + off, n);

    pthread_mutex_unlock(&s_hist_lock);

    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &b64len,
                              (const unsigned char *)raw, n) != 0) {
        /* 编码失败是本地数据问题，重发也没用：跳过这一片，否则整批卡死 */
        syslog(LOG_ERR, "[%s] history: base64 失败，丢掉这一片\n", TAG);
        pthread_mutex_lock(&s_hist_lock);
        if (s_hist_buf != NULL && s_hist_off == off) {
            s_hist_off += n;
            s_hist_seq++;
        }
        pthread_mutex_unlock(&s_hist_lock);
        return;
    }

    b64[b64len] = '\0';

    perr = mqtt_hist_publish(to, target, seq, total, b64);

    if (perr == MQTT_OK) {
        /* 发出去了才推进 —— 失败的那一片下一拍原样重发 */
        pthread_mutex_lock(&s_hist_lock);
        if (s_hist_buf != NULL && s_hist_off == off) {
            s_hist_off += n;
            s_hist_seq++;
        }
        s_hist_retry = 0;
        pthread_mutex_unlock(&s_hist_lock);
        return;
    }

    /* 失败：offset 没动，下一拍重试。只在前几次和每 20 次报一行，
     * 免得连接断了的时候 200ms 刷一条。 */
    pthread_mutex_lock(&s_hist_lock);
    s_hist_retry++;

    if (s_hist_retry == 1 || (s_hist_retry % 20) == 0) {
        syslog(LOG_ERR,
               "[%s] history chunk %d/%d 发不出去（%s），第 %d 次重试\n",
               TAG, seq + 1, total, mqtt_error_str(perr), s_hist_retry);
    }

    if (s_hist_retry >= MQTT_HIST_RETRY) {
        syslog(LOG_ERR,
               "[%s] history: 第 %d/%d 片连续 %d 次失败，放弃这一批\n",
               TAG, seq + 1, total, s_hist_retry);
        mqtt_hist_led(0);
        free(s_hist_buf);
        s_hist_buf = NULL;
        s_hist_retry = 0;
    }

    pthread_mutex_unlock(&s_hist_lock);
}

/* 家长端请求：{"type":"history","target":"kid_buddy:story","chat_id":"parent"}
 * 只把历史读进内存、把状态准备好；真正发送交给 sync 线程（见 mqtt_hist_pump）。 */
static void mqtt_hist_request(const char *to, const char *target)
{
    char *buf;
    size_t len;

    if (to == NULL || to[0] == '\0' || target == NULL || target[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&s_hist_lock);

    if (s_hist_buf != NULL) {
        syslog(LOG_WARNING,
               "[%s] history: 上一次还没发完（%d/%d），忽略本次请求\n",
               TAG, s_hist_seq, s_hist_total);
        pthread_mutex_unlock(&s_hist_lock);
        return;
    }

    pthread_mutex_unlock(&s_hist_lock);

    buf = malloc(MQTT_HIST_MAX);

    if (buf == NULL) {
        syslog(LOG_ERR, "[%s] history: 内存不足\n", TAG);
        return;
    }

    /* 没有会话文件时它会写 "[]" 并返回 OK —— 正好，空档案也是一种结果，
     * 照样发回去，让家长端显示「还没有记录」，不用另开一条消息类型。 */
    if (session_get_history_json_ts(target, buf, MQTT_HIST_MAX, 200) != OK) {
        free(buf);
        syslog(LOG_ERR, "[%s] history: 读 %s 失败\n", TAG, target);
        return;
    }

    len = strlen(buf);

    pthread_mutex_lock(&s_hist_lock);

    s_hist_buf   = buf;
    s_hist_len   = len;
    s_hist_off   = 0;
    s_hist_seq   = 0;
    s_hist_retry = 0;
    s_hist_total = (int)((len + MQTT_HIST_RAW - 1) / MQTT_HIST_RAW);

    if (s_hist_total <= 0) {
        s_hist_total = 1;
    }
    if (s_hist_total > MQTT_HIST_MAXSEQ) {
        s_hist_total = MQTT_HIST_MAXSEQ;
    }

    strncpy(s_hist_to, to, sizeof(s_hist_to) - 1);
    s_hist_to[sizeof(s_hist_to) - 1] = '\0';
    strncpy(s_hist_target, target, sizeof(s_hist_target) - 1);
    s_hist_target[sizeof(s_hist_target) - 1] = '\0';

    /* 在锁内置位：pump 完成时会在同一把锁内清零，这样即使总片数只有
     * 一两片、下一个 200ms 就发完，也不会出现「先清后置」把灯带卡在
     * 呼吸状态。 */
    mqtt_hist_led(1);

    pthread_mutex_unlock(&s_hist_lock);

    syslog(LOG_INFO, "[%s] history: %s -> %s, %u 字节, %d 片，开始发送\n",
           TAG, target, to, (unsigned)len, s_hist_total);
}

static void *mqtt_sync_thread(void *arg)
{
    (void)arg;

    syslog(LOG_INFO, "[%s] Sync thread started\n", TAG);

    /* 看门狗用的本地状态 */
    int last_client_err = (int)s_client.error;
    time_t stuck_since  = 0;

    while (s_running) {
        enum MQTTErrors err = mqtt_sync(&s_client);

        if (err != MQTT_OK) {
            syslog(LOG_WARNING, "[%s] mqtt_sync error: %d\n", TAG, (int)err);
            /* MQTT-C will call reconnect_callback automatically */
        }

        /* on_publish() 里排下的回执在这里发 —— 这时 mqtt_sync 已经放开
         * client 锁，不会自己等自己（详见 mqtt_ack_defer 的注释）。 */
        mqtt_ack_pump();

        /* ── 出站看门狗（决赛加） ────────────────────────────────
         *
         * 每秒看三样东西。都是为了回答同一个问题：**现在还能不能发出去**。
         * 正常情况下这三条一个字都不会打 —— 只有真出事了才留痕，
         * 所以日志里一旦出现 WATCHDOG，就是现场可以直接顺着查的线索。 */

        time_t now = time(NULL);
        int    ce  = (int)s_client.error;

        /* 1. 客户端的粘性错误状态。它按文档「整个连接期间都应该是 MQTT_OK」，
         *    变了就说明连接已经不健康 —— 只在这个值真的变化时报一次，
         *    不然断线重连期间会每秒刷一行。 */
        if (ce != last_client_err) {
            syslog(LOG_ERR,
                   "[%s] WATCHDOG client.error %d -> %d "
                   "(累计成功 %u / 失败 %u / 丢弃 %u)\n",
                   TAG, last_client_err, ce,
                   (unsigned)s_tx_ok, (unsigned)s_tx_fail, (unsigned)s_tx_drop);
            last_client_err = ce;
        }

        /* 2. 半截发送。send_offset 非 0 = 上一条消息写了一半卡住了。
         *    这正是「所有通道一起哑掉」最可能的样子：派发线程堵在
         *    mqtt_publish 里，后面排队的消息一条都出不去。 */
        if (s_client.send_offset != 0) {
            if (stuck_since == 0) {
                stuck_since = now;
                syslog(LOG_ERR,
                       "[%s] WATCHDOG partial send STUCK: send_offset=%u\n",
                       TAG, (unsigned)s_client.send_offset);
            } else if (now - stuck_since >= 10) {
                syslog(LOG_ERR,
                       "[%s] WATCHDOG partial send stuck for %lds "
                       "(send_offset=%u) —— 出站通道可能已经全哑\n",
                       TAG, (long)(now - stuck_since),
                       (unsigned)s_client.send_offset);
                stuck_since = now;   /* 每 10 秒报一次，不刷屏 */
            }
        } else {
            stuck_since = 0;
        }

        /* 3. 曾经发出去过，但已经两分钟没成功发过一条 —— 最贴近
         *    「板子收得到、一条都发不出」的症状本身。
         *    每条静默期只报一次（成功发送时会复位 s_silent_warned），
         *    否则板子放一晚上会攒出几百行。 */
        if (s_tx_ok > 0 && s_last_ok_sec > 0 && now >= s_last_ok_sec &&
            now - s_last_ok_sec > 120 && !s_silent_warned) {
            s_silent_warned = true;
            syslog(LOG_ERR,
                   "[%s] WATCHDOG outbound SILENT for %lds "
                   "(累计成功 %u / 失败 %u / 丢弃 %u, client.error=%d)\n",
                   TAG, (long)(now - s_last_ok_sec),
                   (unsigned)s_tx_ok, (unsigned)s_tx_fail, (unsigned)s_tx_drop,
                   ce);
        }

        /* 故事记录切片：每 tick 发一片。放在这里而不是 request 里，
         * 是因为发送缓冲只有 1KB，背靠背 publish 会把它撑爆 ——
         * 借 sync 线程的 200ms 节奏天然限速，不发也不用额外线程。 */
        mqtt_hist_pump();

        usleep(AGENT_MQTT_SYNC_INTERVAL_MS * 1000);
    }

    s_sync_thread_exited = true;
    syslog(LOG_INFO, "[%s] Sync thread exited\n", TAG);
    return NULL;
}

/* ── Parse broker URL "host:port" ──────────────────────────── */

static void parse_broker(const char *broker, char *host, size_t host_sz,
                         int *port)
{
    const char *colon = strrchr(broker, ':');

    if (colon && colon != broker) {
        size_t hlen = (size_t)(colon - broker);

        if (hlen >= host_sz) {
            hlen = host_sz - 1;
        }

        memcpy(host, broker, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        strncpy(host, broker, host_sz - 1);
        host[host_sz - 1] = '\0';
        *port = AGENT_MQTT_DEFAULT_PORT;
    }

    if (*port <= 0 || *port > 65535) {
        *port = AGENT_MQTT_DEFAULT_PORT;
    }
}

/* ── Public API ────────────────────────────────────────────── */

int mqtt_channel_init(void)
{
    char broker_buf[192];

    memset(broker_buf, 0, sizeof(broker_buf));
    memset(s_broker_host, 0, sizeof(s_broker_host));
    memset(s_client_id, 0, sizeof(s_client_id));
    memset(s_topic_in, 0, sizeof(s_topic_in));
    memset(s_topic_out, 0, sizeof(s_topic_out));
    memset(s_username, 0, sizeof(s_username));
    memset(s_password, 0, sizeof(s_password));
    s_broker_port = AGENT_MQTT_DEFAULT_PORT;

    /* Read config from config store */
    claw_config_get(AGENT_CFG_KEY_MQTT_BROKER,
                    broker_buf, sizeof(broker_buf));
    claw_config_get(AGENT_CFG_KEY_MQTT_CLIENT_ID,
                    s_client_id, sizeof(s_client_id));
    claw_config_get(AGENT_CFG_KEY_MQTT_TOPIC_IN,
                    s_topic_in, sizeof(s_topic_in));
    claw_config_get(AGENT_CFG_KEY_MQTT_TOPIC_OUT,
                    s_topic_out, sizeof(s_topic_out));
    claw_config_get(AGENT_CFG_KEY_MQTT_USERNAME,
                    s_username, sizeof(s_username));
    claw_config_get(AGENT_CFG_KEY_MQTT_PASSWORD,
                    s_password, sizeof(s_password));

    /* Parse broker host:port */
    if (broker_buf[0]) {
        parse_broker(broker_buf, s_broker_host,
                     sizeof(s_broker_host), &s_broker_port);
    }

    /* Defaults */
    if (!s_client_id[0]) {
        snprintf(s_client_id, sizeof(s_client_id),
                 "agent-%s", AGENT_NODE_ID);
    }

    if (!s_topic_in[0]) {
        strncpy(s_topic_in, AGENT_MQTT_TOPIC_IN_DEFAULT,
                sizeof(s_topic_in) - 1);
    }

    if (!s_topic_out[0]) {
        strncpy(s_topic_out, AGENT_MQTT_TOPIC_OUT_DEFAULT,
                sizeof(s_topic_out) - 1);
    }

    syslog(LOG_INFO, "[%s] MQTT channel initialized (broker=%s:%d)\n",
           TAG, s_broker_host[0] ? s_broker_host : "(not set)",
           s_broker_port);
    return OK;
}

int mqtt_channel_start(void)
{
    if (!s_broker_host[0]) {
        syslog(LOG_INFO, "[%s] No mqtt_broker configured, MQTT disabled\n",
               TAG);
        return OK;
    }

    /* Idempotent: if a sync thread is already running, don't start another.
     * Two threads sharing one s_client + the same client_id will CONNECT
     * twice and the broker keeps kicking one off → endless reconnect loop. */
    if (s_running) {
        syslog(LOG_INFO, "[%s] MQTT already running, skip duplicate start\n",
               TAG);
        return OK;
    }

    /* Initialize the MQTT-C client exactly once. mqtt_init_reconnect()
     * re-initializes s_client.mutex every call; re-initing a mutex that
     * another thread (outbound dispatch via mqtt_publish, or a still-stuck
     * old sync thread) may be blocked on is undefined behavior and can leave
     * that thread permanently blocked. On re-start we soft-reset instead. */
    if (!s_client_inited) {
        mqtt_init_reconnect(&s_client, on_reconnect, NULL, on_publish);
        s_client_inited = true;
    } else {
        mqtt_reinit(&s_client, -1,
                    s_sendbuf, sizeof(s_sendbuf),
                    s_recvbuf, sizeof(s_recvbuf));
        /* mqtt_reinit leaves reconnect_callback/publish_response_callback
         * intact from the first init; only reset the error so the first
         * mqtt_sync triggers on_reconnect. */
        s_client.error = MQTT_ERROR_INITIAL_RECONNECT;
    }

    s_running = true;
    s_sync_thread_exited = false;

    int ret = agent_task_create(mqtt_sync_thread, "mqtt_sync",
                                   AGENT_MQTT_STACK, NULL,
                                   AGENT_MQTT_PRIO);
    if (ret != OK) {
        syslog(LOG_ERR, "[%s] Failed to create sync thread\n", TAG);
        s_running = false;
        s_sync_thread_exited = true;
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] MQTT channel started → %s:%d\n",
           TAG, s_broker_host, s_broker_port);
    return OK;
}

int mqtt_channel_send(const char *chat_id, const char *text)
{
    if (!s_running || s_sockfd < 0 || !chat_id || !text) {
        /* 走到这里说明连 publish 都没试成。原来这条路径是**完全静默**的：
         * 通道没起来 / 套接字已经关了，调用方只看得到一个 ERROR 返回值，
         * 日志里一个字都没有。 */
        s_tx_drop++;
        syslog(LOG_WARNING,
               "[%s] send DROPPED before publish (running=%d sockfd=%d "
               "chat_id=%s text=%s) — 累计丢弃 %u\n",
               TAG, (int)s_running, s_sockfd,
               chat_id ? "ok" : "NULL", text ? "ok" : "NULL",
               (unsigned)s_tx_drop);
        return ERROR;
    }

    /* Build JSON: {"type":"response","content":"...","chat_id":"..."} */
    cJSON *resp = cJSON_CreateObject();

    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json = cJSON_PrintUnformatted(resp);

    cJSON_Delete(resp);
    if (!json) {
        return ERROR;
    }

    enum MQTTErrors err = mqtt_publish(&s_client, s_topic_out,
                                       json, strlen(json),
                                       MQTT_PUBLISH_QOS_1);
    if (err != MQTT_OK) {
        s_tx_fail++;
        s_last_err = (int)err;

        /* 把 MQTT-C 自己的内部状态一起打出来 —— 光有错误码看不出是
         * 「缓冲满了」「半截发送卡住」还是「连接已经废了」。 */
        syslog(LOG_ERR,
               "[%s] Publish failed: err=%d (累计成功 %u / 失败 %u / 丢弃 %u) "
               "client.error=%d send_offset=%u\n",
               TAG, (int)err,
               (unsigned)s_tx_ok, (unsigned)s_tx_fail, (unsigned)s_tx_drop,
               (int)s_client.error, (unsigned)s_client.send_offset);
        free(json);
        return ERROR;
    }

    s_tx_ok++;
    s_last_ok_sec   = time(NULL);
    s_silent_warned = false;   /* 又发出去了，静默告警复位 */

    syslog(LOG_INFO, "[%s] Published to %s (%d bytes)\n",
           TAG, s_topic_out, (int)strlen(json));
    free(json);
    return OK;
}

void mqtt_channel_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;

    /* Close socket first to unblock any blocking recv in sync thread */
    if (s_sockfd >= 0) {
        close(s_sockfd);
        s_sockfd = -1;
    }

    /* Wait for the sync thread to actually exit before returning. Without
     * this, a fast stop→start (e.g. `set_mqtt`) leaves the old thread alive:
     * start() flips s_running back to true and creates a second thread, and
     * the two threads CONNECT with the same client_id → the broker keeps
     * kicking one off → endless reconnect loop (MQTT_ERROR_SOCKET_ERROR). */
    for (int i = 0; i < 20 && !s_sync_thread_exited; i++) {
        usleep(100 * 1000);
    }

    /* 停掉时如果故事记录还没发完，把缓冲释放掉，并清掉灯带的「正在
     * 回传」位；否则 sync 线程没了，灯带会一直呼吸下去。 */
    pthread_mutex_lock(&s_hist_lock);
    if (s_hist_buf != NULL) {
        syslog(LOG_INFO, "[%s] history: 停用时还有 %d/%d 片没发完，丢弃\n",
               TAG, s_hist_seq, s_hist_total);
        mqtt_hist_led(0);
        free(s_hist_buf);
        s_hist_buf = NULL;
        s_hist_retry = 0;
    }
    pthread_mutex_unlock(&s_hist_lock);

    syslog(LOG_INFO, "[%s] MQTT channel stopped\n", TAG);
}
