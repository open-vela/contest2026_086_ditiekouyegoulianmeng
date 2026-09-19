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
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/boardctl.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <lvgl/lvgl.h>
#include <velaclaw/client.h>
#include "voice/voice_channel.h"
#include "voice/voice_asr.h"
#include "voice/audio_capture.h"
/* network_is_connected() — flat build, so the agent's infra code is in this
 * same binary and callable directly (see app/kid_buddy/Makefile, which already
 * adds packages/ai_agent/src to the include path). */
#include "infra/network_manager.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef NEED_BOARDINIT
#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/* Screen dimensions are NOT compile-time constants any more.
 *
 * The panel is an ILI9341 wired in LANDSCAPE (CONFIG_LCD_ILI9341_IFACE0_LANDSCAPE
 * → drivers/lcd/ili9341.c swaps ILI9341_XRES/YRES in getxres/getyres), so
 * /dev/lcd0 reports 320x240 and lv_nuttx_lcd passes that straight to
 * lv_display_create().  This file used to hard-code 240x320 portrait, which
 * put the 4th role card at y=314 on a 240-tall screen -- off the bottom.
 * Ask LVGL at runtime instead and derive every coordinate from it; see
 * face_w / face_h below. */
static int g_scr_w = 0;
static int g_scr_h = 0;

/* Number of AI roles */
#define ROLE_COUNT  4

/* Maximum message display length */
#define MSG_BUF_LEN  1024

/* Big enough for the subtitle panel's 3 lines x 33 cells plus an ellipsis, with
 * room to spare. One of these lives on the stack of whichever thread refreshes
 * a label -- keep it well under MSG_BUF_LEN, which the audio paths use. */
#define FACE_TEXT_BUF  256

/* The reply scrolls. FACE_SAY_ROWS_VIS is the PANEL height and it does not
 * move: the reply keeps its two visible rows, but the label behind them is
 * allowed to be taller than the window it shows through, and a timer walks it
 * upward one row at a time so the whole reply gets read.
 *
 * This replaced a hard cut at two rows with an ellipsis, which was the wrong
 * trade: the model is asked for 100 characters and 33 cells x 2 rows is only
 * 66, so most replies ended mid-sentence and the rest was unreachable.
 *
 * These four live up here rather than with the panel geometry below because
 * FACE_SAY_BUF sizes a file-scope buffer. */
#define FACE_SAY_ROWS_VIS   2   /* rows of reply visible at once */
#define FACE_SAY_ROWS_MAX   8   /* what the label is allowed to grow to */
#define FACE_SAY_BUF     1024   /* bytes; 8 full-width rows + 7 newlines */
#define FACE_SCROLL_MS   2500   /* one row per this long */

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
 * with a wording the list does not cover, add it here.
 *
 * The openai/openvilla entries come from real transcripts. The ASR request is
 * tagged language "zh" (see MIMO_ASR_LANGUAGE), so an English word that is not
 * in its Chinese lexicon gets folded onto the nearest one it does know:
 * "openvela" comes back as "OpenAI"/"openai" or "OpenVilla". Both still carry
 * the 你好 prefix, which is what keeps them from firing by accident. */
#define WAKE_ALIASES         { "你好openvela", "helloopenvela", "哈喽openvela", \
                               "你好欧本维拉", "你好欧朋维拉", "哈喽欧本维拉", \
                               "你好openai", "helloopenai", "哈喽openai", \
                               "你好openvilla", "helloopenvilla", "哈喽openvilla" }
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

/* How long g_reply_busy may go un-refreshed before poll_timer_cb releases it.
 * The kid_buddy request timeout is 15 s and the agent's own LLM timeout is
 * 60 s, so 90 s only trips when the reply really is never coming back. */
#define REPLY_BUSY_TIMEOUT_MS  (90 * 1000)

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

/* UI version tag — logged at boot so you can verify at a glance which build is
 * actually running on the board (it used to be drawn in the screen corner; the
 * face UI has no room for it -- the corner is the bookmark strip now). Bump
 * this every time you rebuild + reflash. */
#define KID_BUDDY_VERSION  "v2.67"

/* Spoken locally a few seconds after boot, before the boot-probe LLM turn.
 * Deliberately not generated by the model: with no network, or with a slow
 * first token, the child would otherwise get silence on power-up. Plain text
 * only — no quotes, no emoji, no markdown — because it does not pass through
 * strip_emoji/strip_markdown/strip_quotes the way LLM replies do. */
#define KID_BUDDY_BOOT_GREETING \
    "欢迎回来！我是你的小伙伴，想聊点什么呀？"

/* When the boot greeting is spoken (seconds, on the 1 s proactive timer) and
 * when the boot-probe LLM turn is sent. The two must stay apart: send_raw_to_llm()
 * clears the TTS pending slot, and the model's streaming fragments replace
 * whatever is in it ("latest wins"), so a greeting enqueued in the same tick
 * as the probe either never plays or gets overwritten before the TTS worker
 * picks it up. */
#define BOOT_GREETING_TICKS  4
#define BOOT_PROBE_TICKS     12

/* Spoken instead of ai_agent's own error text when a turn fails. Two variants,
 * because a turn the board started by itself failing reads very differently
 * from the kid asking something and getting nothing back. Plain text only —
 * these bypass the model, so they never pass through
 * strip_emoji/strip_markdown/strip_quotes. */
#define KID_BUDDY_PROACTIVE_FAIL_LINE \
    "我现在有点想不出来，等一下再问我好不好？"
#define KID_BUDDY_FAIL_LINE \
    "我好像连不上网了，等网络好了再问我吧。"

/* ── Face UI ──────────────────────────────────────────────────
 * The whole screen is one geometric cartoon face: no text anywhere, including
 * the LLM reply body (a child cannot read it, and this build only has a single
 * 16px CJK font anyway).  Role selection is 4 coloured "bookmark" tabs peeking
 * from the right edge; everything else is expression.
 *
 * FRAME BUDGET -- read this before adding any animation.
 * CONFIG_LV_NUTTX_LCD_BUFFER_COUNT=1 makes lv_nuttx_lcd.c pick
 * LV_DISPLAY_RENDER_MODE_FULL with a full-screen draw buffer, so ANY
 * invalidation re-renders and re-flushes the entire display: 320x240x2 =
 * 153,600 bytes over a 40 MHz SPI1 = ~31 ms of bus time per frame.  The
 * consequences drive this whole file:
 *
 *   1. Shrinking the invalidated area saves nothing. What costs is the NUMBER
 *      of animated frames per second, not how big they are.
 *   2. lv_obj_set_style_*() invalidates unconditionally, so writing a style
 *      every tick means the board never stops flushing. Every write in
 *      face_apply() is therefore guarded by a "did it actually change" test
 *      (see set_* helpers). An idle face must issue ZERO LVGL calls.
 *   3. SPI1 shares its DMA channels with audio capture/playback (see the note
 *      in rtos-hal hal/source/spi/hal_spi.c), so animating hard while TTS plays
 *      risks glitching the speech. The mouth drops to FACE_FPS_TALKING.
 *   4. Rotation and radius are expensive in specific ways; see the notes on
 *      the eyebrow and eye objects in ui_create_face(). */
#define FACE_PERIOD_MS       100   /* 10 fps: expression + idle motion */
#define FACE_PERIOD_TALK_MS  250   /* 4 fps while TTS is playing (see #3) */
#define FACE_BLINK_MIN_MS    3000  /* random blink interval */
#define FACE_BLINK_MAX_MS    6000
#define FACE_BLINK_CLOSE_MS  100   /* eyes shut this long (~1 frame at 10 fps) */
#define FACE_LOOK_MS         2800  /* idle gaze drifts on this cadence */

/* How long each transient expression holds after its trigger. */
#define FACE_HAPPY_HOLD_MS    3000  /* proactive turn finished / report sent */
#define FACE_SAD_HOLD_MS      5000  /* a turn failed -- see g_face_sad_until_ms */
#define FACE_BELL_HOLD_MS     5000  /* reminder arrived */
#define FACE_CONFUSED_HOLD_MS 2500  /* speech heard, ASR came back empty */

/* Bookmark tabs. The strip is flush with the right edge of the screen, so the
 * tabs sit in a vertical band TAPE_W wide; the selected one slides out by
 * TAPE_PULL to read as "pulled out of the book". */
#define TAPE_W        30
#define TAPE_PULL     10
#define TAPE_GAP      10
#define TAPE_MIN_H    46

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
            "用孩子能听懂的语言。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。"
            "不要输出任何引号：单引号、双引号、半角全角、直角引号「」一律不用，"
            "对话内容直接写出来，不加引号。",
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
            "让每个角色用不同的语气活起来。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。"
            "不要输出任何引号：单引号、双引号、半角全角、直角引号「」一律不用，"
            "对话内容直接写出来，不加引号。",
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
            "让科学像一场冒险，而不是课本。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。"
            "不要输出任何引号：单引号、双引号、半角全角、直角引号「」一律不用，"
            "对话内容直接写出来，不加引号。",
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
            "如果孩子看起来难过，就给予安慰和一个好玩的建议。直接输出纯文本，不要用 Markdown 格式（不要用 #、*、**、---、列表符号等）。"
            "不要输出任何引号：单引号、双引号、半角全角、直角引号「」一律不用，"
            "对话内容直接写出来，不加引号。",
        .demo_question = "我们现在可以一起玩什么好玩的游戏呀？",
    },
};

/****************************************************************************
 * Global State
 ****************************************************************************/

static velaclaw_client_t *g_agent_client = NULL;
static bool                g_agent_connected = false;

static role_id_t g_current_role = ROLE_TEACHER;

/* ── Face UI object handles ───────────────────────────────────
 * Every part of the face is created exactly once, at boot, and then only ever
 * has its style/geometry mutated. Nothing is ever deleted or created at
 * runtime -- on this target a create/delete pair drags the heap through a
 * fresh allocation for the draw buffers behind it, and on a full-render
 * display there is nothing to be gained by building widgets lazily. */
typedef struct {
    lv_obj_t *scr;             /* the screen itself is the background */
    lv_obj_t *eye_white[2];
    lv_obj_t *eye_shut[2];     /* flat bars, swapped in for a blink */
    lv_obj_t *pupil[2];
    lv_obj_t *glint[2];
    lv_obj_t *brow[2];
    lv_obj_t *mouth_arc;       /* smile / frown, swept by mouth_curve */
    lv_obj_t *mouth_line;      /* flat mouth -- an arc with start==end draws nothing */
    lv_obj_t *mouth_ring;      /* open "O", plus the talking open/close */
    lv_obj_t *dot[3];          /* thinking */
    lv_obj_t *vol_arc;         /* listening, driven by real mic energy */
    lv_obj_t *bell;            /* reminder */
    lv_obj_t *bubble[3];       /* offline / sleepy */
    lv_obj_t *hook;            /* didn't catch that */
    lv_obj_t *hand;            /* proactive greeting, rotated as a unit */
    lv_obj_t *tape[ROLE_COUNT];/* role bookmarks, right edge */
    lv_obj_t *panel;           /* subtitle plate, bottom third */
    lv_obj_t *you_lbl;         /* what the child was heard to say */
    lv_obj_t *say_win;         /* two rows tall, clips -- the reply scrolls inside */
    lv_obj_t *say_lbl;         /* the reply, up to FACE_SAY_ROWS_MAX rows */
} face_t;

static face_t g_face;
static bool   g_face_ready = false;

/* Face geometry, derived once from the real display size. */
static int g_face_cx = 0;   /* eye-line centre */
static int g_face_cy = 0;
static int g_face_d  = 0;   /* width of the eye pair */
static int g_fh      = 0;   /* height of the band the face gets -- see face_geom() */
static int g_panel_x0 = 0, g_panel_y0 = 0;   /* subtitle panel, absolute */
static int g_panel_x1 = 0, g_panel_y1 = 0;
static int g_text_cols = 0;                  /* panel width in half-width cells */

/* ── Expression state ─────────────────────────────────────────
 * g_fx_now is eased toward g_fx_target every tick; g_fx_rendered remembers
 * what is actually on screen so face_apply() can skip writing a property that
 * has not changed. That third copy is the whole performance strategy -- on a
 * full-render display an unconditional style write costs a 31 ms full-screen
 * flush (see the FRAME BUDGET note above). */
/* Which of the three mouth shapes is showing. They are separate objects rather
 * than one morphed widget on purpose: an arc swept to nothing is invisible
 * (lv_draw_sw_arc early-returns when start_angle == end_angle), and resizing a
 * rounded object every frame defeats LVGL's radius cache. Toggling hidden
 * flags between three pre-made shapes costs nothing and removes a whole class
 * of visual bugs. */
enum {
    FACE_MOUTH_ARC = 0,   /* smile / frown, swept by mouth_curve */
    FACE_MOUTH_LINE,      /* flat and unimpressed */
    FACE_MOUTH_RING,      /* open "O" -- surprise, and the talking open/close */
};

typedef struct {
    int16_t eye_open;    /* 0 = shut, 100 = normal, 140 = wide */
    int16_t pupil_dx;    /* pupil offset from the eye centre, px */
    int16_t pupil_dy;
    int16_t brow_dy;     /* + raises the brow, - drops it */
    int16_t brow_tilt;   /* left brow,  degrees; + tips the inner end DOWN */
    int16_t brow_tilt_r; /* right brow, same sense. Split from the left one so
                          * "confused" can raise a single brow, which is the
                          * only asymmetry that reads as puzzlement rather
                          * than as sadness. */
    uint8_t mouth;       /* FACE_MOUTH_* */
    int16_t mouth_curve; /* -100 frown .. +100 smile, arc mode only */
} face_expr_t;

static face_expr_t g_fx_now;
static face_expr_t g_fx_target;
static face_expr_t g_fx_rendered;

/* Bookmark colour for the currently selected role (drives the background tint
 * and the tab highlight). */
#define FACE_BG_BASE  0x141a2e   /* neutral night blue */
#define FACE_BG_STORY 0x2a2113   /* warm, when an adventure is running */

/* ── Cross-thread observation points ──────────────────────────
 * The face has to show things the LVGL thread cannot see by itself (is audio
 * playing right now? how loud is the mic? did the turn fail?). Worker threads
 * publish to these and the LVGL timer latches them, because LVGL objects may
 * only be touched from the LVGL thread. Plain ints rather than timestamps so
 * there is no torn 64-bit read on a 32-bit target. */
static volatile int g_obs_speaking = 0;       /* TTS worker: audio is playing */
static volatile int g_obs_mic_msq = 0;        /* wake loop: VAD mean-square */
static volatile int g_obs_mic_speech = 0;     /* wake loop: VAD onset fired */
static volatile int g_obs_asr_inflight = 0;   /* wake loop: inside ASR call */
static volatile int g_obs_asr_empty = 0;      /* heard speech, ASR empty */
static volatile int g_obs_turn_failed = 0;    /* agent handed back error text */
static volatile int g_obs_reply_streaming = 0;/* first text fragment landed */

/* The child's own words, last utterance, for the subtitle. Written by the wake
 * thread through asr_line_set(); read on the LVGL thread. */
