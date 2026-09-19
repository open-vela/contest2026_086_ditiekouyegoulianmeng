# 家长端控制台（phone-app）

家长在手机上打开的一个网页。用它给板子发消息、设提醒，并看板子回来的回复和回执。

和板子之间走 **MQTT**，板子侧零改动：板子照旧用 1883 明文 TCP 连 broker，这个网页走
**WebSocket**，只有在 broker 上开一个 WebSocket listener 需要配。

## 怎么用

1. 在 broker 上开 WebSocket listener。以 mosquitto 为例，在 `mosquitto.conf` 里加：

   ```
   listener 9001
   protocol websockets
   ```

2. 手机和 broker 在同一个局域网（或 broker 在公网）时，手机浏览器直接打开
   `index.html` 即可——单文件，没有构建步骤，也不需要装 App。
   可以「添加到主屏幕」当 PWA 用。

3. 页面上填：

   | 字段 | 说明 | 默认 |
   | --- | --- | --- |
   | Broker 地址（WebSocket） | 形如 `ws://192.168.1.100:9001` | 空 |
   | 我的 chat_id | 家长自己的会话标识，回执按它匹配 | `parent` |
   | 发送 topic（→板子） | 板子的入站 topic | `agent/in` |
   | 接收 topic（板子→） | 板子的出站 topic | `agent/out` |
   | 用户名 / 密码 | broker 要求认证时填，否则留空 | 空 |

   填过的设置存在浏览器 localStorage 里，下次打开还在。**密码不存**。

4. 点「连接」，状态灯变绿后就可以发消息了。

## 界面

- 底部四个快捷按钮：提醒喝水 / 提醒睡觉 / 打个招呼 / 看孩子回执
- 也可以直接在输入框里打任意一句话发给板子
- 上方日志区显示收发内容。板子回来的消息标「📥」，带 chat_id 的一并显示

## 消息格式

发给板子（`agent/in`）：

```json
{"type":"message","content":"提醒孩子 30 秒后喝水","chat_id":"parent"}
```

从板子收（`agent/out`）：

```json
{"type":"response","content":"……","chat_id":"parent"}
```

## 验证到什么程度

**全链路已跑通**（家长端 → 板子 → 家长端，再到点播报）：
从网页发「提醒孩子…喝水」→ 板子记住这条任务 → 板子回一条确认消息到网页 →
到设定时间板子开口语音提醒喝水。四个环节都实际走过。

另外，孩子听完提醒后说「知道了」触发的那条**儿童确认回执**（经 `agent/out` 发回、
按 chat_id 区分）在板子侧单独上板验证过。

## 注意

- 板子侧的 MQTT 通道默认是**关**的，需要在板级配置里同时打开
  `CONFIG_AI_AGENT_MQTT` 和 `CONFIG_NETUTILS_MQTTC`，并且删掉对应的 `.o` 强制重编，
  详见仓库根目录 README 的板级改动一节。
- 这个页面走的是**明文 ws://**。放公网用的话，需要给 broker 配 TLS 并把地址换成
  `wss://`，这一版没做。
- 页面依赖 CDN 上的 `mqtt.js`。完全离线的环境需要把这个文件下下来一起放。
