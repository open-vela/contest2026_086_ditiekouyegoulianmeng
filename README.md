# kid_buddy — AI 儿童陪伴（队伍 086）

一块 Gemini-S1 开发板，一个能听会说的儿童玩伴。

孩子喊一声「你好，openvela」把它唤醒，可以正常聊天问问题，也可以进入**角色扮演**：从老师、
故事家、科学家、朋友里挑一个角色，跟它一起走一段有分支的剧情。剧情进度按会话分开存，
孩子中途走开，过一会儿它会自己接着往下讲。

- 赛道：AI 硬件产品创新
- 硬件：Gemini-S1（全志 R528 / sun8iw20，128M NAND，单麦 + 喇叭 + LCD）
- 应用版本：v2.67

屏幕上是一张**纯几何的脸**（圆 + 弧 + 圆角矩形画的六个表情 + 头顶道具，不带任何美术
素材），右缘四条书签带点一下换角色，底部三分之一是三行字幕板（孩子的话 + 回复正文）。

## 目录

```
app/kid_buddy/     作品主体：4200 多行 C + 5 份角色人设设计稿
board/nsh_minidisplay/
                   板级配置（defconfig + Make.defs），本仓自带即可编译
image/             预编译整机固件 + SHA256
phone-app/         家长端控制台（单文件网页，MQTT over WebSocket，板子零改动）
logs/              AI Coding 对话日志
docs/              作品介绍、演示视频脚本、上游 PR 备忘
.claude/skills/    开发过程中沉淀的 4 个 Skill
contest2026_086_ditiekouyegoulianmeng.xml
                   manifest，把 app/kid_buddy 软链进编译树
```

`app/kid_buddy` 经 manifest 的 `<linkfile>` 软链到 `packages/demos/contest2026_086_kid_buddy`，
再由 openvela 的 `Make.defs` 通配和 `mkkconfig` 自动发现 —— 不用把代码拷进生产仓的目录树。

作品对 `packages/ai_agent` 和 `vendor/allwinnertech` 确实有功能改动，但都走 PR，不直接
改这两个仓的本地副本，见下面「上游依赖」和「板级改动」。

## 跑起来

### 一、直接烧预编译固件（最快）

`image/kid_buddy_gemini-s1_uart0_128Mnand_v2.67.img` 是完整固件，用全志
PhoenixSuit / LiveSuit 按常规流程烧进 NAND 即可。上电后 `rcS.nsh` 会自动拉起
`kid_buddy`（图形界面）和 `ai_agent`（语音后台）。

应用版本号不再画在屏幕上（那个位置现在是书签带），改由开机日志打印，串口
（uart0，115200 8N1）上能看到：

```
[kid_buddy] GUI v2.67 display 320x240
[kid_buddy] face UI v2.67 on 320x240 (face band 162, panel 6,166 278x69, 33 cols)
```

这个镜像里已经包含了下面「上游依赖」和「板级改动」的全部内容，不需要再编任何东西。

### 二、从源码编

```bash
repo init -u https://github.com/open-vela/contest2026_086_ditiekouyegoulianmeng \
  -b dev-ai-contest-2026 -m contest2026_086_ditiekouyegoulianmeng.xml
repo sync -c -j8

# 在工作区根目录
./build.sh contest2026_086_ditiekouyegoulianmeng/board/nsh_minidisplay
```

`build.sh` 会拿板级 defconfig 覆盖 `.config` 后全量编译，结束时再把它写回那个目录。

板级配置放在**本仓**（`board/nsh_minidisplay/`）而不是 `vendor/allwinnertech` 里，是
因为这份配置和 vendor 仓里那份**不是同一份**：vendor 那份是目标分支的通用配置，而这份
是我们实际出固件用的——单麦增益为 1、关掉蓝牙整套、打开 `MBEDTLS_NET_C`。两者
有 555 项差异，混用会编不过或者编出行为不对的固件（我们踩过：从 vendor 那份生成
`.config` 会丢掉 `MBEDTLS_NET_C`，链接直接失败）。

`configure.sh` 支持任意含 `defconfig` + `Make.defs` 的目录当板级配置，所以这样编是
官方支持的用法，不是绕路。

编完还要打包。**这两步都得做**，只编译不打包，板子跑的还是旧镜像：

```bash
cd vendor/allwinnertech/lichee
./tools/scripts/pack_img.sh -c sun8iw20p1 -p rtos -b r528s3-gemini-s1 -o nuttx \
  -d uart0 -s none -m normal -w none -v none -i none -t "$PWD" \
  -f r528s3/gemini-s1_nand -g r528s3/gemini-s1_nand
```

产物落在
`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`。

> 如果改过 `packages/ai_agent` 的头文件（比如往结构体里加字段），记得先
> `make -C nuttx distclean` 再编。这套构建不跟踪 `.h` 依赖，留着旧 `.o` 会出现结构体
> 大小对不上的诡异问题。另外 `make clean` 在这个树里不是全量清理，`registry/` 和
> `builtin_list.c` 都留着，必须用 `distclean`。

## 上游依赖

本作品用到 `packages/ai_agent` 的 4 处改动，按主题拆成 4 个 PR 提到
`open-vela/packages_ai_agent` 的 `dev-ai-contest-2026` 分支：

