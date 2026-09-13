/****************************************************************************
 * vendor/allwinnertech/apps/kid_buddy/kid_buddy.c
 * AI Role-Playing Companion for children (6-12 years).
 *
 * Features:
 *   - 4 AI characters: Teacher, Storyteller, Scientist, Friend
 *   - Touch to switch roles on ILI9341 LCD (240×320)
 *   - LLM-powered conversation via ai_agent / velaclaw client
 *   - Each role has distinct personality and knowledge domain
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/boardctl.h>
#include <lvgl/lvgl.h>
#include <velaclaw/client.h>
#include "voice/voice_channel.h"
#include "voice/voice_asr.h"
#include "voice/audio_capture.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef NEED_BOARDINIT
#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/* Screen dimensions (ILI9341 portrait: 240×320) */
#define SCR_W  240
#define SCR_H  320

/* Number of AI roles */
#define ROLE_COUNT  4

/* Maximum message display length */
#define MSG_BUF_LEN  1024

/* Wake-word listening (MiMo ASR) parameters. Audio is captured at
 * 16kHz/16-bit/mono and VAD uses a short-time mean-square energy threshold
 * (computed over 20 ms chunks). The wake loop opens the mic only when no
 * reply is playing, detects one utterance, and hands the recognized command
 * (with the wake word stripped) to the LLM. */
#define WAKE_SAMPLE_RATE     16000
#define WAKE_CHUNK_BYTES     640          /* 20 ms @ 16kHz/16-bit mono */
#define WAKE_START_MSQ       200000       /* mean-square: speech onset */
#define WAKE_END_MSQ         80000        /* mean-square: speech offset (hysteresis) */
#define WAKE_SILENCE_CHUNKS  40           /* 40 × 20 ms = 800 ms silence ends an utterance */
#define WAKE_MAX_BYTES       (8 * 32000)  /* cap a single utterance at 8 s */
/* Contest rule: the wake word is fixed to "你好，openvela" / "Hello，openvela".
 * Detection here is a match on the MiMo ASR transcript, not a local KWS, and
 * ASR renders a mixed Chinese/English phrase inconsistently -- spacing, case
 * and punctuation all vary, and "openvela" sometimes comes back as a phonetic
 * transliteration. So we match the alias list below against a normalized view
 * of the transcript (case folded; anything that is not a letter, digit or CJK
 * ideograph treated as a separator) rather than one literal string. Keep the
 * list tight -- every alias is one more phrase that can wake the buddy by
 * accident.
 *
 * The "wake: heard ..." log line prints the raw transcript. If MiMo comes back
 * with a wording the list does not cover, add it here. */
#define WAKE_ALIASES         { "你好openvela", "helloopenvela", "哈喽openvela", \
                               "你好欧本维拉", "你好欧朋维拉", "哈喽欧本维拉" }
#define WAKE_IDLE_PROMPT     "我在呢，你想做什么？"

/* Follow-up window: after the buddy finishes a reply, accept the kid's next
 * utterance as a command for this many ms WITHOUT the wake word, so they can
 * answer a question / pick a story option without re-saying the wake word. */
#define FOLLOW_UP_MS         12000

/* After a hard ASR failure (MiMo HTTP 429 "Too many requests", network error),
 * pause the wake loop this long before listening again. Without this the loop
 * spins detect-speech→ASR→429→detect-speech→…, hammering the rate-limited
 * endpoint and keeping the board deaf to a real wake word. */
#define ASR_BACKOFF_MS       5000

/* ── Story/RPG session isolation ──────────────────────────────
 * A long-running adventure story and ordinary free chat must not share one
 * conversation history, or the plot state gets polluted by small talk (and
 * vice versa). When the kid enters story mode we key the turn under this
 * chat_id instead of the client's default app_id, so ai_agent's session_mgr
 * keeps a separate history for it. Entered by a voice trigger, left by an
 * exit phrase; the flag is persisted so a mid-story power-off resumes. */
#define STORY_CHAT_ID       "kid_buddy:story"
#define STORY_FLAG_PATH     "/data/ai_agent/kid_buddy_story.flag"

/* Idle story continuation (剧情主动续讲): if the kid walks away mid-story,
 * ask whether to continue after this much silence. The proactive turn itself
 * resets the idle clock, so this fires at most once per idle period. */
#define IDLE_STORY_MS       (60 * 1000)
#define IDLE_CHECK_MS       5000

/* UI version tag — shown in the top-right corner so you can verify at a
 * glance which build is actually running on the board. Bump this every
 * time you rebuild + reflash, then check the screen to confirm the new
 * image took effect. */
#define KID_BUDDY_VERSION  "v2.54"

/****************************************************************************
 * Types
 ****************************************************************************/

typedef enum {
    ROLE_TEACHER = 0,
    ROLE_STORYTELLER,
    ROLE_SCIENTIST,
    ROLE_FRIEND,
} role_id_t;

typedef struct {
    role_id_t    id;
    const char  *name;
    const char  *emoji;
    const char  *description;
    const char  *system_prompt;
    const char  *demo_question;
    lv_color_t   color;
} role_def_t;

/****************************************************************************
 * Role Definitions
 ****************************************************************************/

static const role_def_t g_roles[ROLE_COUNT] = {
    [ROLE_TEACHER] = {
        .id          = ROLE_TEACHER,
        .name        = "Teacher Owl",
        .emoji       = "O",
        .description = "Patient & wise, loves to explain",
        .color       = {.red = 0xe0, .green = 0x80, .blue = 0x30},
        .system_prompt =
            "你是猫头鹰老师，一位温暖、耐心的老师，面向 6-12 岁的孩子。"
            "你的语气温柔又鼓励人，你引导孩子自己发现答案，而不是直接告诉他们。"
            "科目：语文、数学、英语、科学。"
            "如果孩子想玩角色扮演、冒险游戏或听故事，你就暂时变成「故事主持人」，"
            "进入他的想象世界，陪他编故事、选剧情，不必只当老师。"
            "每次回答最后用一个鼓励性的问题收尾，保持孩子的好奇心。\n\n"
            "重要：请始终用简体中文回答。回答控制在 100 字以内，"
            "用孩子能听懂的语言。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。",
        .demo_question = "天空为什么是蓝色的？请用有趣的方式讲给我听。",
    },
    [ROLE_STORYTELLER] = {
        .id          = ROLE_STORYTELLER,
        .name        = "Story Dragon",
        .emoji       = "D",
        .description = "Vivid tales from a little dragon",
        .color       = {.red = 0x60, .green = 0xc0, .blue = 0x40},
        .system_prompt =
            "你是故事龙，一位活泼的讲故事高手，专门给 6-12 岁的孩子讲故事。"
            "你的故事色彩丰富、富有想象力。你会讲童话、寓言、成语故事，"
            "也能现场即兴编原创故事。用生动的语言和音效让故事活起来！"
            "如果孩子想玩角色扮演游戏，就让他自己选剧情，一路演下去。\n\n"
            "重要：请始终用简体中文回答。故事控制在 150 字以内，"
            "让每个角色用不同的语气活起来。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。",
        .demo_question = "给我讲一个短故事，关于一只好奇的小猫，它想像小鸟一样飞。",
    },
    [ROLE_SCIENTIST] = {
        .id          = ROLE_SCIENTIST,
        .name        = "Dr. Robot",
        .emoji       = "R",
        .description = "Curious explorer of how things work",
        .color       = {.red = 0x40, .green = 0x80, .blue = 0xe0},
        .system_prompt =
            "你是机器人博士，一位充满好奇心的科学家，喜欢探索世界运转的奥秘。"
            "你回答关于自然、动物、太空、科技和实验的问题。"
            "如果孩子想玩角色扮演、冒险游戏或听故事，你就暂时变成「故事主持人」，"
            "进入他的想象世界，陪他编故事、选剧情。"
            "你的风格是：先讲一个有趣的事实，再简单地解释。你喜欢说「你知道吗？」\n\n"
            "重要：请始终用简体中文回答。回答控制在 100 字以内，"
            "让科学像一场冒险，而不是课本。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。",
        .demo_question = "蜜蜂是怎么酿蜂蜜的？听起来好神奇！",
    },
    [ROLE_FRIEND] = {
        .id          = ROLE_FRIEND,
        .name        = "Buddy Pup",
        .emoji       = "P",
        .description = "Your cheerful playtime companion",
        .color       = {.red = 0xf0, .green = 0x80, .blue = 0xa0},
        .system_prompt =
            "你是小狗伙伴，一个开朗、爱玩的小朋友的好朋友。"
            "你和孩子聊日常生活、分享笑话、聊兴趣爱好，并且善解人意地倾听。"
            "如果孩子想玩角色扮演、冒险游戏或听故事，你就暂时变成「故事主持人」，"
            "进入他的想象世界，陪他编故事、选剧情。"
            "你的风格轻松、有趣、温暖，像最好的朋友一样。多用俏皮的文字表达，"
            "不要用 emoji 表情符号。\n\n"
            "重要：请始终用简体中文回答。回答控制在 80 字以内，保持积极向上。"
            "如果孩子看起来难过，就给予安慰和一个好玩的建议。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。",
        .demo_question = "我们现在可以一起玩什么好玩的游戏呀？",
    },
};

