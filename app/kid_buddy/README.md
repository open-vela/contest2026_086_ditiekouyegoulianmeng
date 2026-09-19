# kid_buddy（应用形态 · 本队作品主体）

映射到 openvela `packages/demos/contest2026_086_kid_buddy`，由
`contest2026_086_ditiekouyegoulianmeng.xml` 里的 `<linkfile>` 软链进编译树。

## 是什么

运行在 **Gemini-S1（全志 R528）** 开发板上的 AI 儿童陪伴应用，LVGL 图形界面 +
语音交互。除常规问答外，核心是**多角色扮演（RPG）**：孩子可以用「你好，openvela」唤醒，
选择老师 / 故事家 / 科学家 / 朋友四个角色之一，进行**分支剧情**互动，剧情进度
按会话隔离保存，空闲时角色会主动续讲。

界面（v2.64 起）是**整屏一张纯几何的脸** —— 圆角矩形、圆和 `lv_arc` 拼出来的六个表情
加头顶道具，不带任何美术素材；右缘四条书签带点一下换角色，底部三分之一是三行字幕板
（孩子的话 + 回复正文）。整屏唯一的可点区域就是那四条书签带。

字幕正文（v2.67 起）放不下会**自己往上滚**：标签最多 8 行，前面是一个正好两行高的
窗口对象，`face_say_scroll()` 每 `FACE_SCROLL_MS` 把它往上移一行，走到底回到开头。
窗口靠 LVGL 只按矩形裁剪子对象来挡住外面的行——不要再给这个标签加
`LV_OBJ_FLAG_OVERFLOW_VISIBLE`，加了就等于把整段正文糊在字幕板上。

## 文件

| 文件 | 作用 |
| --- | --- |
| `kid_buddy.c` | 全部应用逻辑：LVGL 界面、唤醒词、ASR/TTS 调度、角色与剧情状态机 |
| `skills/*.md` | 四个角色的人设**设计稿**（英文）。⚠️ **运行时不读这些文件** —— 见下 |
| `Kconfig` | `CONFIG_KID_BUDDY_APP` 及其程序名 / 优先级 / 栈 / 数据目录 |
| `Makefile` / `Make.defs` | make 构建接线（openvela 默认构建方式） |
| `CMakeLists.txt` | cmake 构建接线（与 Makefile 等价） |

## `skills/` 与人设的真实关系

`skills/` 下的 5 份 Markdown 是**英文设计稿**（性格、知识范围、说话方式、安全边界、
示例对话）。真正注入提示词的是 `kid_buddy.c` 里 `g_roles[]` 的 `system_prompt`
中文字符串 —— 它在设计稿基础上改写，并按这个硬件收紧了约束：只讲简体中文、100 字
以内、禁 Markdown、禁任何引号（TTS 会把引号念出来）、孩子想玩故事时切换成故事主持人。

**两份是手工同步的**：改人设要改 `g_roles[]` 并重新编译，不加载文件、也不能只加一份
`.md` 就多一个角色。保留英文稿是因为它比压缩后的提示词完整，是改人设时的参照。

## 依赖

- `packages/ai_agent`：LLM / ASR / TTS / MQTT 通道。本作品对该公共仓的改动
  以**独立 PR 提交到 `open-vela/packages_ai_agent` 的 `dev-ai-contest-2026`
  分支**，见仓库根 `README.md`。
- `vendor/allwinnertech`：板级配置与 codec 驱动。改动同样以 PR 提交。
- `CONFIG_AI_AGENT_MQTT=y`、`CONFIG_NETUTILS_MQTTC=y`（家长端与儿童确认上报）
- `CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT=y`、`CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN=1`
  —— **单麦板子增益必须是 1**，设成 Kconfig 默认的 6 会把底噪放大 36 倍导致误唤醒

## 启用

```bash
# 工作区根目录
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay menuconfig
#   → Application Configuration → Demos → Kid Buddy - AI Role-Playing Companion
```

板级 defconfig 里已默认开启，一般无需手动改。