static char     g_asr_line[FACE_TEXT_BUF];
static volatile bool g_asr_line_ready = false;
static pthread_mutex_t g_asr_line_lock = PTHREAD_MUTEX_INITIALIZER;

/* What each label is currently showing, and when the reply last changed -- see
 * FACE_TEXT_MIN_MS for why the timestamp exists. These are also the buffers
 * LVGL points at: both labels are set with lv_label_set_text_static(), so they
 * must be file-scope and must never be freed. */
static char    g_you_shown[FACE_TEXT_BUF];
static char    g_say_shown[FACE_SAY_BUF];
static int64_t g_say_shown_at = 0;

/* The scrolling reply: how many rows the label currently holds, which row is
 * the first visible one, and when that last moved. All LVGL-thread. */
static int     g_say_rows = 0;
static int     g_say_off = 0;
static int64_t g_say_scroll_at = 0;

/* Scratch for the wrap in face_set_text(). File-scope rather than a local:
 * one row is 'budget' full-width glyphs = 3 bytes each, so FACE_SAY_ROWS_MAX
 * rows is most of a kilobyte, and this runs on the LVGL thread's stack. */
static char    g_fit_scratch[FACE_SAY_BUF];

/* Latched hold windows, all LVGL-thread. */
static int64_t g_face_happy_until = 0;
static int64_t g_face_sad_until = 0;
static int64_t g_face_bell_until = 0;
static int64_t g_face_confused_until = 0;
static int64_t g_face_wave_until = 0;

/* LVGL-thread only: g_face_tick numbers the face_apply() calls so animations
 * can be paced off a divisor of the timer period rather than a second timer.
 * g_llm_turn marks the turns that actually asked the model -- the boot
 * greeting and the wake-word acknowledgement are spoken locally, and without
 * this flag the face would show "thinking" while the board is already
 * talking. */
static int g_face_tick = 0;
static volatile int g_llm_turn = 0;

/* Which prop is currently on screen, and its animation phase. */
typedef enum {
    PROP_NONE = 0,
    PROP_DOTS,     /* thinking */
    PROP_VOLUME,   /* listening */
    PROP_BELL,     /* reminder */
    PROP_BUBBLES,  /* offline */
    PROP_HOOK,     /* didn't catch that */
    PROP_HAND,     /* proactive greeting */
} face_prop_t;

static face_prop_t g_prop = PROP_NONE;
static face_prop_t g_prop_prev = PROP_NONE;
static int g_prop_phase = 0;
static int g_prop_dots_angle = 0;
static int g_prop_hold = 0;    /* the pause between laps of the thinking dots */

/* Blink + gaze schedulers (LVGL thread, counted down in face_apply). */
static int64_t g_blink_at = 0;
static int64_t g_blink_until = 0;
static int64_t g_look_at = 0;
static int g_look_dx = 0;
static int g_look_dy = 0;

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

/* Monotonic ms at which g_reply_busy was last (re)stamped. The watchdog in
 * poll_timer_cb compares against this: a reply that is genuinely in flight
 * keeps refreshing it (every LLM fragment, every TTS speak), so a stale stamp
 * means the reply died somewhere and the flag needs releasing by hand. */
static volatile int64_t g_busy_since_ms = 0;

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

/* Set once the local boot greeting has been queued, so the proactive timer
 * speaks it exactly once. Kept apart from the boot-probe turn: the greeting is
 * local and unconditional, the probe needs the agent connection and normally
 * runs several seconds later (see BOOT_GREETING_TICKS / BOOT_PROBE_TICKS). */
static bool g_boot_greeted = false;

/* True while the turn in flight was started by the board itself (boot-probe,
 * idle continuation) rather than by the kid. Set in send_raw_to_llm(), cleared
 * in send_to_llm(); read when a failed turn needs a fallback line, so the two
 * cases can apologise differently. */
static volatile bool g_turn_proactive = false;

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

/* Wake-word-only acknowledgement ("我在呢，你想做什么？"). Same handoff as the
 * command above, for the same reason: the wake thread cannot touch LVGL, so it
 * parks the text here and the LVGL timer puts it on the chat screen. Without
 * this the buddy answered a bare wake word with speech but left the screen
 * sitting on role selection, which reads as "it didn't hear me". */
static char     g_wake_prompt[MSG_BUF_LEN];
static bool     g_wake_prompt_ready = false;

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

static void ui_ensure_face(void);
static void face_apply(void);
static void face_set_role(role_id_t role);
static void face_text_pickup_asr(void);
static void face_text_reply(const char *text, bool final);
static void face_text_new_turn(bool clear_you);
static void face_say_scroll(void);
static int64_t now_ms(void);

/****************************************************************************
 * Reply-busy stamp
 ****************************************************************************/
/* NOTE: g_busy_since_ms lives with the other wake-word state below. */

/* Every site that raises g_reply_busy goes through here, so the watchdog in
 * poll_timer_cb can tell how long the buddy has been mute. The flag is
 * normally cleared by the TTS worker once the reply has been spoken; if that
 * never happens (the agent drops the callback, the outbound queue wedges) the
 * flag stays at 1 forever and the board goes permanently deaf AND silent --
 * which on the LCD just looks like a frozen screen, and used to need a reboot
 * to clear. The watchdog turns that into a self-recovering hiccup. */
static void busy_mark(void);

/* Defined with the TTS worker, but the boot greeting on the proactive timer
 * queues its line before that point in the file. */
static void enqueue_tts(const char *text, bool final);

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void busy_mark(void)
{
    g_busy_since_ms = now_ms();
    g_reply_busy = 1;
}

/* Hand the child's own words to the UI. Called from the wake thread; the LVGL
 * timer picks it up and is the only thing that writes to a label. Same shape as
 * g_wake_cmd just below: a plain buffer behind a lock, with a "ready" flag the
 * reader clears. No LVGL call may happen on this side of the fence. */
static void asr_line_set(const char *text)
{
    pthread_mutex_lock(&g_asr_line_lock);
    strncpy(g_asr_line, text, sizeof(g_asr_line) - 1);
    g_asr_line[sizeof(g_asr_line) - 1] = '\0';
    g_asr_line_ready = true;
    pthread_mutex_unlock(&g_asr_line_lock);
}

/****************************************************************************
 * LLM Callback (called from ai_agent thread, NOT LVGL thread)
 ****************************************************************************/

/* ai_agent answers a failed LLM round by pushing a fixed string as if it were an
 * ordinary reply (agent_loop.c dispatch_response(), plus its timeout and
 * out-of-memory paths) — it does not fail the ask. So status is 0 and the text
 * is indistinguishable from a real answer by shape; the strings themselves are
 * the only handle we have. Keep this list in sync with ai_agent's agent_loop.c.
 *
 * This is what put "Sorry, I encountered an error." on the screen on first boot
 * after a flash: the boot-probe turn goes out the moment the agent comes up and
 * the board has an IP, but the LLM call itself failed, and the raw English
 * error was displayed and read aloud to the kid. */
static const char *const g_agent_error_texts[] =
{
    "Sorry, I encountered an error.",
    "请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。",
    "任务已完成，但生成确认消息超时。",
    "系统内存不足，请稍后再试。",
};

static bool is_agent_error_text(const char *text)
{
    if (text == NULL || text[0] == '\0')
      {
        return true;   /* an empty reply is a failure too — say something */
      }

    for (size_t i = 0;
         i < sizeof(g_agent_error_texts) / sizeof(g_agent_error_texts[0]); i++)
      {
        if (strcmp(text, g_agent_error_texts[i]) == 0)
          {
            return true;
          }
      }

    return false;
}

