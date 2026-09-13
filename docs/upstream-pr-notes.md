# 上游 PR 备忘

本作品对 `open-vela/packages_ai_agent` 和 `open-vela/vendor_allwinnertech` 两个公共仓
有改动，都按大赛规则走 fork + PR 到 `dev-ai-contest-2026` 分支。

这份文件是**给我们自己看的**：PR 的清单、依赖关系、以及待贴在 PR 下的评论正文。
（原稿存在 `/tmp`，被系统清掉了，这里是重写的版本。）

## PR 清单

| PR | 仓 | 主题 |
| --- | --- | --- |
| [#33](https://github.com/open-vela/packages_ai_agent/pull/33) | packages_ai_agent | voice |
| [#34](https://github.com/open-vela/packages_ai_agent/pull/34) | packages_ai_agent | reminder |
| [#35](https://github.com/open-vela/packages_ai_agent/pull/35) | packages_ai_agent | rpg |
| [#36](https://github.com/open-vela/packages_ai_agent/pull/36) | packages_ai_agent | cli |
| vendor | vendor_allwinnertech | codec 修复 + 板级支持（2 个提交） |

vendor PR 开在这里：

```
https://github.com/open-vela/vendor_allwinnertech/compare/dev-ai-contest-2026...Rustypudding:gemini-s1/kid-buddy-upstream?expand=1
```

正文见 `~/pr-drafts/vendor-pr.md`。**只能 rebase 合并，且需 1 个批准 + CODEOWNERS 通过。**

## 依赖关系（逐符号核实过）

队伍仓 `app/kid_buddy/kid_buddy.c` 里「只有 PR 之后才存在」的符号，**只有 3 个，全在 #35**：

| 符号 | 来自 |
| --- | --- |
| `velaclaw_ask_req_t.chat_id`（结构体新字段，改大小） | #35 |
| `velaclaw_publish()` | #35 |
| `velaclaw_set_notify_callback()` | #35 |

→ **#35 是硬依赖**，#33 / #34 / #36 不是。

板级 defconfig 用到的 `CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT` /
`CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN` / `CONFIG_AI_AGENT_MQTT` 在基线 `31faed70`
的 Kconfig 里**全都有**；voice PR 对 Kconfig 的唯一改动只是一行 `select NETUTILS_CJSON`，
没引入新符号。

## 待贴的评论正文

贴之前把开头的 `@` 那行去掉——GitHub 的 review 组不是这样 @ 的，直接说要谁看就行；
或者按组委会给的联系方式反馈。

### 贴到 #33 / #34 / #36（非硬依赖的三个）

```
这是 2026 openvela AI 硬件开发者大赛 086 队（kid_buddy）的作品改动之一，
按主题拆成了 4 个 PR：#33 voice / #34 reminder / #35 rpg / #36 cli。

本 PR 不是另外三个的依赖，单独合入即可，互不影响。

配套的另外两处改动在别的仓，不在本 PR 里：

- open-vela/contest2026_086_ditiekouyegoulianmeng 的 app/kid_buddy/
  （作品主体，LVGL 前端 + 角色扮演状态机）
- open-vela/vendor_allwinnertech 的 r528s3-gemini-s1 板级支持
  （Gemini-S1 / 全志 R528）

其中 app/kid_buddy 对 #35 是硬依赖：它用到 #35 引入的
velaclaw_ask_req_t.chat_id、velaclaw_publish()、velaclaw_set_notify_callback()。
本 PR 与另一个 PR 之间没有依赖。

改动的来龙去脉、验证方式和已知限制写在队伍仓的 README 里。
我们手上没有 dev-ai-contest-2026 分支对应的 nuttx/apps 树，本地无法跑全量构建，
如果 CI 报错请指出，我们按目标分支调整。
```

### 贴到 #35（硬依赖）

```
这是 2026 openvela AI 硬件开发者大赛 086 队（kid_buddy）的作品改动之一，
按主题拆成了 4 个 PR：#33 voice / #34 reminder / #35 rpg / #36 cli。

⚠️ 本 PR 是队伍仓 app/kid_buddy 的**硬依赖**：作品用到本 PR 引入的

- velaclaw_ask_req_t.chat_id（结构体新字段，改了大小）
- velaclaw_publish()
- velaclaw_set_notify_callback()

少了本 PR，队伍仓的 app/kid_buddy 会链接失败。另外三个 PR（#33 / #34 / #36）
不是依赖，可以各自独立评估。

配套的板级支持在 open-vela/vendor_allwinnertech（Gemini-S1 / 全志 R528），
作品主体在 open-vela/contest2026_086_ditiekouyegoulianmeng 的 app/kid_buddy/。

改动的来龙去脉、验证方式和已知限制写在队伍仓的 README 里。
我们手上没有 dev-ai-contest-2026 分支对应的 nuttx/apps 树，本地无法跑全量构建，
如果 CI 报错请指出，我们按目标分支调整。
```

## 别忘了

- 首次向这些仓提 PR 会跑 `cla/signature`。CLA 已经用 `dokipudding@outlook.com`
  签过了；如果新 PR 又卡这个检查，在 PR 下评论 `/check-cla` 触发复检，不用重建 PR。
- 生产仓有 CODEOWNERS 闸门，**我们自己合不了**。放着就行，不要一直等。
- 交赛截止前这些 PR 没合入也不影响提交——`.img` 才是实际交付方式，上游 PR 是
  获奖后按要求再做。
