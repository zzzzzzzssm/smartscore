# P4 与音频 S3 通信协议

P4 使用独立 UART1 与音频 S3 双向通信：

| 端点 | UART | TX | RX | 参数 |
|---|---:|---:|---:|---|
| 音频 S3 | 1 | GPIO2 / H7-10 | GPIO1 / H7-8 | 460800, 8N1, 无流控 |
| ESP32-P4 | 1 | GPIO0 / J6-4 | GPIO1 / J6-6 | 460800, 8N1, 无流控 |

当前方向为 P4 GPIO0/J6-4 TX → S3 GPIO1/H7-8 RX、S3 GPIO2/H7-10 TX →
P4 GPIO1/J6-6 RX，两块板必须共地。

## 帧格式

协议为 NDJSON，每行一个 UTF-8 JSON 对象，以 `\n` 结束，最大 384 字节。公共
字段是：

```json
{"v":1,"type":"heartbeat","seq":10,"sid":0,"ts_ms":1000,"ready":true,"tx_drop":0}
```

音频 S3 当前发送 `hello`、`status`、`pong`、`heartbeat`、`pitch`、`note_on`、
`note_off` 和 `poly`。只有 `hello` 包含 `source:"s3_audio"`；其他消息不包含
`src`，P4 接收器按现有实际格式解析。

P4 将以下两类消息映射到现有 MIDI 跟谱和评分入口：

```json
{"v":1,"type":"note_on","seq":11,"sid":0,"ts_ms":1100,"midi":69,"velocity":100,"freq_hz":440.0,"confidence":0.95}
{"v":1,"type":"note_off","seq":12,"sid":0,"ts_ms":1600,"midi":69,"duration_ms":500,"reason":"silence"}
```

`pitch` 和 `poly` 当前只用于链路诊断，不直接推进跟谱或评分。

## P4 主轮询通信

S3 不再主动长期驱动 GPIO2。识别结果先进入 16 帧发送缓存，P4 每 50 ms
从 GPIO0 发送一次 `poll`；S3 收到后才短时启用 GPIO2，最多回复 4 帧，UART
发送完成后立即把 GPIO2 恢复为高阻输入。这样 P4 复位、烧录和 ROM 启动期间
S3 不会持续驱动 P4 RX。

P4 发送下列按行 JSON 命令：

```json
{"cmd":"poll"}
{"cmd":"ping"}
{"cmd":"start","sid":1}
{"cmd":"stop"}
```

没有待发送识别结果时，S3 用一帧 `heartbeat` 响应 `poll`；`ping`、`start`、
`stop` 的结果也先缓存，并由紧随其后的 `poll` 取回。因此合法响应同时证明
P4 GPIO0 → S3 GPIO1 和 S3 GPIO2 → P4 GPIO1 两个方向均可用。

P4 初始化时先发送 `stop` 和 `ping`，避免未选择音频输入时 S3 事件干扰 USB
MIDI。选择 `audio_s3` 并开始练习时发送带新 `sid` 的 `start`，结束练习后发送
`stop`。如果 S3 在 P4 之后重启，P4 收到新的 `hello` 会重新同步期望的流状态并
再次 `ping`。

## 接收安全

- 错误 JSON、未知类型、缺失字段和越界值直接丢弃。
- 超过 384 字节后丢弃整行，在下一个换行恢复。
- 同一 `sid` 只接受递增 `seq`，重复或倒序消息不重复执行业务。
- `hello` 或 `sid` 变化会建立新的序号基线。
- 音符事件通过独立队列和派发任务进入业务层，UART RX 不等待评分或显示锁。
- 最近 3 秒内收到合法轮询响应时，链路状态为在线。
- 收到 `pong` 后 `command_link_confirmed=true`，证明 P4 TX → S3 RX 方向可用。

P4 使用本地接收时间作为 MIDI 业务时间戳，S3 的 `ts_ms` 仅保留用于诊断。

## 联调检查

1. P4 启动日志应显示 `master-poll UART1 TX=GPIO0 RX=GPIO1 at 460800 baud (50 ms)`。
2. S3 启动日志应显示 `poll-response UART1 TX=GPIO2(high-Z idle) RX=GPIO1 at 460800 baud`。
3. 空闲时 S3 的 `status.stream_enabled` 应为 `false`，P4 轮询仍会取得 heartbeat。
4. 选择 S3 音频并开始练习后应收到 `status`（新 `sid`、流已开启），随后
   `note_on/note_off` 能进入跟谱和评分。
5. 结束练习后应收到停止状态；断开任一 TX/RX 可分别验证两个方向。