/****************************************************************************
 * Global State
 ****************************************************************************/

static velaclaw_client_t *g_agent_client = NULL;
static bool                g_agent_connected = false;

static role_id_t g_current_role = ROLE_TEACHER;

/* LVGL objects */
static lv_obj_t *g_scr_role_select = NULL;
static lv_obj_t *g_scr_chat = NULL;
static lv_obj_t *g_chat_role_label = NULL;
static lv_obj_t *g_chat_msg_area = NULL;
static lv_obj_t *g_chat_status = NULL;
static lv_obj_t *g_chat_spinner = NULL;

/* Message queue between LLM callback and LVGL thread.
 * The LLM callback (ai_agent/outbound-dispatch thread) writes g_msg_status /
 * g_pending_msg / g_msg_ready while the LVGL timer reads them. Guard with a
 * mutex so the status byte and the text can't be torn apart mid-read (which
 * made a "final" status pair up with a stale short fragment). */
static char    g_pending_msg[MSG_BUF_LEN];
static bool    g_msg_ready = false;
static int     g_msg_status = 0;
static pthread_mutex_t g_msg_lock = PTHREAD_MUTEX_INITIALIZER;

/* Streaming TTS state: a single worker thread speaks the whole reply in one
 * voice_channel_speak() call once the reply is complete. voice_channel_speak()
 * aborts any in-progress speak when called concurrently, so every speak must
 * go through this one thread. The pending slot is "latest wins" — a newer
 * fragment replaces an older one still waiting to be spoken. */
static pthread_mutex_t g_tts_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_tts_cond  = PTHREAD_COND_INITIALIZER;
static char     g_tts_pending[MSG_BUF_LEN];
static bool     g_tts_pending_ready = false;
static bool     g_tts_pending_final = false;
static size_t   g_spoken_len = 0;      /* bytes already spoken this response */
static unsigned int g_tts_epoch = 0;   /* bumped each new question */

/* Wake-word listening state. g_reply_busy is set when an interaction (an LLM
 * request + TTS reply, or a standalone spoken prompt) is in flight and the
 * wake loop must not open the mic; the TTS worker clears it once the final
 * reply finishes playing. A simple volatile flag is enough here — races only
 * shift the listen window by one ~20 ms chunk. */
static volatile int g_reply_busy = 0;

/* Set to 1 while the wake loop has the capture mic open (listen_one_utterance
 * holds the codec's only DMA channel the whole time it is listening). An async
 * reminder (kid_notify_cb) must wait for this to clear before its TTS playback
 * can acquire the DMA channel, otherwise the reminder shows text but is silent. */
static volatile int g_mic_open = 0;

/* Monotonic ms at which the last reply finished playing (set by the TTS worker
 * when it clears g_reply_busy). Drives the follow-up answer window: while
 * now_ms() - g_last_reply_ms < FOLLOW_UP_MS the kid may answer without the
 * wake word. 0 means "no reply yet", so no follow-up at boot. */
static volatile int64_t g_last_reply_ms = 0;

/* Story/RPG session state. g_story_mode routes turns to STORY_CHAT_ID instead
 * of the default app_id (see the STORY_CHAT_ID comment). It is a latching
 * mode: a trigger phrase in the kid's utterance turns it on, and only an exit
 * phrase turns it off — so mid-story replies ("我要钻树洞") stay in the story
 * session without needing a trigger word every turn. g_story_mode is
 * persisted to STORY_FLAG_PATH and restored on boot, so the boot-time 记忆续篇
 * probe reads the story session's history rather than free chat.
 * g_last_interaction_ms drives idle 主动续讲 (see idle_story_timer_cb). */
static volatile bool    g_story_mode = false;
static volatile int64_t g_last_interaction_ms = 0;

/* Monotonic ms until which the wake loop backs off after an ASR hard failure
 * (ret<0: HTTP 429 rate-limit / network error). See ASR_BACKOFF_MS. */
static volatile int64_t g_asr_backoff_until = 0;

/* Voice-command handoff: the wake thread writes the recognized command (wake
 * word already stripped) under g_wake_cmd_lock, and the LVGL timer thread
 * reads it and calls send_to_llm() — LVGL objects must only be touched from
 * the LVGL thread, so the wake thread never calls send_to_llm() directly. */
static char     g_wake_cmd[MSG_BUF_LEN];
static bool     g_wake_cmd_ready = false;
static pthread_mutex_t g_wake_cmd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Child-confirmation report state. When a cron reminder arrives with a
 * [KID_REPORT:...] prefix (set by cron_service.c), kid_notify_cb strips the
 * prefix and stores the report destination + reminder text here. The wake loop
 * then, while g_report_pending and inside the follow-up window, treats the
 * kid's acknowledgment ("知道了"/"好的"/…) as a confirmation and publishes a
 * report back to the parent instead of sending the words to the LLM. */
static char     g_report_channel[16];
static char     g_report_chat_id[64];
static char     g_report_reminder[MSG_BUF_LEN];
static volatile int g_report_pending = 0;

/****************************************************************************
 * Forward Declarations
 ****************************************************************************/

static void ui_show_chat_screen(void);
static void ui_show_role_select(void);
static void send_demo_question(void);
static int64_t now_ms(void);

/****************************************************************************
 * LLM Callback (called from ai_agent thread, NOT LVGL thread)
 ****************************************************************************/

static void llm_response_cb(int status, const char *response, void *cookie)
{
    (void)cookie;

    pthread_mutex_lock(&g_msg_lock);

    g_msg_status = status;
    if ((status == 0 || status == 1) && response != NULL)
      {
        /* status 0 = final reply, status 1 = streaming fragment (cumulative
         * text so far). Both carry the running reply text. */
        strncpy(g_pending_msg, response, MSG_BUF_LEN - 1);
        g_pending_msg[MSG_BUF_LEN - 1] = '\0';
      }
    else
      {
        snprintf(g_pending_msg, MSG_BUF_LEN, "[LLM error: %d]", status);
      }
    g_msg_ready = true;

    syslog(LOG_INFO, "[kid_buddy] llm_cb status=%d len=%d\n",
           status, (int)strlen(g_pending_msg));

    pthread_mutex_unlock(&g_msg_lock);
}

/* Notify callback for UNSOLICITED outbound messages (e.g. cron-fired
 * reminders) delivered while no velaclaw_ask is in flight. Route into the
 * same g_pending_msg / g_msg_ready slot that llm_response_cb uses, so the
 * existing poll_timer_cb path (emoji/markdown strip → LCD → TTS) speaks and
 * displays the reminder with zero new UI code. Partial fragments (status 1)
 * are ignored — cron reminders arrive as a single complete message. */
static void kid_notify_cb(int status, const char *msg, void *cookie)
{
    (void)cookie;

    if (msg == NULL || status == 1)
      {
        return;
      }

    /* A cron reminder may carry a [KID_REPORT:<channel>|<chat_id>] prefix
     * asking for a child-confirmation report. Strip it, remember the report
     * destination, and speak only the reminder text itself. */
    const char *text = msg;
    if (strncmp(msg, "[KID_REPORT:", 12) == 0)
      {
        const char *end = strchr(msg, ']');
        const char *sep = strchr(msg + 12, '|');
        if (end && sep && sep < end)
          {
            size_t clen = (size_t)(sep - (msg + 12));
            size_t cidlen = (size_t)(end - (sep + 1));
            if (clen < sizeof(g_report_channel)
                && cidlen < sizeof(g_report_chat_id))
              {
                pthread_mutex_lock(&g_msg_lock);
                memcpy(g_report_channel, msg + 12, clen);
                g_report_channel[clen] = '\0';
                memcpy(g_report_chat_id, sep + 1, cidlen);
                g_report_chat_id[cidlen] = '\0';
                pthread_mutex_unlock(&g_msg_lock);

                text = end + 1;
                if (*text == '\n')
                  {
                    text++;
                  }
                g_report_pending = 1;
              }
          }
      }

    /* A reminder needs to speak, but the wake loop may be holding the capture
     * mic (and the codec's shared DMA channel) right now. Raise g_reply_busy so
     * the wake loop aborts its listen and releases the mic; the TTS worker then
     * waits for g_mic_open to clear before it opens playback. */
    g_reply_busy = 1;

    /* A reminder is an independent, complete message — not a streaming fragment
     * of an in-flight LLM reply. Reset the streaming TTS offset so the TTS
     * worker speaks it from the beginning; otherwise, if the previous reply was
     * longer, len <= spoken and the reminder is silently skipped (text on LCD
     * but no voice). */
    pthread_mutex_lock(&g_tts_lock);
    g_tts_epoch++;
    g_spoken_len = 0;
    g_tts_pending_ready = false;
    g_tts_pending_final = false;
    pthread_mutex_unlock(&g_tts_lock);

    /* The confirmation window only opens once the reminder finishes playing
     * (the TTS worker arms g_last_reply_ms). Clear any stale arm from a prior
     * reply so the kid must acknowledge *after* hearing the reminder. */
    g_last_reply_ms = 0;

    pthread_mutex_lock(&g_msg_lock);
    g_msg_status = 0;
    strncpy(g_pending_msg, text, MSG_BUF_LEN - 1);
    g_pending_msg[MSG_BUF_LEN - 1] = '\0';
    if (g_report_pending)
      {
        strncpy(g_report_reminder, text, MSG_BUF_LEN - 1);
        g_report_reminder[MSG_BUF_LEN - 1] = '\0';
      }
    g_msg_ready = true;
    pthread_mutex_unlock(&g_msg_lock);

    syslog(LOG_INFO, "[kid_buddy] notify_cb len=%d report=%d\n",
           (int)strlen(text), g_report_pending);
}

