# logs/ — AI Coding 日志

本目录存放 **086 队（地铁口野狗联盟）kid_buddy 作品开发过程中与 AI 工具的完整对话日志**，
与作品代码一并提交。**这不是示例，是真实开发记录**（示例的 `your-github-login/` 目录已删除）。

## 本队日志内容

| 项 | 值 |
| --- | --- |
| 提交者 | `Rustypudding` |
| 工具 | Claude Code（`claude-code`） |
| 时间跨度 | 2026-07-25 08:30 UTC → 2026-11-04 14:29 UTC |
| 活跃开发日 | 43 天 |
| 事件总数 | 29,162 条 |
| 其中工具调用 | 18,682 条 |
| 文件大小 | 约 82 MB |

整个开发过程在一个 Claude Code 会话中连续完成（session id
`583e52cf-…`），界面重做、上游 5 个 PR、全部上板排障和决赛版迭代都在里面，
没有分段或筛选。会话太长，单个文件会超过 GitHub 的 100 MB 上限，所以按
事件日期拆成了多个文件；组委会的校验工具会把同一会话的多个日期文件当作
一条连续的 `seq` 流来验。

## 目录结构

```text
logs/
└── Rustypudding/                                          # GitHub 用户名
    ├── manifest.json                                      # 会话清单（含事件总数）
    ├── 2026-09-12/
    │   └── claude-code__583e52cf-….jsonl                   # seq 0 ~ 17679
    ├── 2026-09-20/ … 2026-11-03/                           # 按天分段
    └── 2026-11-04/
        └── claude-code__583e52cf-….jsonl                   # 最后一段
```

- 目录名是该批事件所在的**事件日期**；`2026-09-12` 那批包含 07-25 起的
  全部早期记录（当时归集工具按写入日期建目录），后面的目录按天分。
- 每个 `.jsonl` 每行一个事件，字段定义见官方手册；只提交 JSONL 本身。

## 怎么核对

用组委会归集工具里的校验脚本跑一遍即可，期望输出 `✅ ALL OK`：

```bash
python3 tools/validate-log.py logs/
```

它会比对 `manifest.json` 里 `event_count` 与 JSONL 实际行数、检查 `seq` 是否连续、字段是否符合 schema。
本队当前提交的版本校验结果为 `ALL OK`（1 个文件 / 17,680 条事件）。

导出与提交的完整步骤、字段定义见
[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