| PR | 内容 |
| --- | --- |
| [#33](https://github.com/open-vela/packages_ai_agent/pull/33) | voice：MiMo ASR/TTS 后端、TTS 分句流水线、麦克风输入前端上电 |
| [#34](https://github.com/open-vela/packages_ai_agent/pull/34) | reminder：chunked 响应读完即停、TLS 逐 IP 回退重试 |
| [#35](https://github.com/open-vela/packages_ai_agent/pull/35) | rpg：流式工具调用、会话隔离用的 `chat_id`、本地客户端关掉回复缓存 |
| [#36](https://github.com/open-vela/packages_ai_agent/pull/36) | cli：`speak` / `thinking` 命令、MQTT 儿童上报、启动幂等 |

**只编译本仓的话，4 个里只有 #35 是硬依赖。** `app/kid_buddy` 用到
`velaclaw_ask_req_t.chat_id`、`velaclaw_publish()`、`velaclaw_set_notify_callback()`
三个符号，都由 #35 引入；少了 #35 会链接失败。另外三个不加也能编过，只是语音和提醒
链路不完整。

上游评审有 CODEOWNERS 闸门，参赛者合不了自己的 PR。截至交赛时这 4 个 PR 都还在排队，
所以**以 `image/` 里的预编译固件为准**。

## 板级改动

`vendor/allwinnertech` 改了 3 个文件：

- `boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig`
  打开 LVGL、CJK 字体、MQTT、NTP/DHCP 等；关键是
  `CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN=1` —— 单麦板子上增益必须是 1，用 Kconfig 默认的 6
  会把底噪放大几十倍，VAD 一路误触发，一直自己唤醒自己。
- `boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh`
  开机拉起 `kid_buddy` 和 `ai_agent`。（开发期这里曾硬编码过一行 `date -s` 把系统时钟设到
  一个固定日期，现在已经拿掉——设时钟改成由 kid_buddy 自己做了，见下面「已知限制」）
- `chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c`
  第二次打开播放设备时不再重复配 codec。重复配置会把 DAC 时钟打毛，表现是回复的第一句
  有声音、后面的追问全哑。

## AI Coding 日志

`logs/Rustypudding/` 下是开发全程的 Claude Code 对话记录，时间跨度 2026-07-25 到 09-12，
25 个有产出的日子。应用本身、上游那 4 个 PR、板级改动，都是在这个流程里做出来的。
（这份导出是 09-12 的快照；09-13 之后的界面重做不在其中，见 `manifest.json` 的
`updated_at`，提交前需要重跑一次归集工具。）

## 已知限制

- 语音链路全在云端（LLM / ASR / TTS），断网就只剩界面能用。
- 唤醒不是本地的声学唤醒（板子上没有 KWS 引擎），而是 VAD 掐出一句话、送 MiMo ASR
  转写后再做字符串匹配，所以环境吵的时候得离近点说。
- 唤醒词按大赛规定用「你好，openvela」。但转写是中英混说，ASR 输出的空格、大小写、
  标点都不固定，`openvela` 还可能被音译成「欧本维拉」，所以匹配时会把大小写、空格、
  标点全部忽略，并同时认几个常见音译写法。**这套写法只做过离线单测，没在真机上对着
  真实 ASR 输出校准过** —— 如果喊不醒，日志里 `wake: heard "..."` 会打出原始转写，
  照着往里加一条即可。
- 预编译固件是为交赛用本仓的板级配置从零编出来的一份完整镜像（不是开发过程中的旧产物）。
  当前版本 v2.67 是**界面整屏重做后的第四版**：v2.64 把深蓝卡片换成一张纯几何的脸，
  v2.65 在底部加回三行字幕板，v2.66 修字幕换行（回复里带换行时正文会溢出到第三行、
  被标签静默裁掉），v2.67 把字幕改成滚动（两行显示不下，原来直接省略号截掉，
  而提示词要求模型答 100 字，两行 33 格只有 66 格，大部分回复都断在半句）。
- **★ v2.67 已上板逐条验证通过（2026-09-19）。** 脸谱界面（脸、字幕板、书签带、
  六个表情、全部道具、触摸、以及断网时「难过 + 中文安抚语」这条失败路径）连同
  v2.67 新增的字幕滚动一起在真机上跑过，同批的回归项（唤醒、追问窗口、提醒到点、
  故事模式分叉、MQTT 上报）也一起过了。更早确认的是 v2.55 上板复烧修好了白屏
  （板级配置误开 `CONFIG_LCD_FRAMEBUFFER` → `/dev/lcd0` 从未注册）；截至 v2.53
  的各功能版本都在真机上跑过（MQTT + 角色扮演回路是 2026-09-09 验过的）。
- 对 v2.67 镜像做过的构造校验：版本号字符串在镜像里**只出现一次**且为 `v2.67`；内建名
  `kid_buddy` 在；**`date -s` 一次都不出现**（时钟自愈已从 vendor 的 `rcS.nsh` 挪进应用
  本身，所以从源码编出来的固件也覆盖得到）；新唤醒词（`你好openvela` / 音译 `欧本维拉`）
  都在，唤醒别名表里没有旧唤醒词；脸谱的开机日志格式串 `face UI %s on %dx%d` 也查得到。
  （镜像里另有一处「小伙伴」，是开机欢迎语里的称呼，与唤醒词无关。）
- **回退**：`v2.60` ~ `v2.66` 七份历史镜像留在本地（每份 27 MB，没有进仓），
  这一版上板有问题可以直接烧回去。