/****************************************************************************
 * Story/RPG session mode
 *
 * The kid's utterances arrive here as ASR text, so the mode is driven by a
 * small keyword table rather than by the LLM (the chat_id has to be chosen
 * before the request is sent — the LLM cannot retroactively move a turn into
 * another session). Exit phrases are checked first so "不玩了" reliably wins
 * over a trigger word in the same sentence.
 ****************************************************************************/

static const char *const g_story_triggers[] = {
    "冒险游戏", "角色扮演", "讲故事", "讲个故事", "说个故事", "听故事",
    "玩冒险", "冒险故事", "故事龙", "剧情", "演一个", "扮演",
};

static const char *const g_story_exits[] = {
    "不玩了", "不玩啦", "结束游戏", "结束故事", "退出游戏",
    "换个话题", "到此为止", "不想玩了",
};

#define STORY_TRIGGER_COUNT \
    (sizeof(g_story_triggers) / sizeof(g_story_triggers[0]))
#define STORY_EXIT_COUNT \
    (sizeof(g_story_exits) / sizeof(g_story_exits[0]))

static bool text_has_any(const char *text, const char *const *words,
                         size_t count)
{
    for (size_t i = 0; i < count; i++)
      {
        if (strstr(text, words[i]) != NULL)
          {
            return true;
          }
      }
    return false;
}

/* Persist the story-mode flag so a power-off mid-adventure resumes in the
 * story session on the next boot (see story_mode_load). Best-effort: a
 * failed write only costs the cross-reboot resume, never the current turn. */
static void story_mode_persist(bool on)
{
    if (on)
      {
        FILE *f = fopen(STORY_FLAG_PATH, "w");
        if (f != NULL)
          {
            fputs("1\n", f);
            fclose(f);
          }
      }
    else
      {
        unlink(STORY_FLAG_PATH);
      }
}

static void story_mode_set(bool on)
{
    if (g_story_mode == on)
      {
        return;
      }

    g_story_mode = on;
    story_mode_persist(on);
    syslog(LOG_INFO, "[kid_buddy] story mode %s (chat_id=%s)\n",
           on ? "ON" : "OFF", on ? STORY_CHAT_ID : "(default)");
}

static void story_mode_load(void)
{
    FILE *f = fopen(STORY_FLAG_PATH, "r");

    if (f == NULL)
      {
        return;
      }

    int c = fgetc(f);
    fclose(f);

    if (c == '1')
      {
        g_story_mode = true;
        syslog(LOG_INFO, "[kid_buddy] story session restored from flag\n");
      }
}

/* Flip story mode according to what the kid just said. Called for every user
 * utterance before it is sent, so the triggering sentence itself already
 * lands in the story session. */
static void story_mode_note_utterance(const char *text)
{
    if (text_has_any(text, g_story_exits, STORY_EXIT_COUNT))
      {
        story_mode_set(false);
        return;
      }

    if (!g_story_mode
        && text_has_any(text, g_story_triggers, STORY_TRIGGER_COUNT))
      {
        story_mode_set(true);
      }
}

/****************************************************************************
 * Send a message to LLM with current role's system prompt
 ****************************************************************************/

static void ui_show_chat_screen(void);

static void send_to_llm(const char *user_text)
{
    /* A voice wake command can arrive before the kid has tapped a role: the
     * chat widgets (g_chat_status etc.) are created lazily in
     * ui_show_chat_screen(), so they are still NULL and lv_label_set_text()
     * below would fault.  Show the default-role chat screen first so every
     * label we touch actually exists. */
    if (g_chat_status == NULL)
      {
        ui_show_chat_screen();
      }

    if (!g_agent_connected || g_agent_client == NULL)
      {
        lv_label_set_text(g_chat_status, "Agent offline - start 'ai_agent &' first");
        return;
      }

    /* Mark an interaction in flight so the wake loop stays quiet through the
     * LLM request + TTS reply. Cleared by the TTS worker after the final
     * reply is spoken (or here if the request fails immediately). */
    g_reply_busy = 1;

    /* Enter/leave story mode before routing this turn, so the sentence that
     * triggers the adventure is itself the first story-session message. */
    story_mode_note_utterance(user_text);

    /* Any real interaction resets the idle-continuation clock. */
    g_last_interaction_ms = now_ms();

    /* Build full prompt: system prompt + user text */
    const role_def_t *role = &g_roles[g_current_role];
    char full_prompt[MSG_BUF_LEN];
    snprintf(full_prompt, MSG_BUF_LEN,
             "[SYSTEM]\n%s\n\n[USER]\n%s",
             role->system_prompt, user_text);

    velaclaw_ask_req_t req = {
        .text       = full_prompt,
        .timeout_ms = 15000,  /* 15 second timeout */
        .chat_id    = g_story_mode ? STORY_CHAT_ID : NULL
    };

    /* Start a fresh response: bump the epoch and reset the spoken offset so
     * the streamed reply is spoken from the beginning, and drop any stale
     * fragment still queued for the previous question. */
    pthread_mutex_lock(&g_tts_lock);
    g_tts_epoch++;
    g_spoken_len = 0;
    g_tts_pending_ready = false;
    g_tts_pending_final = false;
    pthread_mutex_unlock(&g_tts_lock);

    lv_label_set_text(g_chat_status, "Thinking...");
    if (g_chat_spinner)
      {
        lv_obj_clear_flag(g_chat_spinner, LV_OBJ_FLAG_HIDDEN);
      }

    int ret = velaclaw_ask(g_agent_client, &req, llm_response_cb, NULL);
    if (ret < 0)
      {
        lv_label_set_text(g_chat_status, "Failed to send message");
        if (g_chat_spinner)
          {
            lv_obj_add_flag(g_chat_spinner, LV_OBJ_FLAG_HIDDEN);
          }
        g_reply_busy = 0;  /* request failed, release the wake loop */
      }
}

/****************************************************************************
 * Send a raw prompt (no role persona) to the LLM.  Used for proactive
 * (unprompted) turns — e.g. boot-time story continuation — where the current
 * role's [SYSTEM] persona should not steer the reply.
 ****************************************************************************/

static void send_raw_to_llm(const char *user_text)
{
    if (g_chat_status == NULL)
      {
        ui_show_chat_screen();
      }

    if (!g_agent_connected || g_agent_client == NULL)
      {
        return;
      }

    g_reply_busy = 1;

    /* Proactive turns also reset the idle clock, so an idle prompt can't
     * immediately trigger another one. */
    g_last_interaction_ms = now_ms();

    char full_prompt[MSG_BUF_LEN];
    snprintf(full_prompt, MSG_BUF_LEN, "%s", user_text);

    velaclaw_ask_req_t req = {
        .text       = full_prompt,
        .timeout_ms = 15000,
        .chat_id    = g_story_mode ? STORY_CHAT_ID : NULL
    };

    /* Fresh response: bump the epoch and reset the spoken offset so the reply
     * is spoken from the beginning, and drop any stale fragment still queued. */
    pthread_mutex_lock(&g_tts_lock);
    g_tts_epoch++;
    g_spoken_len = 0;
    g_tts_pending_ready = false;
    g_tts_pending_final = false;
    pthread_mutex_unlock(&g_tts_lock);

    lv_label_set_text(g_chat_status, "Thinking...");
    if (g_chat_spinner)
      {
        lv_obj_clear_flag(g_chat_spinner, LV_OBJ_FLAG_HIDDEN);
      }

    int ret = velaclaw_ask(g_agent_client, &req, llm_response_cb, NULL);
    if (ret < 0)
      {
        lv_label_set_text(g_chat_status, "Failed to send message");
        if (g_chat_spinner)
          {
            lv_obj_add_flag(g_chat_spinner, LV_OBJ_FLAG_HIDDEN);
          }
        g_reply_busy = 0;  /* request failed, release the wake loop */
      }
}

