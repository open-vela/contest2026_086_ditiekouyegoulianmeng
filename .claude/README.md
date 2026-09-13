# 本队的 AI Skills

比赛要求「沉淀至少 1 个有效 Skill」。官方技能集（`openvela/.claude`）已经覆盖了
openvela 开发的通用路径——编译、写驱动、审驱动、分析 PCM 质量、提 PR，这些我们都
在用，没有必要重写一遍。

所以这里放的是**官方没有、而我们踩了两周才搞明白的东西**：那些「文档里不写、报错信息
也看不出来、但一定会再遇到」的坑。

## 技能列表

| 技能 | 解决什么 |
| --- | --- |
| [openvela-build-traps](skills/openvela-build-traps/) | 构建/配置的隐蔽陷阱：陈旧 `.o` 导致的 ABI 错乱、`clean` 不是全量清理、`build.sh` 会覆盖你的 defconfig、`distclean` 之后第三方 app 补丁打不上 |
| [ai-agent-board-bringup](skills/ai-agent-board-bringup/) | 把 `packages/ai_agent` 在真板上跑通：配置矩阵、TLS 多 IP 回退、chunked 读停顿、MQTT 双线程互踢、dispatch 头阻塞 |
| [voice-loop-tuning](skills/voice-loop-tuning/) | 语音回路调参：单麦增益、mic/speaker 的 DMA 争用、codec 二次打开无声、唤醒词匹配 |
| [openvela-contest-submit](skills/openvela-contest-submit/) | 提 PR 前必须知道的闸门：ruleset 与 branch protection 的区别、CLA 邮箱、只能 rebase 合并导致 SHA 重写、checkpatch 只查 `Change-Id` |

## 怎么用

把本仓克隆进 openvela 工程根目录，或者直接把 `.claude/` 拷过去：

```bash
git clone https://github.com/open-vela/contest2026_086_ditiekouyegoulianmeng
cp -r contest2026_086_ditiekouyegoulianmeng/.claude <openvela 工程根>/.claude
```

Claude Code 启动时会自动发现 `skills/*/SKILL.md`，在相关任务里按 `description`
里的 `Use when:` 触发词选用。

## 每条技能是怎么来的

不是凭空写的，是从 `logs/` 里那些对话里提炼的——每条都对应一次真实故障：

- 结构体加字段后读出 240 这样的垃圾值 → 陈旧 `.o`（`kid_buddy` 的「太阳」bug）
- TTS 追问答完没声音 → codec `set_sysclk` 重复配置
- 提醒到点不响 → MQTT dispatch 单线程被头阻塞
- 喊不醒 → 单麦增益用了 Kconfig 默认的 6，底噪把 VAD 顶穿
- 推分支被拒 → 以为 branch protection 关着就没事，其实 ruleset 对所有分支生效

具体的日志日期和文件行号没写进技能文件——那些属于这个作品，不属于一条可复用的
技能。技能里留的是**判断依据和修法**。
