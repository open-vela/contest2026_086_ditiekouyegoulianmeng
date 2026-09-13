# 预编译固件（kid_buddy）

本目录存放**可直接烧录的整机固件**，用于在评委环境中复现作品，无需自行编译。

| 文件 | 说明 |
|---|---|
| `kid_buddy_gemini-s1_uart0_128Mnand_v2.53.img` | Gemini-S1（全志 R528）NAND 整机镜像，应用版本 `v2.53` |
| `kid_buddy_gemini-s1_uart0_128Mnand_v2.53.img.sha256` | 上者的 SHA256 校验和 |

## 镜像内容

- **应用主体**：本仓 `app/kid_buddy/`（LVGL 图形界面 + 语音交互 + 多角色文字 RPG）
- **上游依赖**：`packages_ai_agent` 的 4 个 PR 改动（#33 voice / #34 reminder / #35 rpg / #36 cli）
  在本镜像中**已编译进去**。这 4 个 PR 提交在 `open-vela/packages_ai_agent` 的
  `dev-ai-contest-2026` 分支上，评审合入前，直接按 `openvela.xml` 编译本仓会因缺少
  `velaclaw_ask_req_t.chat_id` / `velaclaw_publish()` / `velaclaw_set_notify_callback()`
  而链接失败 —— 本预编译镜像即为规避该依赖、保证可复现而提供。
- **板级改动**：`vendor/allwinnertech` 的 3 个文件（板级 defconfig、启动脚本 `rcS.nsh`、
  音频 codec 驱动），已随镜像生效。

## 烧录

镜像格式与全志官方整机包一致（`128Mnand` 分区布局），使用全志 PhoenixSuit / LiveSuit
按常规流程烧录：板子进入烧录（FEL）模式 → 选择本 `.img` → 烧录完成后重启。

启动后 `rcS.nsh` 会自动拉起 `kid_buddy`（图形界面）与 `ai_agent`（语音/对话后台）。

## 校验

```bash
sha256sum -c kid_buddy_gemini-s1_uart0_128Mnand_v2.53.img.sha256
```
