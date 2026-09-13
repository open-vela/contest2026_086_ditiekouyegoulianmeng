# kid_buddy（应用形态 · 本队作品主体）

映射到 openvela `packages/demos/contest2026_086_kid_buddy`，由
`contest2026_086_ditiekouyegoulianmeng.xml` 里的 `<linkfile>` 软链进编译树。

## 是什么

运行在 **Gemini-S1（全志 R528）** 开发板上的 AI 儿童陪伴应用，LVGL 图形界面 +
语音交互。除常规问答外，核心是**多角色扮演（RPG）**：孩子可以用「你好，openvela」唤醒，
选择老师 / 故事家 / 科学家 / 朋友四个角色之一，进行**分支剧情**互动，剧情进度
按会话隔离保存，空闲时角色会主动续讲。

## 文件

| 文件 | 作用 |
| --- | --- |
| `kid_buddy.c` | 全部应用逻辑：LVGL 界面、唤醒词、ASR/TTS 调度、角色与剧情状态机 |
| `skills/*.md` | 四个角色的人设与剧情大纲，运行时载入并注入提示词 |
| `Kconfig` | `CONFIG_KID_BUDDY_APP` 及其程序名 / 优先级 / 栈 / 数据目录 |
| `Makefile` / `Make.defs` | make 构建接线（openvela 默认构建方式） |
| `CMakeLists.txt` | cmake 构建接线（与 Makefile 等价） |

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