/****************************************************************************
 * Idle story continuation (剧情主动续讲 / 上下文主动): the kid listened to half
 * an adventure, then walked away. Once the story has been quiet for
 * IDLE_STORY_MS, proactively ask whether to continue — the same "主动" story
 * the boot-time probe tells, but mid-session. Runs on the LVGL thread; the
 * proactive turn resets g_last_interaction_ms (in send_raw_to_llm), so this
 * fires at most once per idle period and never spams.
 ****************************************************************************/

static void idle_story_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!g_story_mode || g_reply_busy || !g_agent_connected
        || g_agent_client == NULL || g_wake_cmd_ready)
      {
        return;
      }

    if (g_last_interaction_ms == 0
        || now_ms() - g_last_interaction_ms < IDLE_STORY_MS)
      {
        return;
      }

    syslog(LOG_INFO, "[kid_buddy] story idle %lldms, prompting continuation\n",
           (long long)(now_ms() - g_last_interaction_ms));

    send_raw_to_llm(
        "（这是剧情空闲时的主动搭话，请先看一眼我们的对话历史，不要重新讲故事的开头。"
        "如果刚才的冒险正讲到一半，就用一两句话主动问小朋友：还想继续冒险吗？"
        "并给出「继续」和「结束」两个选择。如果故事已经讲完了，就夸夸他讲得好。）");
}

/****************************************************************************
 * Boot-time proactive continuation (记忆续篇 / 上下文主动): a few seconds
 * after startup, ask the LLM to check history and either resume an unfinished
 * story/RPG or greet the kid. One-shot — deletes itself after firing.
 ****************************************************************************/

static void proactive_timer_cb(lv_timer_t *timer)
{
    static int ticks = 0;

    if (++ticks < 6)
      {
        return;  /* wait ~6s for UI + agent + network to settle */
      }

    lv_timer_delete(timer);

    if (!g_agent_connected || g_agent_client == NULL)
      {
        return;
      }

    if (g_reply_busy)
      {
        return;  /* kid already interacting — don't interrupt */
      }

    send_raw_to_llm(
        "（这是开机后的主动检查，请查看我们的对话历史。"
        "如果之前有讲到一半的冒险故事或游戏，就主动对小朋友说："
        "上次的冒险讲到一半啦，还要继续吗？并给出「继续」和「重新开始」两个选择。"
        "如果没有未完成的故事，就简单打个招呼：小朋友来啦，今天想玩什么呀？）");
}

/****************************************************************************
 * Demo question based on current role
 ****************************************************************************/

static void send_demo_question(void)
{
    const role_def_t *role = &g_roles[g_current_role];
    send_to_llm(role->demo_question);
}

/****************************************************************************
 * LVGL Timer: poll for LLM responses and update UI
 ****************************************************************************/

/* ── Streaming TTS worker ─────────────────────────────────────
 * Speak a response aloud in a background thread so the LVGL UI stays
 * responsive while TTS synthesizes and plays. A single worker thread
 * serializes every speak (voice_channel_speak aborts any in-progress speak
 * on a concurrent call). The worker does NOT speak partial fragments as they
 * stream: it waits for the final fragment and speaks the whole reply in one
 * voice_channel_speak() call. This keeps a single ALSA open/close cycle and
 * lets voice_channel_speak() buffer the full reply for continuous playback. */

static void *tts_stream_worker(void *arg)
{
    (void)arg;

    for (;;)
      {
        char text[MSG_BUF_LEN];
        bool final = false;
        unsigned int epoch;
        size_t spoken;

        pthread_mutex_lock(&g_tts_lock);
        while (!g_tts_pending_ready)
          {
            pthread_cond_wait(&g_tts_cond, &g_tts_lock);
          }
        strncpy(text, g_tts_pending, MSG_BUF_LEN - 1);
        text[MSG_BUF_LEN - 1] = '\0';
        final = g_tts_pending_final;
        epoch = g_tts_epoch;
        spoken = g_spoken_len;
        g_tts_pending_ready = false;
        g_tts_pending_final = false;
        pthread_mutex_unlock(&g_tts_lock);

        size_t len = strlen(text);

        if (spoken > len)
          {
            spoken = len; /* offset reset under us mid-speak */
          }

        /* Speak the whole reply in one voice_channel_speak() call, and only
         * once the reply is complete (final). Speaking sentence-by-sentence
         * as the text streamed made the worker open/close the ALSA codec
         * once per fragment; the codec's external-speaker PA (gpio_spk) is
         * armed on the first open but not reliably re-armed on a later open
         * in the same response, so only the first sentence was audible. A
         * single open/prepare/write/close cycle is the sequence the codec
         * plays reliably, and voice_channel_speak() already buffers the full
         * reply (fetch-then-play) so the story reads continuously. We trade
         * a later first word for complete, continuous speech — the priority
         * for a child's toy. */
        if (!final)
          {
            continue;
          }

        syslog(LOG_INFO,
               "[kid_buddy] tts_worker final=1 len=%d speak_whole\n",
               (int)len);

        if (len > spoken)
          {
            g_reply_busy = 1;
            /* A reminder can arrive while the wake loop still holds the mic
             * (and the codec's shared DMA channel). Wait for it to release the
             * mic before opening playback, else the reminder is silent. */
            for (int i = 0; i < 100 && g_mic_open; i++)
              {
                usleep(10 * 1000);  /* 10 ms */
              }
            voice_channel_speak(text);
            g_reply_busy = 0;   /* final reply spoken, wake loop may listen */
            g_last_reply_ms = now_ms();  /* arm the follow-up answer window */

            pthread_mutex_lock(&g_tts_lock);
            /* Advance only if no newer question reset the offset mid-speak. */
            if (epoch == g_tts_epoch)
              {
                g_spoken_len = len;
              }
            pthread_mutex_unlock(&g_tts_lock);
          }
      }

    return NULL;
}

/* Enqueue the latest cumulative reply text for speaking. "latest wins" — a
 * newer fragment replaces an older one still waiting in the slot. */
static void enqueue_tts(const char *text, bool final)
{
    pthread_mutex_lock(&g_tts_lock);
    strncpy(g_tts_pending, text, MSG_BUF_LEN - 1);
    g_tts_pending[MSG_BUF_LEN - 1] = '\0';
    g_tts_pending_final = final;
    g_tts_pending_ready = true;
    syslog(LOG_INFO, "[kid_buddy] enqueue_tts final=%d len=%d\n",
           (int)final, (int)strlen(g_tts_pending));
    pthread_cond_signal(&g_tts_cond);
    pthread_mutex_unlock(&g_tts_lock);
}

/* ── Wake-word listening (MiMo ASR + local VAD) ────────────────
 * The wake loop runs in its own thread, opens the onboard mic only when no
 * reply is playing (AEC is disabled, so the mic must not hear the speaker's
 * own TTS), buffers one utterance using an energy-threshold VAD, sends it to
 * MiMo ASR, matches the wake word, and hands the remainder to the LLM. */

/* Characters to strip from either end of a recognized command. */
static const char *const g_junk[] = {
    " ", "\t", "\n", "\r",
    ",", ".", "!", "?", ";", ":", "~", "\"", "'",
    "，", "。", "！", "？", "、", "：", "；", "～", "…", "“", "”",
    NULL
};

static int junk_prefix_len(const char *p)
{
    for (int i = 0; g_junk[i]; i++) {
        size_t n = strlen(g_junk[i]);
        if (strncmp(p, g_junk[i], n) == 0) {
            return (int)n;
        }
    }
    return 0;
}

static int junk_suffix_len(const char *p, size_t len)
{
    for (int i = 0; g_junk[i]; i++) {
        size_t n = strlen(g_junk[i]);
        if (n <= len && strncmp(p + len - n, g_junk[i], n) == 0) {
            return (int)n;
        }
    }
    return 0;
}

/* Strip whitespace + punctuation from both ends of a command in place. */
static void trim_junk(char *s)
{
    char *start = s;
    int n;

    while (*start && (n = junk_prefix_len(start)) != 0) {
        start += n;
    }

    size_t len = strlen(start);
    while (len > 0 && (n = junk_suffix_len(start, len)) != 0) {
        start[len - n] = '\0';
        len -= n;
    }

    if (start != s) {
        memmove(s, start, len + 1);
    }
}

/* ── Wake-word matching ────────────────────────────────────────
 * The phrase is matched case-, space- and punctuation-insensitively, so any of
 * "你好，openvela" / "你好 openvela" / "Hello, OpenVela" / a phonetic "你好欧本
 * 维拉" wakes the buddy. Nothing else in the transcript is altered -- the
 * command handed to the LLM keeps its own punctuation. */

static const char *const g_wake_aliases[] = WAKE_ALIASES;

/* Decode one UTF-8 code point. Returns the bytes consumed (at least 1); an
 * invalid sequence yields 0xffffffff, which no alias can equal. */
