# kid_buddy — AI 儿童陪伴（队伍 086）

一块 Gemini-S1 开发板，一个能听会说的儿童玩伴。

孩子喊一声「你好，openvela」把它唤醒，可以正常聊天问问题，也可以进入**角色扮演**：从老师、
故事家、科学家、朋友里挑一个角色，跟它一起走一段有分支的剧情。剧情进度按会话分开存，
孩子中途走开，过一会儿它会自己接着往下讲；剧情里开过的宝箱、拿走的东西、和居民立下的
约定也会记在板子上，下次回到星芽岛还认得。

- 赛道：AI 硬件产品创新
- 硬件：Gemini-S1（全志 R528 / sun8iw20，128M NAND，单麦 + 喇叭 + LCD）
- 应用版本：v2.93

屏幕上是一张**纯几何的脸**（圆 + 弧 + 圆角矩形画的六个表情 + 头顶道具，不带任何美术
素材），右缘四条书签带点一下换角色，底部三分之一是三行字幕板（孩子的话 + 回复正文）。

## 目录

```
app/kid_buddy/     作品主体：4200 多行 C + 5 份角色人设设计稿
board/nsh_minidisplay/
                   板级配置（defconfig + Make.defs），本仓自带即可编译
image/             预编译整机固件 + SHA256
phone-app/         家长端控制台（单文件网页，MQTT over WebSocket，板子零改动）
backend/           家庭账号后端 + 企业端页面（只做身份与路由，不存故事正文）
logs/              AI Coding 对话日志
docs/              技术报告、作品介绍、演示视频脚本、上游 PR 备忘
.claude/skills/    开发过程中沉淀的 4 个 Skill
upstream/          两个上游仓改动过的文件成品，由 manifest 覆盖回工作区
contest2026_086_ditiekouyegoulianmeng.xml
                   manifest，把 app/kid_buddy 软链进编译树、把 upstream/ 覆盖回上游仓
```

`app/kid_buddy` 经 manifest 的 `<linkfile>` 软链到 `packages/demos/contest2026_086_kid_buddy`，
再由 openvela 的 `Make.defs` 通配和 `mkkconfig` 自动发现 —— 不用把代码拷进生产仓的目录树。

作品对 `packages/ai_agent` 和 `vendor/allwinnertech` 有功能改动。改动过的文件成品放在
本仓 `upstream/` 下，由 manifest 在 `repo sync` 时覆盖回这两个仓的工作区 —— 所以
`repo status` 里这两个 project 显示 modified 是正常的，不是脏。见下面「上游依赖」和
「板级改动」。

## 家庭账号与企业端

每个家庭一个账号：家长登录后只看到自己家的板子，连接参数由后端下发，broker 上
每台设备一组独立凭据和 ACL，收不到别人家的消息。企业端（`/admin/`）能看家庭列表、
设备数、登录与操作审计，能停用账号、导出、删除家庭。

**后端不保存任何故事正文。** 表里没有对话字段，故事仍然只在板子 `/data` 和家长
手机里。这是刻意的数据边界，不是还没做。

部署、接口和 broker 配置见 [backend/README.md](backend/README.md)。不登录时家长页
照常可以手动连接，比赛演示不依赖账号服务。

## 跑起来

### 一、直接烧预编译固件（最快）

`image/kid_buddy_gemini-s1_uart0_128Mnand_v2.93.img` 是完整固件，用全志
PhoenixSuit / LiveSuit 按常规流程烧进 NAND 即可。上电后 `rcS.nsh` 会自动拉起
`kid_buddy`（图形界面）和 `ai_agent`（语音后台）。

应用版本号不再画在屏幕上（那个位置现在是书签带），改由开机日志打印，串口
（uart0，115200 8N1）上能看到：