static void llm_response_cb(int status, const char *response, void *cookie)
{
    (void)cookie;

    /* A fragment arriving is proof the reply is still alive, so push the
     * watchdog deadline out. Without this a long story (LLM + 40 s of TTS)
     * could look like a hang to poll_timer_cb. */
    g_busy_since_ms = now_ms();

    pthread_mutex_lock(&g_msg_lock);

    g_msg_status = status;
    if ((status == 0 || status == 1) && response != NULL)
      {
        /* status 0 = final reply, status 1 = streaming fragment (cumulative
         * text so far). Both carry the running reply text. */
        const char *text = response;

        /* Swap a failed round for something a child should actually hear. This
         * is the last point before the text reaches the LCD and the speaker.
         * Final messages only: a partial of a real reply passes through
         * untouched. Status stays 0, so the fallback is displayed and spoken
         * like any other reply. */
        if (status == 0 && is_agent_error_text(text))
          {
            text = g_turn_proactive ? KID_BUDDY_PROACTIVE_FAIL_LINE
                                    : KID_BUDDY_FAIL_LINE;
            /* With no text on screen, the spoken line is the only channel left
             * -- and the spoken line has its own history of going silent (TLS
             * handshakes, DMA contention). So flag it for the face too: a
             * child looking at a neutral face cannot tell a failure from
             * being ignored. */
            g_obs_turn_failed = 1;
            syslog(LOG_WARNING,
                   "[kid_buddy] agent returned an error reply (proactive=%d), "
                   "using a local fallback instead\n", (int)g_turn_proactive);
          }

        strncpy(g_pending_msg, text, MSG_BUF_LEN - 1);
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
    busy_mark();

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

static void send_to_llm(const char *via, const char *user_text)
{
    /* A voice wake command can arrive before anything has touched the UI.
     * ui_ensure_face() is idempotent, so calling it here just guarantees the
     * face exists before face_apply() starts reading state into it. */
    ui_ensure_face();

    if (!g_agent_connected || g_agent_client == NULL)
      {
        /* Nothing was sent and nothing will be spoken. There is no text to
         * explain that any more -- the face shows the offline expression on
         * its own (see face_apply). */
        return;
      }

    /* Mark an interaction in flight so the wake loop stays quiet through the
     * LLM request + TTS reply. Cleared by the TTS worker after the final
     * reply is spoken (or here if the request fails immediately). */
    busy_mark();

    /* The kid asked this one — a failure deserves an apology, not the softer
     * "I can't think of anything right now" line. */
    g_turn_proactive = false;

    /* New turn: no text has arrived yet, so the face starts on the sweeping
     * "thinking" prop rather than the "text is coming" one. */
    g_obs_reply_streaming = 0;
    g_llm_turn = 1;             /* this turn really does ask the model */

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

    /* Clear the reply line for the new turn. The child's line stays: the wake
     * loop set it moments ago and this turn is the answer to it. */
    face_text_new_turn(false);

    /* No expression update here: the face reads g_reply_busy for "thinking" and
     * g_obs_reply_streaming for "text is arriving". */

    /* Tag who asked. The agent log prints "Processing message from
     * local_client:kid_buddy:..." for every one of these, and nobody else uses
     * that channel -- so during a freeze the only open question is WHICH of our
     * senders fired. This line answers it without guessing. */
    syslog(LOG_INFO, "[kid_buddy] llm ask via=%s story=%d \"%.48s\"\n",
           via, g_story_mode, user_text);

    int ret = velaclaw_ask(g_agent_client, &req, llm_response_cb, NULL);
    if (ret < 0)
      {
        /* The request never left the board: no reply, no speech, and with the
         * UI text-free no explanation either. The face has to carry it. */
        g_obs_turn_failed = 1;
        g_reply_busy = 0;  /* request failed, release the wake loop */
        g_busy_since_ms = 0;
      }
}

/****************************************************************************
 * Send a raw prompt (no role persona) to the LLM.  Used for proactive
 * (unprompted) turns — e.g. boot-time story continuation — where the current
 * role's [SYSTEM] persona should not steer the reply.
 ****************************************************************************/

static void send_raw_to_llm(const char *via, const char *user_text)
{
    ui_ensure_face();

    if (!g_agent_connected || g_agent_client == NULL)
      {
        return;
      }

    busy_mark();

    /* Nobody asked for this one — the board started it. If it fails, the kid
     * gets the gentler line rather than an apology for a question he never
     * asked. */
    g_turn_proactive = true;

    g_obs_reply_streaming = 0;  /* see send_to_llm */
    g_llm_turn = 1;

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

    /* Nobody asked this one, so the question line from the previous turn is
     * stale -- clear both. A proactive "shall we carry on with the story?"
     * sitting under the kid's last question reads as a mis-answer. */
    face_text_new_turn(true);

    syslog(LOG_INFO, "[kid_buddy] llm ask via=%s story=%d \"%.48s\"\n",
           via, g_story_mode, user_text);

    int ret = velaclaw_ask(g_agent_client, &req, llm_response_cb, NULL);
    if (ret < 0)
      {
        g_obs_turn_failed = 1;  /* see send_to_llm */
        g_reply_busy = 0;  /* request failed, release the wake loop */
        g_busy_since_ms = 0;
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

    /* g_mic_open means the wake loop is mid-utterance right now. The mic is
     * per-sentence, so a sentence in progress is a kid still talking -- do not
     * cut in with an unprompted story prompt. (The idle clock only starts at
     * the last completed interaction, so a long silence followed by a fresh
     * sentence can land exactly here.) */
    if (!g_story_mode || g_reply_busy || !g_agent_connected
        || g_agent_client == NULL || g_wake_cmd_ready || g_mic_open)
      {
        return;
      }

    if (g_last_interaction_ms == 0
        || now_ms() - g_last_interaction_ms < IDLE_STORY_MS)
      {
        return;
      }

    /* Offline, the continuation prompt can only come back as an error — and
     * that error would replace the story with an apology. Push the idle clock
     * out instead, so this is retried once per idle period rather than on
     * every 5 s check tick. */
    if (!network_is_connected())
      {
        syslog(LOG_INFO, "[kid_buddy] offline, skipping idle continuation\n");
        g_last_interaction_ms = now_ms();
        return;
      }

    syslog(LOG_INFO, "[kid_buddy] story idle %lldms, prompting continuation\n",
           (long long)(now_ms() - g_last_interaction_ms));

    send_raw_to_llm(
        "idle-story",
        "（这是剧情空闲时的主动搭话，请先看一眼我们的对话历史，不要重新讲故事的开头。"
        "如果刚才的冒险正讲到一半，就用一两句话主动问小朋友还想不想继续冒险，"
        "并给出继续和结束两个选择。如果故事已经讲完了，就夸夸他讲得好。"
        "整段话里不要出现任何引号。）");
}

/****************************************************************************
 * Boot-time greeting and proactive continuation (记忆续篇 / 上下文主动):
 * two phases on the same one-shot timer.
 *
 *   1. ~4s  — speak a fixed local welcome line. No network, no LLM, so the
 *             board says something even with no connectivity or a slow first
 *             token. This is what the child hears on power-up.
 *   2. ~12s — ask the LLM to check history and either resume an unfinished
 *             story/RPG or pick up the conversation. Deletes the timer, and is
 *             skipped entirely when there is no network (see below).
 *
 * The two phases are separated on purpose; see BOOT_GREETING_TICKS.
 ****************************************************************************/

static void proactive_timer_cb(lv_timer_t *timer)
{
    static int ticks = 0;

    ticks++;

    /* ── Phase 1: local greeting ─────────────────────────────── */
    if (!g_boot_greeted && ticks >= BOOT_GREETING_TICKS)
      {
        g_boot_greeted = true;

        syslog(LOG_INFO, "[kid_buddy] boot greeting (local)\n");

        /* The line is for the ear only. The face needs no instruction here:
         * g_boot_greeted turns the sleepy "still waking up" look into an alert
         * one, and g_obs_speaking drives the talking mouth while it plays.
         *
         * Cleared explicitly because g_llm_turn persists between turns: the
         * last thing a previous session did was almost certainly ask the model,
         * so leaving the flag set would make the face show "thinking" through
         * a greeting that never went near the network. */
        g_llm_turn = 0;

        /* Mark busy before queueing, exactly as wake_speak_prompt() does: the
         * wake loop re-opens the mic whenever g_reply_busy is clear, and the
         * codec's DMA channel is shared with playback — a mic opened during
         * the greeting makes it play silently. The TTS worker clears the flag
         * once the line has been spoken (and the watchdog in poll_timer_cb
         * recovers it if that never happens). */
        busy_mark();
        enqueue_tts(KID_BUDDY_BOOT_GREETING, true);

        /* Never share a tick with the probe — send_raw_to_llm() would clear
         * the slot the greeting was just queued into. */
        return;
      }

    /* ── Phase 2: boot-probe LLM turn ────────────────────────── */
    if (ticks < BOOT_PROBE_TICKS)
      {
        return;
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

    /* No network, no probe. The agent comes up as soon as it has an address,
     * but the LLM call still needs the internet: without it the round fails and
     * the agent hands back its own error text, which is exactly what put
     * "Sorry, I encountered an error." on the screen at first boot after a
     * flash. The local greeting has already played by now, so the child is not
     * left with silence either way. */
    if (!network_is_connected())
      {
        syslog(LOG_INFO, "[kid_buddy] offline, skipping boot-probe turn\n");
        return;
      }

    send_raw_to_llm(
        "boot-probe",
        "（这是开机后的主动检查，请查看我们的对话历史。"
        "如果之前有讲到一半的冒险故事或游戏，就主动告诉小朋友上次的冒险讲到一半啦，"
        "问他要不要继续，并给出继续和重新开始两个选择。"
        "如果没有未完成的故事，就简单打个招呼，问问小朋友今天想玩什么。"
        "整段话里不要出现任何引号。）");
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
            busy_mark();
            /* A reminder can arrive while the wake loop still holds the mic
             * (and the codec's shared DMA channel). Wait for it to release the
             * mic before opening playback, else the reminder is silent. */
            for (int i = 0; i < 100 && g_mic_open; i++)
              {
                usleep(10 * 1000);  /* 10 ms */
              }

            /* Tell the face it is talking. g_reply_busy alone cannot: it is
             * raised at the START of the turn (before the LLM request) and
             * cleared at the end, so thinking and speaking look identical from
             * the outside. Also note voice_channel_speak() is synchronous --
             * it returns once every sample has been handed to the codec. */
            g_obs_speaking = 1;
            voice_channel_speak(text);
            g_obs_speaking = 0;

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

/* Monotonic clock in milliseconds (CLOCK_MONOTONIC). Defined next to the
 * reply-busy stamp near the top of the file; used for the follow-up answer
 * window so a kid can answer without re-saying the wake word. */

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

        /* Hand the VAD's own energy figure to the face UI so the listening
         * prop can jump with the child's voice. This costs nothing -- the
         * value is already computed for the onset/offset thresholds below --
         * and it is the only evidence on screen that the toy is really
         * listening. Clamped because sum is int64 and the field is int32. */
        g_obs_mic_msq = (ms > 2000000000LL) ? 2000000000 : (int)ms;

        if (!started && ms >= WAKE_START_MSQ) {
            started = true;
            silence_run = 0;
            /* Tells the face that someone is actually talking to it. g_mic_open
             * alone will not do: the wake loop holds the mic open indefinitely
             * while it waits, so from the outside "waiting" and "listening to
             * you" look identical -- and a face that says "I am listening" at
             * an empty room is worse than no face at all. */
            g_obs_mic_speech = 1;
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
    g_obs_mic_msq = 0;     /* no more energy to report until the next listen */
    g_obs_mic_speech = 0;

    if (!started || pcm_len < WAKE_CHUNK_BYTES) {
        syslog(LOG_INFO, "[kid_buddy] wake: no speech (%zu bytes)\n", pcm_len);
        free(pcm);
        return NULL;
    }

    char text[MSG_BUF_LEN];
    /* This call blocks on the network for a second or more. Everything the
     * child sees during that gap comes from this flag -- without it the face
     * would sit on "listening" with a dead volume arc, which reads as a
     * freeze. */
    g_obs_asr_inflight = 1;
    int ret = voice_asr_recognize(pcm, pcm_len, text, sizeof(text));
    g_obs_asr_inflight = 0;
    free(pcm);

    if (ret < 0 || text[0] == '\0') {
        syslog(LOG_WARNING, "[kid_buddy] wake: ASR failed/empty (ret=%d)\n",
               ret);

        /* Heard speech but got no words back. Worth a distinct face: the
         * child needs to know the toy heard them and it was the recognising
         * that failed, not them. A hard error (ret < 0) is a different story
         * and gets the backoff expression instead. */
        if (ret == 0) {
            g_obs_asr_empty = 1;
        }
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

/* Queue a standalone prompt (e.g. "我在呢") for the LVGL thread.  The thread
 * cannot call enqueue_tts() or touch a label itself, so it parks the text and
 * raises g_reply_busy; the timer shows it on the chat screen and only then
 * speaks it, which keeps the prompt serialized with reply speech.  Raising
 * g_reply_busy here rather than on the LVGL thread matters: the timer only
 * runs every 200 ms, and in that gap the wake loop would otherwise re-open the
 * mic and grab the codec DMA channel out from under the prompt. */
static void wake_speak_prompt(const char *text)
{
    pthread_mutex_lock(&g_tts_lock);
    g_tts_epoch++;
    g_spoken_len = 0;
    g_tts_pending_ready = false;
    g_tts_pending_final = false;
    pthread_mutex_unlock(&g_tts_lock);

    busy_mark();

    pthread_mutex_lock(&g_msg_lock);
    strncpy(g_wake_prompt, text, MSG_BUF_LEN - 1);
    g_wake_prompt[MSG_BUF_LEN - 1] = '\0';
    g_wake_prompt_ready = true;
    pthread_mutex_unlock(&g_msg_lock);
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
        asr_line_set(command);

        busy_mark();
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

/* Quotation marks — half-width and full-width, straight and curly, plus the
 * CJK corner brackets.  MiMo TTS reads every one of them aloud, which wrecks a
 * story: the characters' dialogue is normally quoted, so the child hears
 * "左引号 ... 右引号" wrapped around every line.  Drop them and let the words
 * stand on their own.  The role prompts ask the model not to emit them either,
 * but a prompt is a request, not a guarantee — this is the half that always
 * runs. */
static bool is_quote_cp(unsigned int cp)
{
    switch (cp)
      {
        case 0x0022:  /* " */
        /* The half-width apostrophe (0x27) is deliberately NOT stripped: it is
         * a contraction mark far more often than a quotation mark, so eating it
         * would turn "don't" into "dont". The role prompts still ask for no
         * quotes at all; this is only about not mangling a real word. */
        case 0x0060:  /* ` */
        case 0x2018:  /* ‘ */
        case 0x2019:  /* ’ */
        case 0x201a:  /* ‚ */
        case 0x201b:  /* ‛ */
        case 0x201c:  /* “ */
        case 0x201d:  /* ” */
        case 0x201e:  /* „ */
        case 0x201f:  /* ‟ */
        case 0x2032:  /* ′ */
        case 0x2033:  /* ″ */
        case 0x2035:  /* ‵ */
        case 0x2036:  /* ‶ */
        case 0x300c:  /* 「 */
        case 0x300d:  /* 」 */
        case 0x300e:  /* 『 */
        case 0x300f:  /* 』 */
        case 0x301d:  /* 〝 */
        case 0x301e:  /* 〞 */
        case 0x301f:  /* 〟 */
        case 0xff02:  /* ＂ */
        case 0xff07:  /* ＇ */
            return true;
        default:
            return false;
      }
}

static void strip_quotes(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src)
      {
        unsigned int cp = 0;
        int n = utf8_decode(src, &cp);

        if (is_quote_cp(cp))
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

    /* Watchdog: release a reply that died somewhere between the request and
     * the TTS callback. While the request is healthy every LLM fragment and
     * every TTS speak refreshes g_busy_since_ms, so a stamp older than
     * REPLY_BUSY_TIMEOUT_MS means nothing is coming. Leaving the flag set
     * would keep the wake loop out of the mic forever — the board silences
     * itself and stops answering, which reads as a frozen screen and used to
     * need a power cycle to clear. */
    if (g_reply_busy && g_busy_since_ms != 0
        && now_ms() - g_busy_since_ms > REPLY_BUSY_TIMEOUT_MS)
      {
        syslog(LOG_WARNING,
               "[kid_buddy] reply watchdog: no response for %lldms, "
               "releasing the wake loop\n",
               (long long)(now_ms() - g_busy_since_ms));
        g_reply_busy = 0;
        g_busy_since_ms = 0;
        /* No expression change: releasing the flag drops the face straight back
         * to idle, which is the honest thing to show. */
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
        /* Drop quotation marks — TTS reads 「」""'' aloud, and story dialogue
         * is quoted on nearly every line. */
        strip_quotes(msg_text);

        if (msg_status == 0 || msg_status == 1)
          {
            /* Spoken AND drawn -- the same stripped string feeds both. The
             * strip_* calls above matter to both: MiMo TTS reads emoji and
             * Markdown rules aloud as stray words, and the 16 px font draws
             * them as tofu. */
            enqueue_tts(msg_text, msg_status == 0);
            face_text_reply(msg_text, msg_status == 0);

            if (msg_status == 1)
              {
                /* First text is arriving. The face switches its thinking prop
                 * from a plain sweep to a scattered one so that a 20-second
                 * wait shows a progression instead of one loop that starts to
                 * read as a hang. */
                g_obs_reply_streaming = 1;
              }
          }
        else
          {
            /* A status outside {0,1} means the round failed. This branch never
             * enqueued TTS, so the turn is now COMPLETELY silent -- the face
             * is the only report the child gets. */
            g_obs_turn_failed = 1;
            g_reply_busy = 0;  /* LLM errored out, release the wake loop */
          }
      }

    /* The child's own words, if the wake loop has finished transcribing. Done
     * here, before the turn starts, so the line is on screen while the model is
     * still thinking -- which is exactly when a six-year-old needs to see that
     * they were heard. */
    face_text_pickup_asr();

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
        send_to_llm("voice", wake_cmd);
        return;
      }

    /* Wake-word-only acknowledgement handoff. Speak it from here; the wake
     * thread deliberately did not (it cannot touch the codec's playback path
     * or LVGL) beyond raising g_reply_busy. The face shows the same thing it
     * shows for any other reply -- it is talking -- so nothing is set here. */
    char wake_prompt[MSG_BUF_LEN];
    bool have_prompt = false;

    pthread_mutex_lock(&g_msg_lock);
    if (g_wake_prompt_ready)
      {
        g_wake_prompt_ready = false;
        strncpy(wake_prompt, g_wake_prompt, MSG_BUF_LEN - 1);
        wake_prompt[MSG_BUF_LEN - 1] = '\0';
        have_prompt = true;
      }
    pthread_mutex_unlock(&g_msg_lock);

    if (have_prompt)
      {
        /* A local line, same as the boot greeting -- see the note there. */
        g_llm_turn = 0;
        ui_ensure_face();
        enqueue_tts(wake_prompt, true);
      }
}

/****************************************************************************
 * Face UI
 *
 * The display is one geometric cartoon face over a subtitle plate. The face is
 * the whole interface -- no role names, no status line -- and the plate carries
 * exactly two things: what the child was heard to say, and the model's reply.
 *
 * The reply text is not for the child. A six-year-old cannot read it. It is
 * there because the speech pipeline on this board has failed silently in more
 * ways than any other part of the system (codec DMA contention, TLS handshakes,
 * ASR rate limits, a dead network), and every one of those failures sounds
 * exactly like the toy choosing not to answer. Words on the screen are the only
 * evidence a passing adult has that the thing is working.
 *
 * Role selection is 4 coloured ribbons peeking out of the right edge, like
 * bookmarks in the book-shaped shell. That is the ONLY thing on the screen
 * that responds to touch.
 *
 * HOW TO READ THIS SECTION
 *   face_geom()        one-time layout maths, from the real display size
 *   face_make_*()      one-time object creation, called by ui_create_face()
 *   face_apply()       the only thing that runs at runtime:
 *                      sample device state -> ease -> write only what changed
 *
 * The third of those is where all the difficulty is. See the FRAME BUDGET
 * note up by FACE_PERIOD_MS: this display is single-buffered and full-render,
 * so every frame that changes anything costs a full-screen SPI flush (~31 ms).
 * An unconditional lv_obj_set_style_*() per tick would therefore keep the
 * board flushing forever. Every write below is guarded by a comparison
 * against g_fx_rendered, and an idle face issues zero LVGL calls.
 ****************************************************************************/

/* Palette. Mirrored in tools/face_preview.py -- if you change a value here,
 * change it there too or the preview stops predicting the board. */
#define FACE_CREAM       0xf6e7c8   /* the face features */
#define FACE_CREAM_DIM   0xb9a583   /* shut eyes */
#define FACE_PUPIL       0x241c15
#define FACE_GLINT       0xfffaf0

/* Mic mean-square that maps to a full-width volume arc. The VAD's speech
 * onset threshold is WAKE_START_MSQ (200000), so this sits a little above it:
 * normal speech fills most of the arc without clipping. */
#define FACE_VOL_FULL_MSQ  600000

/* ── Subtitle panel ───────────────────────────────────────────
 * Three fixed lines at the bottom: one for what the child was heard to say,
 * two for the reply. Fixed, not content-sized, so the panel does not grow and
 * shove the face upward the moment the model starts talking.
 *
 * FACE_LINE_H is lv_font_simsun_16_cjk's line_height (19); FACE_CELL_W is the
 * width of one ASCII character, so a CJK glyph is exactly two cells. Both are
 * from reading lv_font_simsun_16_cjk.c, not measured -- if the font is ever
 * swapped, these two numbers and lv_font.h's declaration go with it. */
#define FACE_LINE_H        19
#define FACE_TEXT_LINES    3
#define FACE_PANEL_PAD      6
#define FACE_PANEL_X        6
#define FACE_PANEL_BOT      5   /* gap under the panel */
#define FACE_PANEL_GAP      4   /* gap between the face band and the panel */
#define FACE_CELL_W         8
#define FACE_PANEL_R        8   /* corner radius of the plate */

/* The subtitle is the only thing on this screen that repaints on its own
 * schedule, so it is the only thing that can flood the SPI bus. In FULL render
 * mode one update is a 153 KB / ~31 ms flush, and SPI1 is the same bus the
 * audio DMA uses -- so a streaming reply that landed a chunk every 200 ms
 * would be a continuous full-screen repaint underneath the TTS. Chunks update
 * the label at most this often; the final chunk always updates immediately, so
 * whatever gets dropped mid-stream is on screen by the time the turn ends. */
#define FACE_TEXT_MIN_MS  250

static void    face_render(void);
static void    face_set_role(role_id_t role);
static void    tape_click_cb(lv_event_t *e);

/* sin() at 15-degree steps, x1000. LVGL has lv_trigo_sin(), but a table this
 * small keeps the orbit maths self-contained and obvious -- and the dots only
 * need 24 positions, not 360. */
static const int16_t face_sin15[24] = {
        0,   259,   500,   707,   866,   966,  1000,   966,
      866,   707,   500,   259,     0,  -259,  -500,  -707,
     -866,  -966, -1000,  -966,  -866,  -707,  -500,  -259
};

static inline int face_sin(int deg)
{
    return face_sin15[((deg % 360) + 360) % 360 / 15];
}

static inline int face_cos(int deg)
{
    return face_sin(deg + 90);
}

/* Panel-independent geometry, all derived from g_scr_h/g_scr_w by face_geom(). */
typedef struct {
    int eye_dx;    /* eye centre offset from the face centre */
    int eye_cy;    /* eye centre, absolute */
    int eye_w;
    int eye_h;     /* fully open */
    int eye_r;     /* corner radius -- DELIBERATELY below eye_w/2 */
    int eye_lo;    /* eye height floor = 2*eye_r, see face_eye_h() */
    int pupil_d;   /* pupil diameter (fixed -- see face_render) */
    int glint_d;
    int brow_cy;
    int brow_w;
    int brow_h;
    int mouth_cy;
    int mouth_w;   /* line / ring reference width */
    int mouth_r;   /* the arc's radius */
    int prop_cy;   /* props live in a band above the face */
    int prop_r;    /* dots orbit */
    int dot_d;     /* dot diameter */
    int vol_r;     /* volume gauge radius */
    int vol_w;     /* gauge stroke width */
    int bell_w;
    int bell_h;
    int bub_d;     /* smallest bubble diameter; the others are 2x and 3x */
} face_geom_t;

static face_geom_t g_geo;

/****************************************************************************
 * Face geometry
 ****************************************************************************/

static void face_geom(void)
{
    int band_w = g_scr_w - TAPE_W;
    int panel_h = FACE_TEXT_LINES * FACE_LINE_H + 2 * FACE_PANEL_PAD;

    /* The subtitle panel is placed FIRST, and the face is laid out in what is
     * left over (g_fh). The reverse -- sizing the face and then fitting text
     * underneath -- is how you end up with a panel whose height depends on the
     * face, which makes the text jump when the face changes. Text is fixed
     * furniture; the face is what adapts. */
    g_panel_y1 = g_scr_h - FACE_PANEL_BOT;
    g_panel_y0 = g_panel_y1 - panel_h;
    g_panel_x0 = FACE_PANEL_X;
    g_panel_x1 = g_scr_w - TAPE_W - FACE_PANEL_X;
    g_fh       = g_panel_y0 - FACE_PANEL_GAP;
    g_text_cols = (g_panel_x1 - g_panel_x0 - 2 * FACE_PANEL_PAD) / FACE_CELL_W;

    /* The face is a VERTICAL STACK -- props, brows, eyes, mouth -- so it is
     * laid out in fractions of the band it was given, not of a notional "face
     * diameter". An earlier draft scaled everything off min(band_w, height);
     * on this 320x240 panel (which is wider than it is tall) that produced a
     * small face marooned in the middle of a wide, empty band.
     *
     * The anchors run 12%..82%. The full-screen layout used 12%..79% and left
     * 8% of the screen unused below the mouth; with the band now ending at the
     * panel there is nowhere for that slack to go, so the face would just be
     * that much smaller. 12..82 plus the mouth radius puts the chin at 97% of
     * the band.
     *
     * Props go above the face on purpose. Overhead is where a thought sits;
     * the same shapes below the mouth would read as objects being stood on. */
    g_face_cy = g_fh * 57 / 100;      /* the eye line */
    g_face_cx = band_w / 2;

    /* The eye is WIDER than an earlier draft and much less tall. That draft
     * used 1.75:1 on the theory that a tall eye leaves room to squint; drawn,
     * it was two lozenges with a dot floating in each, and the squint travel
     * it bought was nil anyway -- a capsule's corner radius starts clamping as
     * soon as the height drops below the width. At ~1.33:1 with the pupil at
     * 70% of the eye's width it reads as an eye at every size.
     *
     * eye_w/eye_h cannot grow past this: the eye sets the face's height, and
     * what is below it (mouth, chin) needs its share of a band that is only
     * 162 px tall. So the face is made WIDER instead, by moving the eyes
     * apart -- see eye_dx. */
    g_geo.eye_h  = g_fh * 30 / 100;
    g_geo.eye_w  = g_fh * 22 / 100;
    g_geo.eye_r  = g_geo.eye_w * 37 / 100;   /* < eye_w/2 -- see face_eye_h() */
    g_geo.eye_lo = g_geo.eye_r * 2;

    /* 180% of eye_w, which is what actually fills the band. At the 118% of the
     * full-screen layout the eyes sat 6 px apart against a 35 px eye -- they
     * touched, and the whole face read as a small blob at the centre of a wide
     * empty strip. The gap is now 28 px. */
    g_geo.eye_dx = g_geo.eye_w * 180 / 100;
    g_geo.eye_cy = g_face_cy;

    /* Keep the pair inside the band. eye_w is what gives, not the gap: two
     * narrower eyes still read as a face, whereas eyes running under the
     * bookmarks read as a bug. The pair is 2*1.80*eye_w + eye_w = 4.6 wide. */
    if (g_geo.eye_dx * 2 + g_geo.eye_w > band_w * 88 / 100)
      {
        g_geo.eye_w  = band_w * 88 / 100 * 100 / 460;
        g_geo.eye_r  = g_geo.eye_w * 37 / 100;
        g_geo.eye_lo = g_geo.eye_r * 2;
        g_geo.eye_dx = g_geo.eye_w * 180 / 100;
      }

    g_face_d = g_geo.eye_dx * 2 + g_geo.eye_w;   /* width of the pair */

    g_geo.pupil_d  = g_geo.eye_w * 70 / 100;
    g_geo.glint_d  = g_geo.pupil_d * 30 / 100;
    if (g_geo.glint_d < 4)
      {
        g_geo.glint_d = 4;
      }

    g_geo.brow_w   = g_geo.eye_w * 108 / 100;
    g_geo.brow_h   = g_geo.eye_h * 15 / 100;
    g_geo.brow_cy  = g_fh * 36 / 100;

    g_geo.mouth_cy = g_fh * 82 / 100;
    g_geo.mouth_w  = g_geo.eye_w * 175 / 100;
    g_geo.mouth_r  = g_geo.eye_w * 67 / 100;

    /* The props are fractions of the BAND, not of g_face_d. Tying them to the
     * face width worked when the face owned a 240 px screen; in a 162 px band
     * it made the bell swing off the top of the screen (prop_cy - d*10/100
     * went negative) and dropped the volume gauge's rim onto the eyebrows.
     * What a prop has to do is FIT THE STRIP ABOVE THE BROWS -- how wide the
     * face happens to be is irrelevant to that. */
    g_geo.prop_cy  = g_fh * 12 / 100;   /* 11% clipped the top dot at 11% */
    g_geo.prop_r   = g_fh * 9 / 100;    /* dots' orbit radius */
    g_geo.dot_d    = g_fh * 5 / 100;
    g_geo.vol_r    = g_fh * 17 / 100;   /* rim lands at 29% -- brows at 33% */
    g_geo.vol_w    = g_fh * 4 / 100;
    g_geo.bell_w   = g_fh * 15 / 100;
    g_geo.bell_h   = g_fh * 17 / 100;
    g_geo.bub_d    = g_fh * 4 / 100;
}

/****************************************************************************
 * Subtitle text
 ****************************************************************************/

/* One character's width in half-width cells: 8 px per cell, so 1 cell for a
 * Latin glyph and 2 for a CJK one. This is the unit the panel is measured in,
 * and it is why the truncation below works in cells rather than in bytes -- a
 * byte budget would cut a mixed Chinese/Latin line at a different visual width
 * depending on how much of it happened to be ASCII.
 *
 * The obvious rule -- "U+2000 and up is full width" -- is wrong, and wrong in
 * the expensive direction. That threshold is a guess about the font, and the
 * font does not agree with it: measured out of SimSun.woff, the source of
 * lv_font_simsun_16_cjk, 128 code points BELOW U+2000 are full width (the
 * Greek and Cyrillic alphabets, and the symbols ° ± × ÷ § ¨ · ¤, which the
 * generator pulled in at CJK metrics) and 10 at or above it are half width
 * (• ‰ etc.). Guessing one of the first group as narrow makes a line 8 px
 * wider than this function claims, which is enough for LVGL to wrap it a
 * second time -- and a label clips its own overflow, so the visible result
 * would be a reply whose last line silently never appears.
 *
 * So the exceptions are listed. They were read out of the .woff with a script,
 * not estimated, and the font is exactly 8.00 / 16.00 px wide otherwise --
 * every ASCII glyph including 'W' and ' ' is 8 px to the last bit. */
static int face_cp_cells(unsigned int cp)
{
    if (cp >= 0x2000)
      {
        if (cp == 0x201a || cp == 0x201e)
          {
            return 1;
          }

        if (cp >= 0x2020 && cp <= 0x2022)
          {
            return 1;
          }

        if (cp == 0x2039 || cp == 0x203a || cp == 0x2122)
          {
            return 1;
          }

        return 2;
      }

    if (cp == 0x00a0 || cp == 0x00a4 || cp == 0x00a7 || cp == 0x00a8
        || cp == 0x00b7 || cp == 0x00d7 || cp == 0x00f7)
      {
        return 2;
      }

    if (cp >= 0x00b0 && cp <= 0x00b1)
      {
        return 2;
      }

    if (cp >= 0x02c7 && cp <= 0x02cb)
      {
        return 2;
      }

    if (cp >= 0x0391 && cp <= 0x03c9)   /* Greek */
      {
        return 2;
      }

    if (cp == 0x0401 || (cp >= 0x0410 && cp <= 0x044f) || cp == 0x0451)
      {
        return 2;                        /* Cyrillic */
      }

    return 1;
}

/* Copy src into dst, wrapped to at most `max_rows` rows of `budget` cells
 * each, and RETURN THE ROW COUNT. Only a string too long for max_rows rows is
 * closed with an ellipsis; the caller scrolls anything that fits. The cut lands
 * on a character boundary, never inside a UTF-8 sequence -- slicing bytes would
 * leave the label rendering the tail of a 3-byte character as garbage.
 *
 * The wrapping is the point, not a nicety. LVGL wraps the label for us, but it
 * also breaks on '\n' and at spaces, and a model reply routinely contains
 * both -- so a cell count over the whole string (what this did first) handed a
 * two-line label three lines' worth of text whenever a paragraph break fell
 * inside the budget. A label clips its own overflow, so there is no visible
 * mess to catch the eye: the reply simply stops, with no ellipsis to say so.
 * Inserting the breaks here is also what makes the row count exact, and the
 * row count is what tells the scroller how far down it is allowed to go.
 *
 * Breaks go in exactly where LVGL would put them anyway: at a newline, and
 * otherwise as soon as the line is full. Every CJK character is its own word
 * to LVGL, so breaking per character is what it does regardless; leaving
 * spaces to it would be the one place our count and its wrap could disagree. */
static int face_fit(char *dst, size_t cap, const char *src, int budget,
                    int max_rows, bool *cut_out)
{
    size_t n = 0;
    int cells = 0;      /* cells used on the last line so far */
    int line = 0;       /* lines finished so far */
    bool cut = false;
    int rows = 0;

    if (cut_out != NULL)
      {
        *cut_out = false;
      }

    /* A full-width glyph plus its ellipsis has to fit on a line of its own,
     * and an empty panel is not worth the arithmetic below. */
    if (budget < 4 || max_rows < 1)
      {
        dst[0] = '\0';
        if (cut_out != NULL)
          {
            *cut_out = (*src != '\0');
          }

        return 0;
      }

    cap--;              /* hold one byte back for the NUL */

    while (*src)
      {
        unsigned int cp;
        int len = utf8_decode(src, &cp);
        int w   = face_cp_cells(cp);

        if (cp == '\n' || cp == '\r')
          {
            src += len;
            if (cp == '\r' && *src == '\n')
              {
                src++;
              }

            if (n == 0 || dst[n - 1] == '\n')
              {
                continue;   /* an empty row costs a whole line, and has none */
              }

            if (line + 1 >= max_rows)
              {
                cut = (*src != '\0');
                break;
              }

            dst[n++] = '\n';
            line++;
            cells = 0;
            continue;
          }

        /* The +3 keeps room for the ellipsis below: it is written after the
         * loop, so a byte cap hit here must not leave n sitting on cap. */
        if (cells + w > budget || n + (size_t)len + 3 > cap)
          {
            /* No room left on this line. Start another, unless there is no
             * text on this one to break away from, or no line left to start. */
            if (cells == 0 || line + 1 >= max_rows)
              {
                cut = true;
                break;
              }

            dst[n++] = '\n';
            line++;
            cells = 0;
          }

        memcpy(dst + n, src, (size_t)len);
        n += (size_t)len;
        src += len;
        cells += w;
      }

    if (cut)
      {
        /* The ellipsis is U+2026, which this font draws full width -- two
         * cells -- and it has to share the last line with whatever is on it.
         * Drop characters until it fits, but never the newline that puts it on
         * that line; an ellipsis on a line of its own would cost a row. */
        while (cells + 2 > budget && n > 0 && dst[n - 1] != '\n')
          {
            unsigned int cp;

            n--;
            while (n > 0 && ((unsigned char)dst[n] & 0xc0) == 0x80)
              {
                n--;
              }

            (void)utf8_decode(dst + n, &cp);
            cells -= face_cp_cells(cp);
          }

        if (cells + 2 <= budget && n + 3 <= cap)
          {
            memcpy(dst + n, "\xe2\x80\xa6", 3);
            n += 3;
          }
      }

    /* "你好\n" leaves a row the label would never draw. Harmless, but it is
     * also a byte that makes the next fragment compare as a change. */
    while (n > 0 && dst[n - 1] == '\n')
      {
        n--;
      }

    dst[n] = '\0';

    /* Counted from the string rather than from `line`, because the strip above
     * can take a row away and `line` would not know: text ending in a newline
     * finishes one line but draws only that many rows. */
    if (n > 0)
      {
        rows = 1;
        for (size_t i = 0; i < n; i++)
          {
            if (dst[i] == '\n')
              {
                rows++;
              }
          }
      }

    if (cut_out != NULL)
      {
        *cut_out = cut;
      }

    return rows;
}

/* store must be a buffer that outlives the label -- see lv_label_set_text_static
 * below. `s` may be any temporary. Writes the wrapped rows the label will hold
 * to *rows_out (may be NULL). Returns true if the label's text changed. */
static bool face_set_text(lv_obj_t *lbl, char *store, size_t cap, const char *s,
                          int budget, int max_rows, int *rows_out)
{
    bool cut;
    int rows = face_fit(g_fit_scratch, cap, s, budget, max_rows, &cut);

    (void)cut;   /* the ellipsis already says it on screen */

    if (rows_out != NULL)
      {
        *rows_out = rows;
      }

    /* Only write when the string actually changed. lv_label_set_text() is
     * unconditional, and on this display an invalidate is a 153 KB full-screen
     * flush (~31 ms on SPI1), so a fragment that does not alter the text must
     * not reach it. */
    if (strcmp(g_fit_scratch, store) == 0)
      {
        return false;
      }

    strncpy(store, g_fit_scratch, cap - 1);
    store[cap - 1] = '\0';

    /* _static, not the copying setter: this runs up to 4x a second per label
     * while a reply streams, and the copying one hands the heap a malloc/free
     * pair every time. The buffers are file-scope, so the pointer stays valid
     * for the life of the label. */
    lv_label_set_text_static(lbl, store);
    return true;
}

/* Clear the subtitles for a turn that is just starting. Called from the LVGL
 * thread, from the two places that actually begin a turn (send_to_llm and
 * send_raw_to_llm), rather than inferred from a counter -- the voice path sets
 * the child's line and starts the turn in the same tick, and a "the turn number
 * changed" test would then wipe the line it had only just written.
 *
 * send_raw_to_llm() passes clear_you: the proactive and boot turns have no
 * utterance behind them, so the previous question is stale and must go. The
 * voice path leaves it, because its line is arriving in the same tick. */
static void face_text_new_turn(bool clear_you)
{
    if (!g_face_ready)
      {
        return;
      }

    g_say_shown_at = 0;
    face_set_text(g_face.say_lbl, g_say_shown, sizeof(g_say_shown), "",
                  g_text_cols, FACE_SAY_ROWS_MAX, &g_say_rows);

    /* Back to the top for the new reply. The scroll clock restarts with it, so
     * the first row gets a full FACE_SCROLL_MS before it moves. */
    g_say_off = 0;
    g_say_scroll_at = now_ms();
    lv_obj_set_y(g_face.say_lbl, 0);

    if (clear_you)
      {
        face_set_text(g_face.you_lbl, g_you_shown, sizeof(g_you_shown), "",
                      g_text_cols, 1, NULL);
      }
}

/* Show the child's own words, as soon as the wake loop has a transcript. */
static void face_text_pickup_asr(void)
{
    char you[FACE_TEXT_BUF];
    char line[FACE_TEXT_BUF + 8];   /* "你：" is 6 bytes, plus the NUL */

    if (!g_face_ready || !g_asr_line_ready)
      {
        return;
      }

    pthread_mutex_lock(&g_asr_line_lock);
    strncpy(you, g_asr_line, sizeof(you) - 1);
    you[sizeof(you) - 1] = '\0';
    g_asr_line_ready = false;
    pthread_mutex_unlock(&g_asr_line_lock);

    /* The budget covers the whole line, prefix and all -- "你：" is two
     * full-width characters, so the transcript gets the other 29 cells. */
    snprintf(line, sizeof(line), "你：%s", you);
    face_set_text(g_face.you_lbl, g_you_shown, sizeof(g_you_shown), line,
                  g_text_cols, 1, NULL);
}

/* Show the reply. `text` is the CUMULATIVE reply of the current turn -- ai_agent
 * re-sends the whole string so far with every status==1 fragment -- so this
 * replaces what is on screen rather than appending to it. */
static void face_text_reply(const char *text, bool final)
{
    if (!g_face_ready)
      {
        return;
      }

    /* Throttled while streaming; see FACE_TEXT_MIN_MS. Nothing is lost by
     * dropping a middle fragment: the next one carries all of it, and the final
     * one is never dropped. */
    if (!final && now_ms() - g_say_shown_at < FACE_TEXT_MIN_MS)
      {
        return;
      }

    /* The clock is only restarted when the panel actually changes, not on every
     * fragment that arrives -- otherwise a reply that streams faster than
     * FACE_TEXT_MIN_MS would keep pushing its own deadline back and the visible
     * text would never advance until the turn ended. */
    if (face_set_text(g_face.say_lbl, g_say_shown, sizeof(g_say_shown), text,
                      g_text_cols, FACE_SAY_ROWS_MAX, &g_say_rows))
      {
        g_say_shown_at = now_ms();

        /* The scroll waits on the same signal rather than on `final`. The
         * fragments are cumulative, so the label only ever grows -- holding the
         * offset steady and just restarting the clock means the reply starts
         * walking FACE_SCROLL_MS after the text last moved, which is the same
         * thing as "after the turn finished" when it finishes, and the right
         * thing anyway when a turn dies mid-stream and the text stops arriving.
         * Waiting on `final` instead would strand a partial reply at the top
         * forever, because the error path never calls this with final. */
        g_say_scroll_at = now_ms();
      }
}

/****************************************************************************
 * Small object helpers
 ****************************************************************************/

/* Every object in the face is a plain lv_obj with no theme decoration and no
 * padding. Padding matters: lv_obj_align() positions against the parent's
 * CONTENT area, so a theme's default padding would silently shift every child
 * and make the geometry above a lie. */
static lv_obj_t *face_blob(lv_obj_t *parent, int w, int h, uint32_t rgb)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

static lv_obj_t *face_circle(lv_obj_t *parent, int d, uint32_t rgb)
{
    lv_obj_t *o = face_blob(parent, d, d, rgb);

    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    return o;
}

/* An arc used as a stroked curve. The background ring is made transparent so
 * only the indicator shows -- that is the whole point of using lv_arc here:
 * its edges are mask-antialiased, unlike lv_line, whose segment joins are
 * left unfilled and read as a notch on a curve. */
static lv_obj_t *face_arc(lv_obj_t *parent, int size, int width, uint32_t rgb)
{
    lv_obj_t *o = lv_arc_create(parent);

    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(o, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(o, lv_color_hex(rgb), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(o, true, LV_PART_INDICATOR);

    /* lv_arc_draw() unconditionally draws the knob -- it is the last thing the
     * function does, with no "is this arc interactive" test -- and the default
     * theme paints LV_PART_KNOB as a filled circle. Left alone, every arc in
     * this file (mouth, volume, hook) would wear a dot on its tip. Making it
     * transparent rather than merely small matters: get_knob_area() sizes the
     * area from the knob's padding, and its area is invalidated on every angle
     * change, so an oversized invisible knob would still cost work. */
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(o, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(o, 0, LV_PART_KNOB);
    return o;
}

/****************************************************************************
 * Object creation
 ****************************************************************************/

static void face_make_eyes(void)
{
    int w = g_geo.eye_w;
    int h = g_geo.eye_h;

    for (int i = 0; i < 2; i++)
      {
        int cx = g_face_cx + (i == 0 ? -g_geo.eye_dx : g_geo.eye_dx);

        /* The eye is a rounded rect whose SIZE animates. Its radius is pinned
         * to eye_r and never changes: lv_draw_sw_mask.c caches the
         * antialiased circle mask keyed on radius alone (4 slots), so a radius
         * that changes per frame means a cache miss per frame, and a miss
         * costs two lv_malloc()s inside circ_calc_aa4().
         *
         * eye_r is deliberately BELOW eye_w/2 rather than a capsule, because
         * the effective radius is min(eye_r, h/2) and a capsule only stays
         * cache-friendly while h >= eye_w -- which would leave the entire
         * squint range 16 px wide. See face_eye_h(). */
        lv_obj_t *eye = face_blob(g_face.scr, w, h, FACE_CREAM);
        lv_obj_set_style_radius(eye, g_geo.eye_r, LV_PART_MAIN);
        lv_obj_set_pos(eye, cx - w / 2, g_geo.eye_cy - h / 2);
        g_face.eye_white[i] = eye;

        /* Pupil + highlight ride inside the eye, so they follow it. */
        g_face.pupil[i] = face_circle(eye, g_geo.pupil_d, FACE_PUPIL);
        lv_obj_align(g_face.pupil[i], LV_ALIGN_CENTER, 0, 0);

        g_face.glint[i] = face_circle(eye, g_geo.glint_d, FACE_GLINT);
        lv_obj_align(g_face.glint[i], LV_ALIGN_CENTER,
                     g_geo.pupil_d / 3, -g_geo.pupil_d / 3);

        /* A separate flat bar for a fully shut eye. This exists because the
         * open eye cannot be squashed to nothing without driving its height
         * (and therefore its effective radius) below the cache-friendly
         * minimum -- and because a rounded rect at height 0 draws nothing at
         * all, so a blink would just make the eyes vanish.
         *
         * It is w/4 thick, not the hairline it started as: at 4 px and a dim
         * cream, sitting 45 px under an equally horizontal 9 px brow, the
         * closed lid read as a second and slightly dirtier pair of eyebrows.
         * Sized off eye_w, not eye_h -- a lid is as tall as its eye is wide. */
        lv_obj_t *shut = face_blob(g_face.scr, w, w / 4, FACE_CREAM_DIM);
        lv_obj_set_style_radius(shut, w / 8, LV_PART_MAIN);
        lv_obj_set_pos(shut, cx - w / 2, g_geo.eye_cy - w / 8);
        lv_obj_add_flag(shut, LV_OBJ_FLAG_HIDDEN);
        g_face.eye_shut[i] = shut;
      }
}

static void face_make_brows(void)
{
    for (int i = 0; i < 2; i++)
      {
        int cx = g_face_cx + (i == 0 ? -g_geo.eye_dx : g_geo.eye_dx);
        lv_obj_t *b = face_blob(g_face.scr, g_geo.brow_w, g_geo.brow_h,
                                FACE_CREAM);

        lv_obj_set_style_radius(b, g_geo.brow_h / 2, LV_PART_MAIN);
        lv_obj_set_pos(b, cx - g_geo.brow_w / 2,
                       g_geo.brow_cy - g_geo.brow_h / 2);

        /* Rotation gives lifelike brows without any geometry maths: one style
         * write per frame instead of recomputing line points. Two rules come
         * out of the LVGL source and both bite if ignored:
         *
         *  - The pivot MUST be set explicitly. The default is the object's
         *    top-left corner and the default theme does not override it
         *    (lv_style_prop_get_default() has no TRANSFORM_PIVOT case), so an
         *    unset pivot makes the brow swing away instead of tilting.
         *  - The hinge is the OUTER end, so a positive tilt drops the inner
         *    end (an angry V). The two brows therefore need mirrored pivots
         *    AND mirrored signs -- see face_render().
         *
         * This is 2 of the 4 rotated objects the whole UI is allowed; each one
         * costs a temporary layer plus a draw buffer per frame (lv_draw.c). */
        lv_obj_set_style_transform_pivot_y(b, LV_PCT(50), LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_x(b, (i == 0) ? LV_PCT(0) : LV_PCT(100),
                                           LV_PART_MAIN);
        g_face.brow[i] = b;
      }
}

static void face_make_mouth(void)
{
    int w = g_geo.mouth_w;
    int r = g_geo.mouth_r;

    /* Smile / frown. Kept as a fixed-size arc and swept by angle, so nothing
     * is resized and no mask cache is disturbed.
     *
     * Sized off mouth_r, NOT off mouth_w: mouth_w is only the reference the
     * flat line and the ring use. Drawing the arc at mouth_w across made the
     * circle twice the size it needed to be, which put a deep frown's apex up
     * between the eyes -- see face_mouth_sweep(). */
    g_face.mouth_arc = face_arc(g_face.scr, r * 2, w / 9, FACE_CREAM);
    lv_obj_align(g_face.mouth_arc, LV_ALIGN_TOP_LEFT,
                 g_face_cx - r, g_geo.mouth_cy - r);

    /* Flat mouth -- a separate bar, because an arc swept to start == end draws
     * nothing at all (lv_draw_sw_arc early-returns), so "neutral" cannot be
     * expressed by simply closing the smile. */
    g_face.mouth_line = face_blob(g_face.scr, w / 2, w / 12, FACE_CREAM);
    lv_obj_set_style_radius(g_face.mouth_line, w / 24, LV_PART_MAIN);
    lv_obj_align(g_face.mouth_line, LV_ALIGN_TOP_LEFT,
                 g_face_cx - w / 4, g_geo.mouth_cy - w / 24);
    lv_obj_add_flag(g_face.mouth_line, LV_OBJ_FLAG_HIDDEN);

    /* Open "O" -- surprise, and the talking mouth. A full ring is a special
     * case in lv_draw_sw_arc that delegates to the plain border path, so this
     * is the cheapest of the three shapes to draw. */
    g_face.mouth_ring = face_arc(g_face.scr, w * 40 / 100, w / 11, FACE_CREAM);
    lv_obj_align(g_face.mouth_ring, LV_ALIGN_TOP_LEFT,
                 g_face_cx - w * 20 / 100, g_geo.mouth_cy - w * 20 / 100);
    lv_obj_add_flag(g_face.mouth_ring, LV_OBJ_FLAG_HIDDEN);
}

static void face_make_props(void)
{
    int d = g_face_d;

    /* Every prop below is sized off g_fh and hung off prop_cy, NOT off the
     * face width d and the old screen-height fraction. See face_geom(): with
     * the props anchored at 12% of a full 240 px screen they cleared the brows
     * comfortably, but at 12% of this 162 px band they came out overlapping
     * them, and the bell -- which hangs from its top edge -- started above
     * y=0. A prop's only constraint is the strip above the brows. */
    (void)d;

    /* Thinking: three dots orbiting. Three plain circles repositioned each
     * frame -- no rotation, so no per-frame layer allocation. */
    for (int i = 0; i < 3; i++)
      {
        g_face.dot[i] = face_circle(g_face.scr, g_geo.dot_d, FACE_CREAM);
        lv_obj_align(g_face.dot[i], LV_ALIGN_TOP_LEFT, g_face_cx - g_geo.dot_d / 2,
                     g_geo.prop_cy + g_geo.prop_r - g_geo.dot_d / 2);
        lv_obj_add_flag(g_face.dot[i], LV_OBJ_FLAG_HIDDEN);
      }

    /* Listening: an arc whose sweep follows the microphone. Fed from the VAD's
     * own per-chunk energy, which is already computed -- see g_obs_mic_msq.
     *
     * Drawn as the TOP HALF of a circle whose top edge touches prop_cy: a wide
     * shallow rainbow that fills left-to-right like a gauge. A small
     * full-circle arc centred on prop_cy instead -- the first attempt -- came
     * out 58 px across and read as a third eyebrow floating over the left eye,
     * which is exactly what the faint track below is there to prevent. */
    g_face.vol_arc = face_arc(g_face.scr, g_geo.vol_r * 2, g_geo.vol_w,
                              FACE_CREAM);

    /* Give the volume arc its faint background track back. face_arc() blanks
     * it for every other arc, but an indicator sweeping across an empty gap
     * reads as a stray mark rather than as a meter -- with the track showing,
     * the same marks read as a gauge filling up. It is also free:
     * lv_arc_draw() issues the background-arc draw call every frame either
     * way, so this changes an alpha, not the number of operations. */
    lv_obj_set_style_arc_opa(g_face.vol_arc, LV_OPA_30, LV_PART_MAIN);
    lv_arc_set_bg_angles(g_face.vol_arc, 180, 360);
    lv_arc_set_range(g_face.vol_arc, 0, 100);
    lv_arc_set_value(g_face.vol_arc, 0);
    lv_obj_align(g_face.vol_arc, LV_ALIGN_TOP_LEFT,
                 g_face_cx - g_geo.vol_r, g_geo.prop_cy);
    lv_obj_add_flag(g_face.vol_arc, LV_OBJ_FLAG_HIDDEN);

    /* Reminder: a bell hanging from a pivot at its top edge, so the wobble
     * reads as a swing rather than a spin. */
    g_face.bell = face_blob(g_face.scr, g_geo.bell_w, g_geo.bell_h, FACE_CREAM);
    lv_obj_set_style_radius(g_face.bell, g_geo.bell_w / 2, LV_PART_MAIN);
    lv_obj_align(g_face.bell, LV_ALIGN_TOP_LEFT,
                 g_face_cx - g_geo.bell_w / 2,
                 g_geo.prop_cy - g_geo.bell_h / 2);
    lv_obj_set_style_transform_pivot_x(g_face.bell, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(g_face.bell, LV_PCT(0), LV_PART_MAIN);
    lv_obj_add_flag(g_face.bell, LV_OBJ_FLAG_HIDDEN);

    /* Offline: slow bubbles. Deliberately the sleepiest prop -- being offline
     * is "I cannot help you right now", not "I have done something wrong", so
     * it must not read as distress. */
    for (int i = 0; i < 3; i++)
      {
        g_face.bubble[i] = face_circle(g_face.scr,
                                       g_geo.bub_d * (2 + i), FACE_CREAM_DIM);
        lv_obj_align(g_face.bubble[i], LV_ALIGN_TOP_LEFT, g_face_cx, g_geo.prop_cy);
        lv_obj_add_flag(g_face.bubble[i], LV_OBJ_FLAG_HIDDEN);
      }

    /* Confused: a drooping hook, drawn as a partial arc. */
    g_face.hook = face_arc(g_face.scr, g_fh * 22 / 100, g_fh * 5 / 100,
                           FACE_CREAM);
    lv_arc_set_bg_angles(g_face.hook, 0, 360);
    lv_arc_set_angles(g_face.hook, 150, 330);
    lv_obj_align(g_face.hook, LV_ALIGN_TOP_LEFT,
                 g_face_cx - g_fh * 11 / 100,
                 g_geo.prop_cy - g_fh * 11 / 100);
    lv_obj_add_flag(g_face.hook, LV_OBJ_FLAG_HIDDEN);

    /* Proactive greeting: a hand waving at the child, rotated as a unit about
     * the wrist. The 4th and last rotated object.
     *
     * Unlike the bell this one is anchored by its MIDDLE, because it has to
     * hang below prop_cy as well as above: anchored by its top edge the way the
     * bell is, a hand this tall (22% of the band) starts at y = -2. */
    {
      int hw = g_fh * 18 / 100;
      int hh = g_fh * 22 / 100;

      g_face.hand = lv_obj_create(g_face.scr);
      lv_obj_remove_flag(g_face.hand, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_size(g_face.hand, hw, hh);
      lv_obj_set_style_pad_all(g_face.hand, 0, LV_PART_MAIN);
      lv_obj_set_style_border_width(g_face.hand, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(g_face.hand, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_align(g_face.hand, LV_ALIGN_TOP_LEFT,
                   g_face_cx - hw / 2, g_geo.prop_cy - hh / 2);
      lv_obj_set_style_transform_pivot_x(g_face.hand, LV_PCT(50), LV_PART_MAIN);
      lv_obj_set_style_transform_pivot_y(g_face.hand, LV_PCT(100), LV_PART_MAIN);

      for (int i = 0; i < 3; i++)
        {
          lv_obj_t *finger = face_blob(g_face.hand, hw * 22 / 100,
                                       hh * 50 / 100, FACE_CREAM);
          lv_obj_set_style_radius(finger, hw * 11 / 100, LV_PART_MAIN);
          lv_obj_set_pos(finger, hw * (4 + i * 30) / 100, 0);
        }

      lv_obj_t *palm = face_blob(g_face.hand, hw * 90 / 100, hh * 55 / 100,
                                 FACE_CREAM);
      lv_obj_set_style_radius(palm, hw * 22 / 100, LV_PART_MAIN);
      lv_obj_set_pos(palm, hw * 5 / 100, hh * 45 / 100);
      lv_obj_add_flag(g_face.hand, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The bookmark ribbons. These are the only interactive objects in the UI. */
static void face_make_tapes(void)
{
    int n = ROLE_COUNT;
    int avail = g_scr_h - 2 * TAPE_GAP;
    int h = avail / n - TAPE_GAP;

    if (h < TAPE_MIN_H)
      {
        h = TAPE_MIN_H;
      }

    for (int i = 0; i < ROLE_COUNT; i++)
      {
        int y = TAPE_GAP + i * (h + TAPE_GAP);

        /* Drawn at its "pulled out" x already; face_set_role() slides the
         * unselected ones back under the edge. Only the left corners are
         * rounded, so they read as ribbons going off the edge of the page. */
        lv_obj_t *t = face_blob(g_face.scr, TAPE_W, h, FACE_CREAM_DIM);
        lv_obj_set_style_bg_color(t, g_roles[i].color, LV_PART_MAIN);
        lv_obj_set_style_radius(t, TAPE_W / 3, LV_PART_MAIN);
        lv_obj_set_pos(t, g_scr_w - TAPE_W - TAPE_PULL, y);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tape_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        g_face.tape[i] = t;
      }
}

/* The subtitle plate and its two labels.
 *
 * Two labels, not one: the child's line and the reply are separate strings with
 * separate lifetimes, and one label would mean rebuilding a joined string on
 * every streaming chunk -- the exact operation this panel is trying to avoid.
 * Both are created once at boot and only ever have their text replaced.
 *
 * The plate is a plain object at LV_OPA_10 over the role-tinted background,
 * which is LVGL's own LV_OPA_MIX2 -- the same 10% cream wash the preview draws.
 * It is deliberately not a shadowed or bordered card: LV_DRAW_SW_SHADOW_CACHE_SIZE
 * is 0 in this build (.config:3889), so every shadow is re-blurred per frame. */
static void face_make_panel(void)
{
    int w = g_panel_x1 - g_panel_x0;
    int h = g_panel_y1 - g_panel_y0;
    int cw = w - 2 * FACE_PANEL_PAD;

    g_face.panel = face_blob(g_face.scr, w, h, FACE_CREAM);
    lv_obj_set_style_radius(g_face.panel, FACE_PANEL_R, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_face.panel, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_pos(g_face.panel, g_panel_x0, g_panel_y0);

    /* Line 1: "你：" and what the child was heard to say. It keeps its row even
     * when empty, so the reply below never slides up and down as transcripts
     * come and go -- motion on a full-render display is a full-screen flush,
     * and a line that moves for no reason is the most expensive kind. */
    lv_obj_t *you = lv_label_create(g_face.scr);
    lv_obj_set_style_text_font(you, &lv_font_simsun_16_cjk, LV_PART_MAIN);
    lv_obj_set_style_text_color(you, lv_color_hex(FACE_CREAM_DIM), LV_PART_MAIN);
    lv_label_set_long_mode(you, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(you, cw, FACE_LINE_H);
    lv_obj_set_pos(you, g_panel_x0 + FACE_PANEL_PAD, g_panel_y0 + FACE_PANEL_PAD);
    lv_label_set_text(you, "");
    g_face.you_lbl = you;

    /* Lines 2-3: the reply, scrolled.
     *
     * Two objects, because the reply is taller than the two rows it shows
     * through. `win` is exactly those two rows and clips -- lv_refr.c:139 takes
     * a child down to the parent's rectangle unless the parent carries
     * LV_OBJ_FLAG_OVERFLOW_VISIBLE, which this one does not. `say` is the
     * label, up to FACE_SAY_ROWS_MAX rows tall, slid upward inside it.
     *
     * The label's height being explicit rather than LV_SIZE_CONTENT is what
     * makes lv_label_refr_text() leave it alone -- with a content height it
     * would resize itself to the text on every fragment. */
    lv_obj_t *win = face_blob(g_face.scr, cw, FACE_LINE_H * FACE_SAY_ROWS_VIS,
                              FACE_CREAM);
    lv_obj_set_style_bg_opa(win, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(win, 0, LV_PART_MAIN);
    lv_obj_set_pos(win, g_panel_x0 + FACE_PANEL_PAD,
                   g_panel_y0 + FACE_PANEL_PAD + FACE_LINE_H);
    g_face.say_win = win;

    lv_obj_t *say = lv_label_create(win);
    lv_obj_set_style_text_font(say, &lv_font_simsun_16_cjk, LV_PART_MAIN);
    lv_obj_set_style_text_color(say, lv_color_hex(FACE_CREAM), LV_PART_MAIN);
    lv_label_set_long_mode(say, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(say, cw, FACE_LINE_H * FACE_SAY_ROWS_MAX);
    lv_obj_set_pos(say, 0, 0);
    lv_label_set_text(say, "");
    g_face.say_lbl = say;
}

/****************************************************************************
 * Build the face (once, at boot)
 ****************************************************************************/

static void ui_create_face(void)
{
    if (g_face_ready)
      {
        return;
      }

    face_geom();

    /* The screen object is the background. radius 0 and opaque on purpose: a
     * full-screen fill with a radius, or with partial opacity, drops off
     * lv_draw_sw_fill.c's fast row-fill path and masks all 76,800 pixels
     * instead. Scrollable off so a child that momentarily pokes outside its
     * parent cannot summon a scrollbar. */
    g_face.scr = lv_obj_create(NULL);
    lv_obj_remove_flag(g_face.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(g_face.scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_face.scr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_face.scr, 0, LV_PART_MAIN);

    face_make_eyes();
    face_make_brows();
    face_make_mouth();
    face_make_props();
    face_make_panel();
    face_make_tapes();

    /* Designated: this struct has grown once already, and a positional
     * initialiser would have silently shifted every field after the insertion
     * point -- `mouth_curve = 35` would have landed in brow_tilt_r and left
     * `mouth` holding 35, with no warning at all. */
    g_fx_now      = (face_expr_t){ .eye_open = 100, .mouth = FACE_MOUTH_ARC,
                                   .mouth_curve = 35 };
    g_fx_target   = g_fx_now;
    /* All -1 so the first face_render() writes every property unconditionally:
     * a "shadow copy" that happens to match a real value would leave that
     * object at whatever the theme gave it. `mouth` is uint8_t and its real
     * values are 0..2, so 255 is its not-drawn-yet marker. */
    g_fx_rendered = (face_expr_t){ .eye_open = -1, .pupil_dx = -1, .pupil_dy = -1,
                                   .brow_dy = -1, .brow_tilt = -1,
                                   .brow_tilt_r = -1, .mouth = 255,
                                   .mouth_curve = -1 };

    g_face_ready = true;

    face_set_role(g_current_role);
    face_render();
    lv_scr_load(g_face.scr);

    syslog(LOG_INFO,
           "[kid_buddy] face UI %s on %dx%d (face band %d, panel %d,%d %dx%d, "
           "%d cols)\n",
           KID_BUDDY_VERSION, g_scr_w, g_scr_h, g_fh,
           g_panel_x0, g_panel_y0, g_panel_x1 - g_panel_x0,
           g_panel_y1 - g_panel_y0, g_text_cols);
}

static void ui_ensure_face(void)
{
    if (!g_face_ready)
      {
        ui_create_face();
      }
}

/****************************************************************************
 * Role switching
 ****************************************************************************/

static void tape_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    face_set_role((role_id_t)idx);
}

static void face_set_role(role_id_t role)
{
    if (!g_face_ready)
      {
        g_current_role = role;
        return;
      }

    g_current_role = role;

    /* Tint the whole page with the role's colour -- the design brief is that
     * picking a character changes the mood of the screen, not just a label.
     *
     * Blend the role colour INTO a near-black ground; do not darken the role
     * colour itself. Dividing orange by four gives brown, dividing green by
     * four gives a different brown and dividing blue by four gives a third
     * brown -- which is exactly what the first attempt at this did, and all
     * four roles came out looking like the same muddy page. Mixing 42% of the
     * role over a common base keeps the differences additive, so the four
     * read as warm / green / blue / rose while the cream features keep their
     * contrast. The bookmark itself still carries the pure colour. */
    {
      int br = (FACE_BG_BASE >> 16) & 0xff;
      int bg = (FACE_BG_BASE >> 8) & 0xff;
      int bb = FACE_BG_BASE & 0xff;
      lv_color_t c = g_roles[role].color;

      lv_obj_set_style_bg_color(g_face.scr,
          lv_color_make(br + ((int)c.red   - br) * 42 / 100,
                        bg + ((int)c.green - bg) * 42 / 100,
                        bb + ((int)c.blue  - bb) * 42 / 100),
          LV_PART_MAIN);
      lv_obj_set_style_bg_opa(g_face.scr, LV_OPA_COVER, LV_PART_MAIN);
    }

    for (int i = 0; i < ROLE_COUNT; i++)
      {
        /* The selected ribbon slides out of the page edge; the rest tuck back
         * under it. Only the selected one is fully visible. */
        int x = g_scr_w - TAPE_W - (i == (int)role ? TAPE_PULL : 0);

        if (lv_obj_get_x(g_face.tape[i]) != x)
          {
            lv_obj_set_x(g_face.tape[i], x);
          }
      }
}

/****************************************************************************
 * Render
 ****************************************************************************/

static void face_show(lv_obj_t *o, bool show)
{
    if (o == NULL)
      {
        return;
      }

    /* Both lv_obj_add_flag() and lv_obj_remove_flag() test the bit before
     * doing anything, so calling these on an unchanged object is genuinely
     * free -- which is what lets face_prop_update() re-assert the whole prop
     * set every frame instead of tracking which one was up last. */
    if (show)
      {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
      }
    else
      {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      }
}

/* How far the eye geometry is scaled up for a given openness.
 *
 * Past 100 the eye grows in BOTH axes and keeps its aspect ratio. Growing the
 * height alone -- the obvious reading of "wide-eyed" -- walks the eye straight
 * back toward the capsule this geometry exists to avoid. */
static int face_eye_scale(int open)
{
    if (open <= 100)
      {
        return 100;
      }

    return 100 + (open - 100) / 2;
}

/* Open height of an eye for a given openness.
 *
 * Never returns less than 2*eye_r: that is where the effective radius --
 * min(eye_r, h/2) -- would start moving, and lv_draw_sw_mask.c keys its circle
 * cache on radius ALONE (CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=4), so a moving
 * radius is a mask rebuild every frame. The geometry buys that floor by
 * keeping eye_r below eye_w/2 instead of using a capsule; the whole squint
 * range then sits between eye_lo and eye_h. */
static int face_eye_h(int open)
{
    if (open <= 0)
      {
        return g_geo.eye_w;
      }

    if (open >= 100)
      {
        return g_geo.eye_h * face_eye_scale(open) / 100;
      }

    return g_geo.eye_lo + (g_geo.eye_h - g_geo.eye_lo) * open / 100;
}

/* Open width of an eye -- see face_eye_scale(). */
static int face_eye_w(int open)
{
    return g_geo.eye_w * face_eye_scale(open) / 100;
}

/* Half-sweep of the mouth arc, in degrees, for a given curve.
 *
 * Sweep is measured from the bottom of the circle (90 for a smile, 270 for a
 * frown), so the chord half-width is mouth_r*sin(sweep) and the bulge is
 * mouth_r*(1-cos(sweep)). Starting the sweep near 0 therefore yields a mouth
 * that is SMALL rather than flat -- an earlier draft used 12 + 78*|c|/100,
 * which at the resting curve of 35 rendered a mouth narrower than one eye.
 * Starting at 52 keeps the chord roughly constant and spends the range on the
 * bulge, which is what "smile harder" ought to look like.
 *
 * The upper bound is bounded in turn by the gap between the eyes: their inner
 * edges sit at face_cx +- (eye_dx - eye_w/2), 64 px apart on this panel, so
 * the half-width has to stay under about 32 or a deep frown runs into an eye. */
static int face_mouth_sweep(int curve)
{
    int c = (curve < 0) ? -curve : curve;

    return 52 + 30 * c / 100;
}

/* Write g_fx_now onto the objects -- but only the fields that actually moved.
 * Every lv_obj_set_style_*() calls lv_obj_invalidate() unconditionally, and
 * on this single-buffered full-render display one invalidate means a complete
 * redraw plus a 153 KB SPI flush (~31 ms). So the guard below is not a
 * micro-optimisation, it is the difference between an idle face costing
 * nothing and an idle face costing a third of the CPU forever. */
static void face_render(void)
{
    const face_expr_t *n = &g_fx_now;
    const face_expr_t *r = &g_fx_rendered;
    int i;

    if (!g_face_ready)
      {
        return;
      }

    if (n->eye_open != r->eye_open)
      {
        bool shut = (n->eye_open <= 0);
        int  h    = face_eye_h(n->eye_open);
        int  w    = face_eye_w(n->eye_open);

        if (!shut)
          {
            /* Radius is NOT touched here, and neither is x -- only the size.
             * The eye stays centred on its own axis, so the pair widens
             * symmetrically without any repositioning. */
            for (i = 0; i < 2; i++)
              {
                lv_obj_set_size(g_face.eye_white[i], w, h);
                lv_obj_set_pos(g_face.eye_white[i],
                               (g_face_cx + (i == 0 ? -g_geo.eye_dx : g_geo.eye_dx))
                                 - w / 2,
                               g_geo.eye_cy - h / 2);
              }
          }

        for (i = 0; i < 2; i++)
          {
            face_show(g_face.eye_white[i], !shut);
            face_show(g_face.eye_shut[i], shut);
          }
      }

    if (n->pupil_dx != r->pupil_dx || n->pupil_dy != r->pupil_dy)
      {
        /* Aligned rather than positioned, so the pupil stays centred when the
         * eye is resized for a squint -- lv_obj_align re-runs on every layout
         * pass, a plain set_pos would not. */
        for (i = 0; i < 2; i++)
          {
            lv_obj_align(g_face.pupil[i], LV_ALIGN_CENTER,
                         n->pupil_dx, n->pupil_dy);
            lv_obj_align(g_face.glint[i], LV_ALIGN_CENTER,
                         n->pupil_dx + g_geo.pupil_d / 3,
                         n->pupil_dy - g_geo.pupil_d / 3);
          }
      }

    if (n->brow_dy  != r->brow_dy  ||
        n->brow_tilt != r->brow_tilt || n->brow_tilt_r != r->brow_tilt_r)
      {
        for (i = 0; i < 2; i++)
          {
            int tilt = (i == 0) ? n->brow_tilt : n->brow_tilt_r;

            lv_obj_set_y(g_face.brow[i],
                         g_geo.brow_cy - g_geo.brow_h / 2 - n->brow_dy);

            /* transform_rotation is in TENTHS of a degree, and positive is
             * CLOCKWISE: lv_draw_sw_transform.c negates the style value
             * (tr_dsc.angle = -draw_dsc->rotation) and its inverse map is
             * [c -s; s c] under y-down, which puts a point to the right of
             * the pivot below it.
             *
             * The sign is then mirrored between the two brows because they
             * hinge on opposite ends (see face_make_brows) while `tilt` is
             * always expressed as "positive drops the inner end". Without the
             * mirror the face would look sad every time it meant to look
             * cross. */
            lv_obj_set_style_transform_rotation(g_face.brow[i],
                (i == 0 ? tilt : -tilt) * 10, LV_PART_MAIN);
          }
      }

    if (n->mouth != r->mouth)
      {
        face_show(g_face.mouth_arc,  n->mouth == FACE_MOUTH_ARC);
        face_show(g_face.mouth_line, n->mouth == FACE_MOUTH_LINE);
        face_show(g_face.mouth_ring, n->mouth == FACE_MOUTH_RING);
      }

    if (n->mouth == FACE_MOUTH_ARC && n->mouth_curve != r->mouth_curve)
      {
        /* LVGL angles run clockwise from 3 o'clock with y downward, so 90 is
         * the bottom of the circle: an arc straddling 90 opens upward (a
         * smile) and one straddling 270 opens downward (a frown). */
        int curve = n->mouth_curve;
        int sweep = face_mouth_sweep(curve);

        if (curve >= 0)
          {
            lv_arc_set_angles(g_face.mouth_arc, 90 - sweep, 90 + sweep);
          }
        else
          {
            lv_arc_set_angles(g_face.mouth_arc, 270 - sweep, 270 + sweep);
          }
      }

    g_fx_rendered = *n;
}

/****************************************************************************
 * Props -- the thought you can see above the face
 ****************************************************************************/

static void face_props_show(void)
{
    int i;

    for (i = 0; i < 3; i++)
      {
        face_show(g_face.dot[i],    g_prop == PROP_DOTS);
        face_show(g_face.bubble[i], g_prop == PROP_BUBBLES);
      }

    face_show(g_face.vol_arc, g_prop == PROP_VOLUME);
    face_show(g_face.bell,    g_prop == PROP_BELL);
    face_show(g_face.hook,    g_prop == PROP_HOOK);
    face_show(g_face.hand,    g_prop == PROP_HAND);
}

static void face_prop_update(int64_t now)
{
    int cx = g_face_cx;
    int cy = g_geo.prop_cy;
    int i;

    /* Restart the phase whenever the prop changes, so each one animates from
     * its own beginning instead of inheriting a half-finished cycle. */
    if (g_prop != g_prop_prev)
      {
        face_props_show();
        g_prop_prev = g_prop;
        g_prop_phase = 0;
        g_prop_dots_angle = 0;
        g_prop_hold = 0;
      }

    if (g_prop == PROP_NONE)
      {
        return;
      }

    switch (g_prop)
      {
        case PROP_DOTS:
          {
            /* One revolution every 6 s, with a half-second pause on each lap.
             * A constant spinner stops looking like thinking somewhere around
             * the twentieth second -- and thinking is the state the child
             * stares at longest -- so the pause is doing real work. */
            if (g_prop_hold > 0)
              {
                g_prop_hold--;
              }
            else
              {
                g_prop_dots_angle += 6;

                if (g_prop_dots_angle >= 360)
                  {
                    g_prop_dots_angle = 0;
                    g_prop_hold = 5;
                  }
              }

            for (i = 0; i < 3; i++)
              {
                int a = g_prop_dots_angle + i * 120;
                int r = g_geo.prop_r;
                int s = g_geo.dot_d;

                lv_obj_set_pos(g_face.dot[i],
                               cx + r * face_cos(a) / 1000 - s / 2,
                               cy + r * face_sin(a) / 1000 - s / 2);
              }
          }
          break;

        case PROP_VOLUME:
          {
            /* Straight from the VAD's own energy figure -- no extra ADC work,
             * which is why this is the one prop that can afford to track a
             * signal that changes every 20 ms. */
            int msq = g_obs_mic_msq;
            int v   = (msq >= FACE_VOL_FULL_MSQ) ? 100
                                                : msq * 100 / FACE_VOL_FULL_MSQ;

            if (v != (int)lv_arc_get_value(g_face.vol_arc))
              {
                lv_arc_set_value(g_face.vol_arc, v);
              }
          }
          break;

        case PROP_BELL:
          {
            /* Swinging, and running down. A bell rung at a constant rate reads
             * as a metronome; one that decays reads as an event. */
            int64_t left = g_face_bell_until - now;
            int env = (left > 0) ? (int)(left * 100 / FACE_BELL_HOLD_MS) : 0;
            int ph  = g_prop_phase % 30;
            int sw  = (ph < 15 ? ph : 30 - ph) - 7;   /* -7 .. +7 */

            lv_obj_set_style_transform_rotation(g_face.bell,
                                                sw * env * 3 / 14, LV_PART_MAIN);
          }
          break;

        case PROP_BUBBLES:
          {
            /* Rising and recycling. The fade at both ends hides the wrap, and
             * opacity is the cheap way to do it: plain `opa` only scales the
             * alpha of the fill, it does not push the object onto a temporary
             * draw layer (that is opa_layered, which this file never sets). */
            for (i = 0; i < 3; i++)
              {
                int span = 60 + i * 12;
                int ph   = (g_prop_phase * (2 + i) / 2) % span;
                int t    = ph * 1000 / span;
                lv_opa_t opa;

                /* Travel is 2*prop_r, not the 3*prop_r the span suggests: the
                 * band above the brows only has room for that much, and the
                 * largest bubble is 4*bub_d across. Sized off prop_r so the
                 * whole run stays inside it. */
                lv_obj_set_pos(g_face.bubble[i],
                               cx + (i - 1) * g_geo.prop_r * 4 / 3,
                               cy + g_geo.prop_r * 3 / 2
                                    - ph * (g_geo.prop_r * 2) / span);

                if (t < 150)
                  {
                    opa = (lv_opa_t)(t * 255 / 150);
                  }
                else if (t > 650)
                  {
                    opa = (lv_opa_t)((1000 - t) * 255 / 350);
                  }
                else
                  {
                    opa = LV_OPA_COVER;
                  }

                lv_obj_set_style_opa(g_face.bubble[i], opa, LV_PART_MAIN);
              }
          }
          break;

        case PROP_HOOK:
          {
            int bob = face_sin(g_prop_phase * 9) * g_fh / 25000;

            lv_obj_set_y(g_face.hook, cy - g_fh * 11 / 100 + bob);
          }
          break;

        case PROP_HAND:
          {
            /* Waving, and putting the hand down again when the greeting's
             * window runs out. */
            int64_t left = g_face_wave_until - now;
            int env = (left > 0) ? (int)(left * 100 / FACE_HAPPY_HOLD_MS) : 0;
            int sw  = face_sin(g_prop_phase * 13);

            lv_obj_set_style_transform_rotation(g_face.hand,
                                                sw * env * 2 / 1000, LV_PART_MAIN);
          }
          break;

        default:
          break;
      }

    g_prop_phase++;
}

/****************************************************************************
 * The state machine
 ****************************************************************************/

/* Expression targets, named so the intent is readable in the ladder below.
 * The numbers are the design: 100 is a normal open eye, 140 is the wide-eyed
 * startle, 45 is a tired lid, and mouth_curve runs -100 (frown) to +100. */
static void face_set_calm(face_expr_t *t)
{
    t->eye_open = 100; t->brow_dy = 0;
    t->brow_tilt = 0;  t->brow_tilt_r = 0;
    t->mouth = FACE_MOUTH_ARC; t->mouth_curve = 35;
}

static void face_apply(void)
{
    static int busy_prev     = 0;
    static int speaking_prev = 0;
    static int report_prev   = 0;

    int64_t now   = now_ms();
    int64_t backoff;
    face_expr_t t;
    bool drift_ok = false;
    bool blink_ok = true;

    if (!g_face_ready)
      {
        return;
      }

    /* Read the 64-bit cross-thread stamp once, into a local. The wake loop
     * writes it from another thread, and on a 32-bit target a torn read here
     * could produce a nonsense deadline; a single copy still races, but a race
     * now costs one wrong frame instead of a weird one. */
    backoff = g_asr_backoff_until;

    /* ── 1. Turn worker-thread edges into timed windows ────────
     * Everything below this line runs on the LVGL thread only. */

    if (g_obs_turn_failed)
      {
        /* With no text on screen this is the ONLY failure report the child
         * gets. The agent dresses its errors up as ordinary replies (see the
         * v2.63 fallback), so if the face does not go sad here, a broken turn
         * is indistinguishable from a working one. */
        g_obs_turn_failed = 0;
        g_face_sad_until  = now + FACE_SAD_HOLD_MS;
      }

    if (g_obs_asr_empty)
      {
        g_obs_asr_empty       = 0;
        g_face_confused_until = now + FACE_CONFUSED_HOLD_MS;
      }

    /* The reminder bell needs an edge, not a level: g_report_pending stays up
     * until the child acknowledges, which can be minutes, so keying the bell
     * off the flag itself would ring for the whole time. */
    if (g_report_pending && !report_prev)
      {
        g_face_bell_until = now + FACE_BELL_HOLD_MS;
      }
    report_prev = g_report_pending;

    /* A proactive turn finishing is the one moment the toy reaches out of its
     * own accord, and the design gives it a wave. */
    if (busy_prev && !g_reply_busy && g_turn_proactive)
      {
        g_face_happy_until = now + FACE_HAPPY_HOLD_MS;
        g_face_wave_until  = now + FACE_HAPPY_HOLD_MS;
      }
    busy_prev = g_reply_busy;

    /* Speech ended, so the reply landed. With no text this is how the child
     * knows the answer is complete rather than still coming. */
    if (speaking_prev && !g_obs_speaking)
      {
        g_face_happy_until = now + FACE_HAPPY_HOLD_MS;
      }
    speaking_prev = g_obs_speaking;

    /* ── 2. Pick the face ──────────────────────────────────────
     * An if/else ladder rather than a state variable: these conditions are not
     * mutually exclusive in reality (the board can be offline AND have just
     * failed), so this order IS the policy. Highest wins, top to bottom. */

    g_prop = PROP_NONE;
    face_set_calm(&t);

    if (backoff != 0 && now < backoff)
      {
        /* The cloud ASR is rate-limiting us. The design asks for a still face
         * here, which is also the cheapest state to render -- exactly what a
         * board being refused service should be doing. */
        t.eye_open = 45;   t.brow_dy = -5;
        t.brow_tilt = -18; t.brow_tilt_r = -18;
        t.mouth_curve = -45; t.pupil_dy = 4;
        blink_ok = false;
      }
    else if (now < g_face_sad_until)
      {
        /* NEGATIVE tilt: inner ends UP. This is the one state where getting
         * the sign backwards is invisible in review and wrong on the panel --
         * a "sad" face wearing an angry brow reads as the toy being cross with
         * the child, which is the exact opposite of what a failed reply should
         * say. Verified against a magnified render. */
        t.eye_open = 80;   t.brow_dy = 4;
        t.brow_tilt = -26; t.brow_tilt_r = -26;
        t.mouth_curve = -60; t.pupil_dy = 5;
        blink_ok = false;
      }
    else if (now < g_face_bell_until)
      {
        t.eye_open = 140;  t.brow_dy = 12;  t.mouth = FACE_MOUTH_RING;
        t.brow_tilt = -12; t.brow_tilt_r = -12;
        g_prop = PROP_BELL;
      }
    else if (now < g_face_confused_until)
      {
        /* Asymmetric on purpose: left brow flat, right brow cocked. A single
         * raised brow is the only thing that reads as puzzlement -- both brows
         * raised together reads as sadness, which is already taken by
         * g_face_sad_until three branches up. */
        t.eye_open = 115;  t.brow_dy = 8;
        t.brow_tilt = 0;   t.brow_tilt_r = -20;
        t.mouth_curve = -20; t.pupil_dx = g_geo.pupil_d / 5;
        g_prop = PROP_HOOK;
      }
    else if (g_obs_speaking)
      {
        /* Talking. The mouth alternates between the two pre-made shapes every
         * 3 ticks -- 300 ms per change, so a full open/close cycle is 600 ms.
         * Nothing is resized: resizing a rounded object changes its mask
         * radius, and the mask cache is keyed on radius alone.
         *
         * This is deliberately the only thing moving. Speaking is the one
         * state where SPI1 is competing with the codec for DMA (see the FRAME
         * BUDGET note), so it gets the fewest frames in the whole design. */
        face_set_calm(&t);
        t.mouth = ((g_face_tick / 3) & 1) ? FACE_MOUTH_RING : FACE_MOUTH_LINE;
        t.pupil_dy = g_geo.pupil_d / 5;
      }
    else if (g_mic_open && g_obs_mic_speech)
      {
        /* Hearing a voice. The pupil looks down and to the side, which is the
         * cheapest possible "I am attending to you". */
        t.eye_open = 85;   t.mouth = FACE_MOUTH_LINE;
        t.pupil_dy = g_geo.pupil_d / 4;  t.pupil_dx = g_geo.pupil_d / 6;
        g_prop = PROP_VOLUME;
      }
    else if (g_obs_asr_inflight)
      {
        t.eye_open = 90;   t.mouth = FACE_MOUTH_LINE;
        t.pupil_dx = -g_geo.pupil_d / 4; t.pupil_dy = -g_geo.pupil_d / 4;
        g_prop = PROP_DOTS;
      }
    else if (g_reply_busy)
      {
        /* A local line (the boot greeting, the wake-word acknowledgement) is
         * spoken without the model ever being asked, and g_llm_turn is what
         * tells them apart -- otherwise the face would sit there thinking
         * while the board is in fact already talking. */
        if (g_llm_turn)
          {
            t.eye_open = 90;  t.mouth = FACE_MOUTH_LINE;
            t.pupil_dx = g_geo.pupil_d / 4;  t.pupil_dy = -g_geo.pupil_d / 5;
            g_prop = PROP_DOTS;
            drift_ok = true;
          }
        else
          {
            t.mouth = FACE_MOUTH_LINE;
            t.pupil_dy = g_geo.pupil_d / 5;
          }
      }
    else if (!g_boot_greeted)
      {
        /* Coming up. Tired, and not yet pretending to be a playmate. */
        t.eye_open = 60;   t.brow_dy = -4;  t.mouth_curve = 15;
      }
    else if (!g_agent_connected || !network_is_connected())
      {
        /* Offline is DROWSY, not sad. Being unable to help is not the same as
         * having done something wrong, and a sad face here would tell a child
         * they had broken the toy. */
        t.eye_open = 35;   t.mouth = FACE_MOUTH_LINE;
        t.brow_dy = -3;
        g_prop = PROP_BUBBLES;
        blink_ok = false;
      }
    else if (now < g_face_happy_until)
      {
        t.eye_open = 95;   t.brow_dy = 6;   t.mouth_curve = 85;
        drift_ok = true;

        if (now < g_face_wave_until)
          {
            g_prop = PROP_HAND;
          }
      }
    else
      {
        /* Idle. Static except for the gaze and the blink below -- this is the
         * state the board lives in, so it has to cost nothing. */
        drift_ok = true;
      }

    /* ── 3. Blink and gaze ─────────────────────────────────────
     * Both are scheduled here rather than in a second timer so that there is
     * exactly one place that decides what the face does this tick. */

    if (blink_ok && now >= g_blink_at)
      {
        g_blink_at    = now + FACE_BLINK_MIN_MS + (rand() %
                        (FACE_BLINK_MAX_MS - FACE_BLINK_MIN_MS));
        g_blink_until = now + FACE_BLINK_CLOSE_MS;
      }

    if (blink_ok && now < g_blink_until)
      {
        /* Forced into both copies: easing a 100 ms blink would smear it into a
         * slow droop. Reopening is left to ease, which is what an eyelid does
         * anyway. */
        t.eye_open        = 0;
        g_fx_now.eye_open = 0;
      }

    if (now >= g_look_at)
      {
        g_look_at = now + FACE_LOOK_MS;

        if (drift_ok)
          {
            g_look_dx = ((rand() % 3) - 1) * (g_geo.pupil_d / 4);
            g_look_dy = ((rand() % 3) - 1) * (g_geo.pupil_d / 5);
          }
        else
          {
            g_look_dx = 0;
            g_look_dy = 0;
          }
      }

    t.pupil_dx += g_look_dx;
    t.pupil_dy += g_look_dy;

    /* ── 4. Ease, render, animate the prop ─────────────────────
     * Ease toward the target so expressions change rather than snap. Integer
     * thirds: it converges in about a dozen ticks and, unlike a fixed-point
     * setup, cannot drift. */
    g_fx_target = t;

    {
      int d;

      d = g_fx_target.eye_open - g_fx_now.eye_open;
      g_fx_now.eye_open = (d > -3 && d < 3) ? g_fx_target.eye_open
                                            : g_fx_now.eye_open + d / 3;

      d = g_fx_target.pupil_dx - g_fx_now.pupil_dx;
      g_fx_now.pupil_dx = (d > -2 && d < 2) ? g_fx_target.pupil_dx
                                            : g_fx_now.pupil_dx + d / 2;

      d = g_fx_target.pupil_dy - g_fx_now.pupil_dy;
      g_fx_now.pupil_dy = (d > -2 && d < 2) ? g_fx_target.pupil_dy
                                            : g_fx_now.pupil_dy + d / 2;

      d = g_fx_target.brow_dy - g_fx_now.brow_dy;
      g_fx_now.brow_dy = (d > -3 && d < 3) ? g_fx_target.brow_dy
                                           : g_fx_now.brow_dy + d / 3;

      d = g_fx_target.brow_tilt - g_fx_now.brow_tilt;
      g_fx_now.brow_tilt = (d > -4 && d < 4) ? g_fx_target.brow_tilt
                                             : g_fx_now.brow_tilt + d / 4;

      d = g_fx_target.brow_tilt_r - g_fx_now.brow_tilt_r;
      g_fx_now.brow_tilt_r = (d > -4 && d < 4) ? g_fx_target.brow_tilt_r
                                               : g_fx_now.brow_tilt_r + d / 4;

      d = g_fx_target.mouth_curve - g_fx_now.mouth_curve;
      g_fx_now.mouth_curve = (d > -5 && d < 5) ? g_fx_target.mouth_curve
                                               : g_fx_now.mouth_curve + d / 5;
    }

    /* The mouth SHAPE switches instantly -- it is a hidden-flag toggle between
     * three ready-made objects, and easing it would only mean writing a hidden
     * flag repeatedly for no visible gain. */
    g_fx_now.mouth = g_fx_target.mouth;

    face_render();
    face_prop_update(now);
    g_face_tick++;
}

/****************************************************************************
 * Face timer
 ****************************************************************************/

/* Walk the reply up one row every FACE_SCROLL_MS until its last row is the one
 * sitting on the bottom of the window, then go back to the top and start over.
 * Looping rather than parking at the end because the reply is often the whole
 * reason the child is looking at the screen, and a frozen plate is
 * indistinguishable from a board that locked up.
 *
 * Called every 100 ms; it only touches LVGL when it actually moves something.
 * That matters more than it looks: in FULL render mode every write here is a
 * 153 KB / ~31 ms full-screen flush on the same SPI bus the audio DMA uses. */
static void face_say_scroll(void)
{
    int64_t now;
    int max;

    if (!g_face_ready || g_face.say_lbl == NULL) { return; }

    /* How far down it is allowed to go: enough that the last row reaches the
     * bottom of the window, and never negative (a reply that fits in the two
     * visible rows has nowhere to go and the label sits at 0 forever). */
    max = g_say_rows - FACE_SAY_ROWS_VIS;
    if (max < 0) { max = 0; }

    /* A new turn shortened the reply under a running offset -- clamp rather
     * than wait for the next tick, or the label would sit on a blank strip
     * for up to FACE_SCROLL_MS. */
    if (g_say_off > max) { g_say_off = max; }

    now = now_ms();

    if (max > 0 && now - g_say_scroll_at >= FACE_SCROLL_MS)
      {
        g_say_scroll_at = now;
        g_say_off = (g_say_off >= max) ? 0 : g_say_off + 1;
      }

    /* Early-returns when the y is already right, so a still label costs
     * nothing here. */
    lv_obj_set_y(g_face.say_lbl, -g_say_off * FACE_LINE_H);
}

/* 100 ms = 10 fps. Not a preference: the frame budget is fixed by hardware.
 * One frame is 320x240x2 = 153,600 bytes, SPI1 runs at 40 MHz, so a single
 * full-screen flush is ~31 ms of pure bus time and the ceiling is roughly
 * 30 fps. Ten is where the animation still reads as alive with a wide margin,
 * and the states that really matter (thinking, listening) are the ones that
 * get it. See the FRAME BUDGET note above FACE_PERIOD_MS. */
static void face_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!g_face_ready)
      {
        ui_create_face();
      }

    face_apply();
    face_say_scroll();
}

/****************************************************************************
 * Seed the system clock
 ****************************************************************************/

/* This board has no RTC and nothing to read the time from at boot, so the
 * clock starts out at 1970.  mbedTLS then rejects every server certificate
 * on its notBefore check, and all the cloud calls (ASR / LLM / TTS) fail
 * with nothing but a TLS error to show for it.  Since there is no real time
 * source here, seed a fixed date instead -- but only while the clock is
 * still obviously unset, so a later NTP sync is never stomped.
 */

static void seed_clock_if_unset(void)
{
    const time_t sane = 1600000000; /* 2020-09-13: anything older means unset */
    struct timespec now;
    struct timespec ts;
    struct tm tm;
    time_t when;

    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && now.tv_sec >= sane)
      {
        syslog(LOG_INFO, "[kid_buddy] clock already sane, leaving it alone\n");
        return;
      }

    memset(&tm, 0, sizeof(tm));
    tm.tm_year  = 2026 - 1900;
    tm.tm_mon   = 9 - 1;           /* September */
    tm.tm_mday  = 10;
    tm.tm_hour  = 12;
    tm.tm_isdst = 0;

    /* Same conversion NSH's own `date -s` does, so the value matches what
     * the boot script used to hardcode. */

    when = mktime(&tm);

    ts.tv_sec  = when;
    ts.tv_nsec = 0;

    if (when < 0 || clock_settime(CLOCK_REALTIME, &ts) < 0)
      {
        syslog(LOG_ERR, "[kid_buddy] clock seed failed: %d\n", errno);
        return;
      }

    syslog(LOG_INFO, "[kid_buddy] no RTC, clock seeded to %s", ctime(&when));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;

    /* Both the display backend and the AI daemon need a sane clock, so do
     * this before anything else comes up. */

    seed_clock_if_unset();

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

    /* Step 4a: Find out how big the screen actually is.
     *
     * This file used to hard-code 240x320, but the panel is an ILI9341 driven
     * in LANDSCAPE (CONFIG_LCD_ILI9341_IFACE0_LANDSCAPE), which makes
     * drivers/lcd/ili9341.c swap its X and Y resolutions before handing them
     * to lv_nuttx_lcd -- so the real display is 320x240 and the old constants
     * had the 4th role card laid out at y=314 on a 240-pixel-tall screen.
     * Asking the driver instead of assuming costs one call and cannot be
     * wrong; and if the panel ever reports something else, the layout below
     * simply re-derives itself. */
    {
      lv_display_t *disp = lv_display_get_default();

      if (disp != NULL)
        {
          g_scr_w = (int)lv_display_get_horizontal_resolution(disp);
          g_scr_h = (int)lv_display_get_vertical_resolution(disp);
        }

      if (g_scr_w <= 0 || g_scr_h <= 0)
        {
          /* Nothing sane came back -- fall back to the driver's known values
           * so the face is at least laid out rather than divided by zero. */
          g_scr_w = 320;
          g_scr_h = 240;
        }
    }

    /* Step 4b: Report the GETAREAALIGN quirk.
     *
     * lv_nuttx_lcd.c calls this ioctl once, and if it fails it only calls
     * perror() and carries on with align_info left zeroed -- after which its
     * rounder_cb() rounds every flush area down to zero width and the screen
     * stays black forever, with the only clue buried in the log. No driver in
     * this tree implements getareaalign, so it is benign today, but that is
     * exactly the kind of thing that should be visible on the first boot
     * rather than rediscovered from a black screen two days before a deadline.
     * See lv_nuttx_lcd.c:210-212 and :124-140. */
    {
      struct lcddev_area_align_s align;
      int fd = open("/dev/lcd0", O_RDONLY);

      if (fd >= 0)
        {
          memset(&align, 0, sizeof(align));

          if (ioctl(fd, LCDDEVIO_GETAREAALIGN,
                    (unsigned long)(uintptr_t)&align) < 0)
            {
              syslog(LOG_WARNING,
                     "[kid_buddy] GETAREAALIGN failed (%d); LVGL flush areas "
                     "will be rounded to zero width -> blank screen\n", errno);
            }
          else
            {
              syslog(LOG_INFO,
                     "[kid_buddy] align row=%u h=%u col=%u w=%u buf=%u\n",
                     align.row_start_align, align.height_align,
                     align.col_start_align, align.width_align, align.buf_align);
            }

          close(fd);
        }
    }

    syslog(LOG_INFO, "[kid_buddy] GUI %s display %dx%d\n",
           KID_BUDDY_VERSION, g_scr_w, g_scr_h);

    /* Step 4c: Build the face. Replaces the old role-card / chat-screen pair
     * entirely. The only text on this UI is the 3-row subtitle plate at the
     * bottom (see face_make_panel) -- there is no status line, no role name. */
    ui_create_face();

    /* Step 5: Create poll timer to update UI from LLM callbacks */
    lv_timer_create(poll_timer_cb, 200, NULL);

    /* Step 5a: The face tick. 10 fps is a hardware limit, not a taste call --
     * see the FRAME BUDGET note above FACE_PERIOD_MS. */
    lv_timer_create(face_timer_cb, FACE_PERIOD_MS, NULL);

    /* Step 5a: Restore story mode BEFORE the boot probe fires, so a board
     * that was powered off mid-adventure resumes inside the story session. */
    story_mode_load();

    /* Step 5b: Boot greeting + proactive continuation — speaks a fixed local
     * welcome line a few seconds after startup, then hands over to the LLM
     * probe that resumes an unfinished story or opens the conversation. */
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