static int utf8_decode(const char *p, unsigned int *cp)
{
    const unsigned char *u = (const unsigned char *)p;

    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
        *cp = ((unsigned int)(u[0] & 0x1f) << 6) | (u[1] & 0x3f);
        return 2;
    }
    if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80
        && (u[2] & 0xc0) == 0x80) {
        *cp = ((unsigned int)(u[0] & 0x0f) << 12)
              | ((unsigned int)(u[1] & 0x3f) << 6) | (u[2] & 0x3f);
        return 3;
    }
    if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80
        && (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80) {
        *cp = 0xffffffff;  /* astral plane: nothing here matches on it */
        return 4;
    }
    *cp = 0xffffffff;
    return 1;
}

/* The code points that survive normalization: ASCII letters and digits, and
 * CJK unified ideographs. Everything else -- space, punctuation, full-width
 * forms, emoji -- is a separator and is skipped while matching. */
static bool wake_kept(unsigned int cp)
{
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z')
        || (cp >= 'a' && cp <= 'z') || (cp >= 0x4e00 && cp <= 0x9fff);
}

static unsigned int wake_fold(unsigned int cp)
{
    return (cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp;
}

/* Does the transcript at p start with alias (already in normalized form, i.e.
 * lowercase and free of separators)? Separators in the transcript are skipped.
 * On success *end is left just past the last byte that was matched. */
static bool wake_match_at(const char *p, const char *alias, const char **end)
{
    const char *last = NULL;

    for (const char *a = alias; *a; ) {
        unsigned int acp, tcp;

        a += utf8_decode(a, &acp);

        do {
            if (*p == '\0') {
                return false;
            }
            p += utf8_decode(p, &tcp);
        } while (!wake_kept(tcp));

        if (wake_fold(tcp) != acp) {
            return false;
        }

        last = p;
    }

    *end = last;
    return true;
}

/* Locate the wake phrase in the transcript. On a hit *start / *end bound it in
 * the ORIGINAL text, so the caller can cut exactly that span and leave the rest
 * of the utterance -- punctuation included -- for the LLM. */
static bool find_wake_word(const char *text, const char **start,
                           const char **end)
{
    int naliases = (int)(sizeof(g_wake_aliases) / sizeof(g_wake_aliases[0]));
    unsigned int cp;

    for (const char *p = text; *p; p += utf8_decode(p, &cp)) {
        for (int i = 0; i < naliases; i++) {
            if (wake_match_at(p, g_wake_aliases[i], end)) {
                *start = p;
                return true;
            }
        }
    }

    return false;
}

/* Cut the wake phrase out of text, then trim the punctuation left at the seam.
 * Returns the length of the remaining command (0 = wake word only). */
static size_t strip_wake_word(const char *text, char *out, size_t cap)
{
    const char *ws, *we;

    if (find_wake_word(text, &ws, &we)) {
        size_t olen = (size_t)(ws - text);
        size_t tail;

        if (olen > cap - 1) {
            olen = cap - 1;
        }
        memcpy(out, text, olen);

        tail = strlen(we);
        if (tail > cap - 1 - olen) {
            tail = cap - 1 - olen;
        }
        memcpy(out + olen, we, tail);
        out[olen + tail] = '\0';
    } else {
        snprintf(out, cap, "%s", text);
    }

    trim_junk(out);
    return strlen(out);
}

/* Case-insensitive compare for the ASCII filler list. */
static bool ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* A bare backchannel / acknowledgment ("嗯", "啊", "Yeah", …) is not a command —
 * it's the kid murmuring, or the mic picking up the tail of the TTS / ambient
 * noise and MiMo ASR transcribing it as a filler. Sending it to the LLM during
 * the follow-up window makes the buddy answer itself and keep talking, so drop
 * these outright. Only pure fillers are filtered; real short answers like
 * "好"/"要"/"1" (a choice) still pass through. */
static bool is_filler_utterance(const char *s)
{
    static const char *const fillers[] = {
        "嗯",   "嗯嗯", "嗯嗯嗯", "啊",   "啊嗯", "哦",   "哦哦",
        "呃",   "额",   "诶",   "喂",   "哟",   "噢",   "唔",
        "嘿",   "哈",   "哈哈", "呵呵", "嘿嘿", "唉",   "哼",
        NULL
    };
    /* MiMo ASR occasionally transcribes a noise burst as an English
     * backchannel word ("Yeah.", "Okay", "Uh") — treat those as filler too. */
    static const char *const en_fillers[] = {
        "yeah", "yes", "yep", "ok", "okay", "oh", "uh", "um", "umm",
        "hmm", "hmmm", "ah", "eh", "hey", "hi", "hello", "well", "so",
        "mhm", "uhhuh", "uh-huh", "huh", "no", NULL
    };

    for (int i = 0; fillers[i] != NULL; i++) {
        if (strcmp(s, fillers[i]) == 0) {
            return true;
        }
    }
    for (int i = 0; en_fillers[i] != NULL; i++) {
        if (ascii_ieq(s, en_fillers[i])) {
            return true;
        }
    }
    return false;
}

/* A child-confirmation acknowledgment — a short affirmative meaning "I did the
 * thing" — for a pending reminder report. Only exact matches qualify, so a
 * longer answer ("知道了，但我不想喝") still routes to the LLM through the
 * normal command path rather than being swallowed as a bare confirmation. */
static bool is_confirm_utterance(const char *s)
{
    static const char *const confirms[] = {
        "知道了", "知道啦", "好的", "好", "好的吧", "收到",
        "完成", "做完了", "做完啦", "做好了", "搞定", "行",
        "可以", "好呀", NULL
    };

    for (int i = 0; confirms[i] != NULL; i++) {
        if (strcmp(s, confirms[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Publish the child-confirmation report back to the parent and clear the
 * pending-report state. Called from the wake loop once the kid acknowledges a
 * reminder inside the follow-up window. */
static void report_child_confirmation(void)
{
    char channel[16];
    char chat_id[64];
    char report_text[MSG_BUF_LEN + 64];  /* prefix + reminder text, no truncation */

    pthread_mutex_lock(&g_msg_lock);
    strncpy(channel, g_report_channel, sizeof(channel) - 1);
    channel[sizeof(channel) - 1] = '\0';
    strncpy(chat_id, g_report_chat_id, sizeof(chat_id) - 1);
    chat_id[sizeof(chat_id) - 1] = '\0';
    snprintf(report_text, sizeof(report_text),
             "孩子已确认提醒：「%s」", g_report_reminder);
    pthread_mutex_unlock(&g_msg_lock);

    syslog(LOG_INFO, "[kid_buddy] report confirm -> %s:%s\n",
           channel, chat_id);

    if (g_agent_client && channel[0] != '\0') {
        velaclaw_publish(g_agent_client, channel, chat_id, report_text);
    }

    g_report_pending = 0;
}

/* Monotonic clock in milliseconds (CLOCK_MONOTONIC). Used for the follow-up
 * answer window so a kid can answer a question without re-saying the wake word. */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Listen for a single utterance and return its ASR text (heap-allocated).
 * Returns NULL if nothing was heard or ASR failed; caller free()s the result.
 * The mic is opened fresh each call so it is only live while we are actually
 * waiting for speech (never during TTS playback).
 *
 * timeout_ms > 0 bounds the listen: if the kid hasn't started speaking within
 * timeout_ms (used for the follow-up answer window), give up and return NULL.
 * timeout_ms == 0 waits indefinitely for speech (normal wake-word listening). */
static char *listen_one_utterance(int timeout_ms)
{
    /* Never open the mic while a reply is playing: capture and playback share
     * the codec's DMA engine, and opening the mic holds a DMA channel that the
     * TTS playback then fails to acquire ("request dma chan failed").  Check
     * BEFORE opening — not just inside the read loop — so the mic is not even
     * opened (and no DMA channel is grabbed) during a reply. */
    if (g_reply_busy)
      {
        return NULL;
      }

    audio_capture_t *cap = audio_capture_open(NULL, WAKE_SAMPLE_RATE, 1, 16);
    if (!cap) {
        syslog(LOG_ERR, "[kid_buddy] wake: capture open failed\n");
        return NULL;
    }

    if (audio_capture_start(cap) < 0) {
        syslog(LOG_ERR, "[kid_buddy] wake: capture start failed\n");
        audio_capture_close(cap);
        return NULL;
    }
    g_mic_open = 1;

    unsigned char *pcm = malloc(WAKE_MAX_BYTES);
    if (!pcm) {
        g_mic_open = 0;
        audio_capture_close(cap);
        return NULL;
    }

    size_t pcm_len = 0;
    bool started = false;
    int silence_run = 0;
    int64_t deadline = (timeout_ms > 0) ? (now_ms() + timeout_ms) : 0;

    syslog(LOG_INFO, "[kid_buddy] wake: listening...\n");

    while (pcm_len < WAKE_MAX_BYTES) {
        /* A touch-triggered interaction may have started while we listened. */
        if (g_reply_busy) {
            syslog(LOG_INFO, "[kid_buddy] wake: reply busy, abort listen\n");
            break;
        }

        /* Follow-up window closed: if speech hasn't started yet, stop waiting. */
        if (!started && deadline > 0 && now_ms() >= deadline) {
            syslog(LOG_INFO, "[kid_buddy] wake: follow-up timeout\n");
            break;
        }

        int16_t chunk[WAKE_CHUNK_BYTES / 2];
        int n = audio_capture_read(cap, chunk, sizeof(chunk));
        if (n <= 0) {
            if (n < 0) {
                syslog(LOG_WARNING, "[kid_buddy] wake: read=%d\n", n);
                break;
            }
            continue;  /* EAGAIN / zero-length */
        }

        int nsamp = n / (int)sizeof(int16_t);
        int64_t sum = 0;
        for (int i = 0; i < nsamp; i++) {
            int32_t v = chunk[i];
            sum += (int64_t)v * v;
        }
        int64_t ms = nsamp > 0 ? sum / nsamp : 0;

        if (!started && ms >= WAKE_START_MSQ) {
            started = true;
            silence_run = 0;
            syslog(LOG_INFO, "[kid_buddy] wake: speech start (msq=%lld)\n",
                   (long long)ms);
        }

        if (started) {
            if (pcm_len + (size_t)n <= WAKE_MAX_BYTES) {
                memcpy(pcm + pcm_len, chunk, n);
                pcm_len += n;
            }

            if (ms < WAKE_END_MSQ) {
                if (++silence_run >= WAKE_SILENCE_CHUNKS) {
                    syslog(LOG_INFO, "[kid_buddy] wake: speech end (%zu bytes)\n",
                           pcm_len);
                    break;
                }
            } else {
                silence_run = 0;
            }
        }
    }

    audio_capture_abort(cap);
    audio_capture_close(cap);
    g_mic_open = 0;

    if (!started || pcm_len < WAKE_CHUNK_BYTES) {
        syslog(LOG_INFO, "[kid_buddy] wake: no speech (%zu bytes)\n", pcm_len);
        free(pcm);
        return NULL;
    }

    char text[MSG_BUF_LEN];
    int ret = voice_asr_recognize(pcm, pcm_len, text, sizeof(text));
    free(pcm);

    if (ret < 0 || text[0] == '\0') {
        syslog(LOG_WARNING, "[kid_buddy] wake: ASR failed/empty (ret=%d)\n",
               ret);
        /* A hard ASR error (HTTP 429 rate-limit, network) means retrying
         * immediately just re-fails and hammers MiMo. Back off so the wake
         * loop pauses before its next listen. */
        if (ret < 0) {
            g_asr_backoff_until = now_ms() + ASR_BACKOFF_MS;
        }
        return NULL;
    }

    return strdup(text);
}

/* Speak a standalone prompt (e.g. "我在呢") through the TTS worker so it is
 * serialized with reply speech, and mark the reply busy so the wake loop
 * stays quiet while the prompt plays. */
static void wake_speak_prompt(const char *text)
{
    pthread_mutex_lock(&g_tts_lock);
    g_tts_epoch++;
    g_spoken_len = 0;
    g_tts_pending_ready = false;
    g_tts_pending_final = false;
    pthread_mutex_unlock(&g_tts_lock);

    g_reply_busy = 1;
    enqueue_tts(text, true);
}

static void *wake_listen_worker(void *arg)
{
    (void)arg;

    /* Settle briefly so ai_agent finishes registering the ASR backends. */
    usleep(2 * 1000 * 1000);

    for (;;) {
        /* Wait for any in-flight reply to finish, then a short cooldown so
         * the tail of the TTS doesn't bleed into the mic. */
        while (g_reply_busy) {
            usleep(100 * 1000);
        }
        usleep(300 * 1000);

        /* Back off after a recent ASR rate-limit/network failure rather than
         * immediately re-listening and re-failing on the same endpoint. */
        while (g_asr_backoff_until && now_ms() < g_asr_backoff_until) {
            usleep(200 * 1000);
        }

        /* Follow-up window: for a short while after the buddy finishes a
         * reply, accept the kid's next utterance as a command even without
         * the wake word (so they can pick a story option by just answering,
         * not by re-saying the wake word). */
        int64_t elapsed = now_ms() - g_last_reply_ms;
        bool follow_up = (g_last_reply_ms != 0 && elapsed < FOLLOW_UP_MS);
        int timeout_ms = follow_up ? (int)(FOLLOW_UP_MS - elapsed) : 0;

        char *asr_text = listen_one_utterance(timeout_ms);
        if (!asr_text) {
            continue;
        }

        syslog(LOG_INFO, "[kid_buddy] wake: heard \"%s\"%s\n", asr_text,
               follow_up ? " (follow-up)" : "");

        const char *ws, *we;
        bool has_wake = find_wake_word(asr_text, &ws, &we);
        char command[MSG_BUF_LEN];
        size_t clen = strip_wake_word(asr_text, command, sizeof(command));
        free(asr_text);

        if (!has_wake && !follow_up) {
            syslog(LOG_INFO, "[kid_buddy] wake: not the wake word, ignore\n");
            continue;
        }

        if (clen == 0) {
            if (has_wake) {
                /* Wake word only — acknowledge and keep listening. */
                syslog(LOG_INFO, "[kid_buddy] wake: wake word only, prompt\n");
                wake_speak_prompt(WAKE_IDLE_PROMPT);
            } else {
                syslog(LOG_INFO, "[kid_buddy] wake: empty follow-up, ignore\n");
            }
            continue;
        }

        /* A bare "嗯/啊/哦" is not a command — the kid murmuring or the mic
         * catching the TTS tail, not an answer. Sending it to the LLM makes
         * the buddy answer itself and repeat the story in a loop. */
        if (is_filler_utterance(command)) {
            syslog(LOG_INFO, "[kid_buddy] wake: filler \"%s\" ignored\n",
                   command);
            continue;
        }

        /* Child-confirmation report: if the last thing the buddy spoke was a
         * reminder awaiting the kid's acknowledgment, and the kid just said a
         * short affirmative inside the follow-up window, report back to the
         * parent instead of sending the words to the LLM. */
        if (g_report_pending && follow_up && is_confirm_utterance(command)) {
            syslog(LOG_INFO, "[kid_buddy] wake: confirm \"%s\"\n", command);
            report_child_confirmation();
            continue;
        }

        /* Any other real command ends the pending-confirmation context — the
         * kid moved on to a new topic. */
        g_report_pending = 0;

        /* Hand the command to the LVGL thread (which calls send_to_llm).
         * Mark busy NOW (before this loop iterates) so the wake loop re-enters
         * the g_reply_busy wait and does NOT re-open the mic — and steal the
         * codec DMA channel — before the LVGL timer calls send_to_llm().
         * send_to_llm() sets it again (idempotent) and the TTS worker clears
         * it after the reply is spoken. */
        g_reply_busy = 1;
        pthread_mutex_lock(&g_wake_cmd_lock);
        strncpy(g_wake_cmd, command, MSG_BUF_LEN - 1);
        g_wake_cmd[MSG_BUF_LEN - 1] = '\0';
        g_wake_cmd_ready = true;
        pthread_mutex_unlock(&g_wake_cmd_lock);

        syslog(LOG_INFO, "[kid_buddy] wake: command \"%s\"\n", command);
    }

    return NULL;
}

/* Strip emoji and other glyphs the LCD font can't render.  The simsun_16_cjk
 * font covers ASCII/Latin/Greek/Cyrillic/GB2312+GBK hanzi/kana/fullwidth but
 * NOT emoji (U+1F000..) or dingbats — those show as tofu boxes, and worse,
 * MiMo TTS reads them aloud as a stray "think"-like word.  Walk the UTF-8
 * string in place and drop any emoji code point (plus the variation-selector
 * and zero-width-joiner bytes that make up emoji sequences). */
static void strip_emoji(char *s)
{
    unsigned char *src = (unsigned char *)s;
    unsigned char *dst = (unsigned char *)s;

    while (*src)
      {
        unsigned int cp = 0;
        int n = 0;
        unsigned char c = *src;

        if (c < 0x80)
          {
            cp = c;
            n = 1;
          }
        else if ((c & 0xE0) == 0xC0)
          {
            cp = c & 0x1F;
            n = 2;
          }
        else if ((c & 0xF0) == 0xE0)
          {
            cp = c & 0x0F;
            n = 3;
          }
        else if ((c & 0xF8) == 0xF0)
          {
            cp = c & 0x07;
            n = 4;
          }
        else
          {
            /* Invalid UTF-8 lead byte — drop it. */
            src++;
            continue;
          }

        int ok = 1;
        for (int i = 1; i < n; i++)
          {
            unsigned char cc = src[i];
            if ((cc & 0xC0) != 0x80)
              {
                ok = 0;
                break;
              }
            cp = (cp << 6) | (cc & 0x3F);
          }
        if (!ok)
          {
            src++;
            continue;
          }

        int emoji =
            (cp >= 0x1F000 && cp <= 0x1FAFF) ||   /* emoji + pictographs */
            (cp >= 0x2600 && cp <= 0x27BF) ||     /* misc symbols + dingbats */
            (cp >= 0x2B00 && cp <= 0x2BFF) ||     /* supplemental symbols */
            cp == 0x200D ||                       /* zero-width joiner */
            (cp >= 0xFE00 && cp <= 0xFE0F) ||     /* variation selectors */
            (cp >= 0x1F1E6 && cp <= 0x1F1FF);     /* regional indicator flags */

        if (emoji)
          {
            src += n;
            continue;
          }

        for (int i = 0; i < n; i++)
          {
            *dst++ = *src++;
          }
      }

    *dst = '\0';
}

/* Strip Markdown formatting that MiMo habitually wraps its answer in.  The
 * LCD font renders the ASCII marks fine, but MiMo TTS reads them aloud — the
 * "---" horizontal rule is the one the user heard as a stray "think"-like
 * word.  Drop code backticks, emphasis (* _ #), horizontal rules (--- ***
 * ___), and leading list bullets, while keeping single/double hyphens so a
 * legitimate "6-12岁" survives. */
static void strip_markdown(char *s)
{
    unsigned char *src = (unsigned char *)s;
    unsigned char *dst = (unsigned char *)s;
    int at_line_start = 1;

    while (*src)
      {
        unsigned char c = *src;

        /* Backticks, emphasis markers, and header hashes — never spoken. */
        if (c == '`' || c == '*' || c == '_' || c == '#')
          {
            src++;
            continue;
          }

        /* Horizontal rule: a run of 3+ dashes (optionally spaced).  A single
         * or double dash is a real hyphen, so leave those alone. */
        if (c == '-')
          {
            const unsigned char *p = src;
            int dashes = 0;
            while (*p == '-') { dashes++; p++; }
            if (dashes >= 3)
              {
                src = (unsigned char *)p;
                continue;
              }
          }

        /* Leading list bullet "- item" — drop the marker so TTS doesn't say
         * "minus".  (The "* item" form is already gone via the '*' drop.) */
        if (at_line_start && c == '-' && src[1] == ' ')
          {
            src += 2;
            continue;
          }

        *dst++ = *src++;
        at_line_start = (c == '\n');
      }

    *dst = '\0';
}

static void poll_timer_cb(lv_timer_t *timer)
{
    static int heartbeat;

    (void)timer;

    /* Liveness probe — logs once every ~10s (200ms period × 50). If this
     * stops after a TTS speak finishes, the LVGL thread has hung (screen
     * frozen), as opposed to a touch-only stall. */
    if (++heartbeat % 50 == 0)
      {
        syslog(LOG_INFO, "[kid_buddy] LVGL heartbeat #%d\n", heartbeat);
      }

    /* Snapshot the latest LLM message under the same lock the callback uses,
     * so the status and text are read as one consistent pair (a torn read
     * could otherwise tag a stale short fragment as "final" and drop the
     * rest of the reply). */
    int  msg_status;
    char msg_text[MSG_BUF_LEN];
    bool have_msg = false;

    pthread_mutex_lock(&g_msg_lock);
    if (g_msg_ready)
      {
        g_msg_ready = false;
        msg_status = g_msg_status;
        strncpy(msg_text, g_pending_msg, MSG_BUF_LEN - 1);
        msg_text[MSG_BUF_LEN - 1] = '\0';
        have_msg = true;
      }
    pthread_mutex_unlock(&g_msg_lock);

    if (have_msg)
      {
        /* Drop emoji before display AND before TTS: the LCD font can't show
         * them (tofu boxes) and MiMo TTS reads them as a stray "think" word. */
        strip_emoji(msg_text);
        /* Drop Markdown ("---", **bold**, #headers, bullets) — TTS reads the
         * "---" rule as a stray word too. */
        strip_markdown(msg_text);

        /* Hide spinner as soon as the first text arrives. */
        if (g_chat_spinner)
          {
            lv_obj_add_flag(g_chat_spinner, LV_OBJ_FLAG_HIDDEN);
          }

        if (msg_status == 0 || msg_status == 1)
          {
            /* Show the running reply text. */
            lv_label_set_text(g_chat_msg_area, msg_text);

            /* Feed the cumulative text to the TTS worker. Partial fragments
             * (status 1) are spoken sentence-by-sentence as they complete;
             * the final fragment (status 0) flushes the remainder. */
            enqueue_tts(msg_text, msg_status == 0);

            if (msg_status == 0)
              {
                lv_label_set_text(g_chat_status, "Tap 'Ask me!' to continue");
              }
            else
              {
                lv_label_set_text(g_chat_status, "Replying...");
              }
          }
        else
          {
            lv_label_set_text(g_chat_msg_area, msg_text);
            lv_label_set_text(g_chat_status, "Error - check LLM config");
            g_reply_busy = 0;  /* LLM errored out, release the wake loop */
          }
      }

    /* Voice wake-word command handoff: the wake thread writes the command
     * (wake word already stripped), we call send_to_llm() here so all LVGL
     * access stays on this thread. */
    char wake_cmd[MSG_BUF_LEN];
    bool have_wake = false;

    pthread_mutex_lock(&g_wake_cmd_lock);
    if (g_wake_cmd_ready)
      {
        g_wake_cmd_ready = false;
        strncpy(wake_cmd, g_wake_cmd, MSG_BUF_LEN - 1);
        wake_cmd[MSG_BUF_LEN - 1] = '\0';
        have_wake = true;
      }
    pthread_mutex_unlock(&g_wake_cmd_lock);

    if (have_wake)
      {
        send_to_llm(wake_cmd);
      }
}

/****************************************************************************
 * Role selection button callback
 ****************************************************************************/

static void role_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_current_role = (role_id_t)idx;
    ui_show_chat_screen();
}

/****************************************************************************
 * Back button callback
 ****************************************************************************/

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_role_select();
}

/****************************************************************************
 * Demo question button callback
 ****************************************************************************/

static void demo_btn_cb(lv_event_t *e)
{
    (void)e;
    send_demo_question();
}

/****************************************************************************
 * Create role selection screen
 ****************************************************************************/

static void ui_create_role_select(void)
{
    g_scr_role_select = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_role_select,
                              lv_color_hex(0x1a1a2e), LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(g_scr_role_select);
    lv_label_set_text(title, "Kid Buddy");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *subtitle = lv_label_create(g_scr_role_select);
    lv_label_set_text(subtitle, "Choose your friend!");
    lv_obj_set_style_text_color(subtitle,
        lv_color_hex(0x8888aa), LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 34);

    /* Version tag (top-right) */
    lv_obj_t *ver = lv_label_create(g_scr_role_select);
    lv_label_set_text(ver, KID_BUDDY_VERSION);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x7777aa), LV_PART_MAIN);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -4, 6);

    /* Build 4 role cards */
    static const int card_h = 58;
    static const int card_margin = 6;
    int y_start = 58;

    for (int i = 0; i < ROLE_COUNT; i++)
      {
        const role_def_t *r = &g_roles[i];
        int y = y_start + i * (card_h + card_margin);

        /* Card background */
        lv_obj_t *card = lv_obj_create(g_scr_role_select);
        lv_obj_set_size(card, SCR_W - 16, card_h);
        lv_obj_set_pos(card, 8, y);
        lv_obj_set_style_bg_color(card,
            lv_color_hex(0x16213e), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(card,
            lv_color_hex(0x0f3460), LV_PART_MAIN);
        lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, role_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        /* Role emoji icon (colored circle) */
        lv_obj_t *icon = lv_obj_create(card);
        lv_obj_set_size(icon, 44, 44);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_color(icon, r->color, LV_PART_MAIN);
        lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(icon, 22, LV_PART_MAIN);

        /* Emoji label inside icon */
        lv_obj_t *icon_emoji = lv_label_create(icon);
        lv_label_set_text(icon_emoji, r->emoji);
        lv_obj_set_style_text_color(icon_emoji,
            lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(icon_emoji,
            &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(icon_emoji);

        /* Role name */
        lv_obj_t *name_label = lv_label_create(card);
        lv_label_set_text(name_label, r->name);
        lv_obj_set_style_text_color(name_label,
            lv_color_hex(0xe0e0f0), LV_PART_MAIN);
        lv_obj_set_style_text_font(name_label,
            &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 60, -8);

        /* Role description */
        lv_obj_t *desc_label = lv_label_create(card);
        lv_label_set_text(desc_label, r->description);
        lv_obj_set_style_text_color(desc_label,
            lv_color_hex(0x8888aa), LV_PART_MAIN);
        lv_obj_set_style_text_font(desc_label,
            &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(desc_label, LV_ALIGN_LEFT_MID, 60, 10);
      }

    /* Connection status bar at bottom */
    lv_obj_t *status_bar = lv_label_create(g_scr_role_select);
    if (g_agent_connected)
      {
        lv_label_set_text(status_bar, "Agent: Connected");
        lv_obj_set_style_text_color(status_bar,
            lv_color_hex(0x40c040), LV_PART_MAIN);
      }
    else
      {
        lv_label_set_text(status_bar, "Agent: Offline");
        lv_obj_set_style_text_color(status_bar,
            lv_color_hex(0xc04040), LV_PART_MAIN);
      }
    lv_obj_set_style_text_font(status_bar, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/****************************************************************************
 * Create chat screen (shown after selecting a role)
 ****************************************************************************/

static void ui_create_chat_screen(void)
{
    g_scr_chat = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_chat,
                              lv_color_hex(0x1a1a2e), LV_PART_MAIN);

    /* Top bar: back button + role name */
    lv_obj_t *top_bar = lv_obj_create(g_scr_chat);
    lv_obj_set_size(top_bar, SCR_W, 44);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar,
        lv_color_hex(0x16213e), LV_PART_MAIN);
    lv_obj_set_style_border_width(top_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(top_bar, 0, LV_PART_MAIN);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(top_bar);
    lv_obj_set_size(back_btn, 50, 30);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back_btn,
        lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "<-");
    lv_obj_set_style_text_color(back_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(back_label);

    /* Role name in top bar */
    g_chat_role_label = lv_label_create(top_bar);
    lv_obj_set_style_text_color(g_chat_role_label,
        lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_chat_role_label,
        &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(g_chat_role_label, LV_ALIGN_CENTER, 0, 0);

    /* Version tag (top-right of top bar) */
    lv_obj_t *ver = lv_label_create(top_bar);
    lv_label_set_text(ver, KID_BUDDY_VERSION);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x7788aa), LV_PART_MAIN);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(ver, LV_ALIGN_RIGHT_MID, -4, 0);

    /* Role avatar circle below top bar */
    lv_obj_t *avatar = lv_obj_create(g_scr_chat);
    lv_obj_set_size(avatar, 52, 52);
    lv_obj_align(avatar, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(avatar,
        g_roles[g_current_role].color, LV_PART_MAIN);
    lv_obj_set_style_border_width(avatar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(avatar, 26, LV_PART_MAIN);

    lv_obj_t *avatar_text = lv_label_create(avatar);
    lv_label_set_text(avatar_text, g_roles[g_current_role].emoji);
    lv_obj_set_style_text_color(avatar_text, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(avatar_text, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(avatar_text);

    /* Message display area (scrollable text) */
    g_chat_msg_area = lv_label_create(g_scr_chat);
    lv_obj_set_width(g_chat_msg_area, SCR_W - 24);
    lv_obj_set_style_text_color(g_chat_msg_area,
        lv_color_hex(0xd0d0e0), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_chat_msg_area,
        &lv_font_simsun_16_cjk, LV_PART_MAIN);
    lv_label_set_long_mode(g_chat_msg_area, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_chat_msg_area, LV_ALIGN_TOP_MID, 0, 112);
    lv_label_set_text(g_chat_msg_area,
        "Tap 'Ask me!' to start a conversation\nwith your AI friend!");

    /* Status label */
    g_chat_status = lv_label_create(g_scr_chat);
    lv_obj_set_style_text_color(g_chat_status,
        lv_color_hex(0x8888aa), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_chat_status,
        &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(g_chat_status, LV_ALIGN_BOTTOM_MID, 0, -44);

    /* Spinner (hidden by default) */
    g_chat_spinner = lv_spinner_create(g_scr_chat);
    lv_obj_set_size(g_chat_spinner, 30, 30);
    lv_obj_align(g_chat_spinner, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_add_flag(g_chat_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Demo question button */
    lv_obj_t *demo_btn = lv_btn_create(g_scr_chat);
    lv_obj_set_size(demo_btn, SCR_W - 32, 36);
    lv_obj_align(demo_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(demo_btn,
        lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_radius(demo_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(demo_btn, demo_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *demo_label = lv_label_create(demo_btn);
    lv_label_set_text(demo_label, "Ask me!");
    lv_obj_set_style_text_color(demo_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(demo_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(demo_label);
}

/****************************************************************************
 * Switch to chat screen for current role
 ****************************************************************************/

static void ui_show_chat_screen(void)
{
    if (g_scr_chat == NULL)
      {
        ui_create_chat_screen();
      }

    /* Update avatar color and labels for current role */
    const role_def_t *role = &g_roles[g_current_role];
    lv_label_set_text_fmt(g_chat_role_label, "%s  %s",
                          role->emoji, role->name);

    /* Update avatar circle color */
    lv_obj_t *avatar = lv_obj_get_child(g_scr_chat, 1); /* avatar is second child */
    if (avatar)
      {
        lv_obj_set_style_bg_color(avatar, role->color, LV_PART_MAIN);
        lv_obj_t *avatar_text = lv_obj_get_child(avatar, 0);
        if (avatar_text)
          {
            lv_label_set_text(avatar_text, role->emoji);
          }
      }

    /* Reset message area */
    lv_label_set_text(g_chat_msg_area,
        "Tap 'Ask me!' to start a conversation\nwith your AI friend!");

    lv_scr_load(g_scr_chat);
}

/****************************************************************************
 * Switch to role selection screen
 ****************************************************************************/

static void ui_show_role_select(void)
{
    if (g_scr_role_select == NULL)
      {
        ui_create_role_select();
      }
    lv_scr_load(g_scr_role_select);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;

    if (lv_is_initialized())
      {
        LV_LOG_ERROR("LVGL already initialized! aborting.");
        return -1;
      }

#ifdef NEED_BOARDINIT
    boardctl(BOARDIOC_INIT, 0);
#endif

    /* Step 1: Initialize LVGL */
    lv_init();

    /* Step 2: Configure NuttX display backend */
    lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
    info.fb_path = "/dev/lcd0";
#endif

#ifdef CONFIG_INPUT_TOUCHSCREEN
    info.input_path = "/dev/input0";
#endif

    lv_nuttx_init(&info, &result);

    /* Necessary delay for LCD initialization timing */
    usleep(100000);

    if (result.disp == NULL)
      {
        LV_LOG_ERROR("Display initialization failure!");
        return 1;
      }

    /* Step 3: Connect to ai_agent daemon */
    g_agent_client = velaclaw_client_open("kid_buddy");
    if (g_agent_client != NULL)
      {
        g_agent_connected = true;
        /* Receive unsolicited outbound messages (cron reminders pushed to the
         * local_client channel) so the buddy can speak them proactively. */
        velaclaw_set_notify_callback(g_agent_client, kid_notify_cb, NULL);
        LV_LOG_USER("Connected to AI Agent daemon.");
      }
    else
      {
        g_agent_connected = false;
        LV_LOG_WARN("AI Agent daemon not available. UI only mode.");
      }

    /* Step 4: Create UI and start on role selection */
    ui_create_role_select();
    lv_scr_load(g_scr_role_select);

    /* Step 5: Create poll timer to update UI from LLM callbacks */
    lv_timer_create(poll_timer_cb, 200, NULL);

    /* Step 5a: Restore story mode BEFORE the boot probe fires, so a board
     * that was powered off mid-adventure resumes inside the story session. */
    story_mode_load();

    /* Step 5b: Boot-time proactive continuation — one-shot check a few seconds
     * after startup that resumes an unfinished story or greets the kid. */
    lv_timer_create(proactive_timer_cb, 1000, NULL);

    /* Step 5c: Idle story continuation — periodically checks whether the kid
     * walked away mid-story and, if so, proactively offers to carry on. */
    lv_timer_create(idle_story_timer_cb, IDLE_CHECK_MS, NULL);

    /* Step 5d: Start the single TTS worker thread that speaks the streamed
     * reply sentence-by-sentence (serialized so it never aborts itself). */
    pthread_attr_t tts_attr;

    pthread_attr_init(&tts_attr);
    pthread_attr_setstacksize(&tts_attr, 65536);
    pthread_t tts_tid;

    if (pthread_create(&tts_tid, &tts_attr, tts_stream_worker, NULL) != 0)
      {
        syslog(LOG_ERR, "[kid_buddy] Failed to start TTS worker\n");
      }
    pthread_attr_destroy(&tts_attr);

    /* Step 5e: Start the wake-word listening thread. It waits for the reply
     * to finish before opening the mic, then uses local VAD + MiMo ASR to
     * detect the wake word and hand the command to the LLM. */
    pthread_attr_t wake_attr;
    pthread_t wake_tid;

    pthread_attr_init(&wake_attr);
    pthread_attr_setstacksize(&wake_attr, 65536);

    if (pthread_create(&wake_tid, &wake_attr, wake_listen_worker, NULL) != 0)
      {
        syslog(LOG_ERR, "[kid_buddy] Failed to start wake worker\n");
      }
    pthread_attr_destroy(&wake_attr);

    /* Step 6: Main loop */
    while (1)
      {
        uint32_t idle = lv_timer_handler();
        idle = idle ? idle : 1;
        usleep(idle * 1000);
      }

    return 0;
}