```
[kid_buddy] GUI v2.93 display 320x240
[kid_buddy] face UI v2.93 on 320x240 (face band 162, panel 6,166 278x69, 33 cols)
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

`repo sync` 结束时 manifest 会做两件事：把 `app/kid_buddy` 软链到
`packages/demos/contest2026_086_kid_buddy`，把 `upstream/` 下的 46 个文件覆盖回
`packages/ai_agent/` 和 `vendor/allwinnertech/`（覆盖出来的文件是只读的，这是 `repo`
的行为）。所以这两步之后不需要再手工打补丁，直接 `build.sh`。

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

`app/kid_buddy` 用到 `velaclaw_ask_req_t.chat_id`、`velaclaw_publish()`、
`velaclaw_set_notify_callback()` 三个符号，都由 #35 引入。少了 #35 连编都编不过。

上游评审有 CODEOWNERS 闸门，参赛者合不了自己的 PR —— 截至交赛这 4 个 PR 都还在排队。
所以 `openvela.xml` 里 `packages/ai_agent` 走默认分支时是拿不到这些改动的。**本仓不
依赖上游合并**：`upstream/ai_agent/` 下放着改动过的 42 个文件的成品，manifest 用
`<copyfile>` 在 `repo sync` 时按原路径覆盖回 `packages/ai_agent/`。`repo sync` 之后的
树和 `image/` 里那份固件是同源的，直接编就行。

> 这里必须是 `<copyfile>`，不能换成 `<linkfile>`。linkfile 在目标位置已经有同名普通
> 文件时不会覆盖 —— repo 的 `removedirs()` 只清 symlink 和空目录，碰到普通文件就停下，
> 结果只在日志里留一行 `error: Cannot symlink ...`，树里躺的还是上游原文件。这 42 个
> 目标里有 38 个是上游本来就有的文件，用 linkfile 会静默失效。

PR 合进上游之后，`upstream/ai_agent/` 和 manifest 里对应的 `<copyfile>` 就可以删掉了。

## 板级改动

`vendor/allwinnertech` 改了 3 个文件（成品同样在 `upstream/vendor_allwinnertech/` 下，
由 manifest 覆盖回工作区，对应 PR
[#21](https://github.com/open-vela/vendor_allwinnertech/pull/21)）：

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

`logs/Rustypudding/` 下是开发全程的 Claude Code 对话记录，时间跨度 2026-07-25 到 09-20，
32 个有产出的日子、17,680 条事件（其中 11,340 条是工具调用）。应用本身、上游那 4 个 PR、
板级改动、界面重做，都是在这个流程里做出来的。整个开发过程在一个会话里连续完成，
所以只有一个 `.jsonl`，没有分段或筛选。详见 `logs/README.md`。

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
  当前版本 **v2.93**，在 v2.67 那版界面之上又叠了二十多个小版本：随机提醒、家长留言、
  故事书导出、孩子起名字、记住孩子的事、多孩子档案、睡前模式、摸头、
  固定故事世界、情绪转介、成长日报、回来玩邀请、外接灯带、公网 broker、
  故事多声线、v2.85 的**星芽岛世界状态**（剧情里开过的宝箱、拿走的东西、
  立下的约定记在 `/data/ai_agent/kid_buddy_world.txt`，下次回来还认得），
  v2.86 的**呼吸灯只在思考和回传故事书时呼吸，其余时间常亮**，
  v2.87 的**灯带每 100 ms 读回 PG13 的复用值、被抢走就改回 PWM2**，
  v2.88 的**灯带寄存器自证**（真机上 PWM2 的复用、时钟门、周期、占空比全部
  正确且计数在跑，脚上却没有电平），v2.89 的**开机自扫 PWM2 复用号**
  （读回引脚电平，结果证明这代 SoC 在复用模式下 DAT 不反映引脚），以及
  v2.90 的**慢速物理自检**（把 PG13 拉高 8 秒让人拿万用表找脚，再把复用号
  2~13 编成「闪 N 下」让人数），v2.91 的**外接灯带定型**（实测 PWM2 在
  PG13 上是 **mux 5** 而不是文档数出来的 6，改成 5 后灯带点亮，自检代码
  已删除），v2.92 的**呼吸起手不跳变**：开始思考时从常亮那一档接着
  往下走，先暗再亮，不再先「啪」一下暗掉；以及 v2.93 的两处修复：
  **故事书里补回孩子自己说的话**（会话文件里孩子那一行存的是整段提示词，
  老的 2048 字节行缓冲读不全就把整行丢掉了，只剩角色台词；现在缓冲按提示词
  上限给足，再只摘 `[USER]` 后面那句），和**家长端点「设定 / 留言」不再把
  MQTT 线程锁死**（回执原来在 `on_publish()` 回调里直接发，而那个回调持有
  MQTT-C 的非递归锁，一发就自己等自己；现在排进队列，等 `mqtt_sync()`
  返回、锁放开后由同步线程发出去）。家长端网页也一起改了排版：故事书和
  冒险记录都按「一句一行」排，正文和孩子台词共用一套字体，只靠「孩子：」
  前缀和颜色区分，不再像两份材料拼在一起。（网页改动不需要重烧，重新上传
  `phone-app/` 即生效。）
  这些**都没上板验证**，逐条清单在 [docs/上板验证清单.md](docs/上板验证清单.md)。
- **★ 已上板逐条验证通过的是 v2.67（2026-09-19）。** 脸谱界面（脸、字幕板、书签带、
  六个表情、全部道具、触摸、以及两条失败路径：这一轮请求失败 → 「难过 + 中文安抚语」，
  断网待机 → 「困倦 + 泡泡」）连同字幕滚动一起在真机上跑过，同批的回归项（唤醒、
  追问窗口、提醒到点、故事模式分叉、MQTT 上报）也一起过了。更早确认的是 v2.55
  上板复烧修好了白屏（板级配置误开 `CONFIG_LCD_FRAMEBUFFER` → `/dev/lcd0` 从未注册）；
  截至 v2.53 的各功能版本都在真机上跑过（MQTT + 角色扮演回路是 2026-09-09 验过的）。
- 对 v2.93 镜像做过的构造校验：版本号 `v2.93` 在镜像里**只出现一次**，旧号
  `v2.92` 一次都不出现；内建名 `kid_buddy` 在；新加的 `ack 队列满` 日志、
  故事书改写用的 `{"role":"user","content":"` 与 `%.*s","ts":%s`、以及原有的 `remember_world` /
  `kid_buddy_world.txt` / `世界状态：星芽岛` / `steady %d%%` 都查得到。v2.67 那次的校验项（`date -s` 一次都不出现、
  新唤醒词与音译别名在、脸谱开机日志格式串在）在这一版上沿用同一套代码，没有回退。
  （镜像里另有一处「小伙伴」，是开机欢迎语里的称呼，与唤醒词无关。）
- **回退**：`v2.60` ~ `v2.92` 的历史镜像都留在本机 `image/` 下（每份 27 MB，没有进仓），
  这一版上板有问题可以直接烧回去；最稳的退路是**已上板验证过的 v2.67**。
