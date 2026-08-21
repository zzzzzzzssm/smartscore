# P4 与音频 S3 通信协议

## 物理链路

两端均使用 UART1、115200 baud、8N1、无流控：

| 端点 | TX | RX |
|---|---:|---:|
| 音频 S3 | GPIO1 / H7-23 / U2.39 | GPIO2 / H7-25 / U2.38 |
| ESP32-P4 | GPIO0 / J6-4 | GPIO1 / J6-6 |

接线为 P4 GPIO0/TX → S3 GPIO2/RX、S3 GPIO1/TX → P4 GPIO1/RX，并共地。
H7 针号来自 `Netlist_PCB1_2026-08-19.tel`；旧记录中的 460800 baud、反向
GPIO 方向和 H7-8/H7-10 均已废弃。实物接线前仍需按连接器丝印确认 H7 编号方向。

## 帧和轮询

链路使用 NDJSON，每行一个 JSON 对象并以 `\n` 结束，单行上限 384 字节。
P4 每 50 ms 发送轮询。S3 只在收到轮询后回复，最多发送四帧。

S3 的兼容 hello 使用 v1，并声明 v2 能力：

```json
{"v":1,"type":"hello","seq":1,"sid":0,"ts_ms":100,"source":"s3_audio","sample_rate":24000,"a4":440.0,"mic":"dual","capabilities":["note_set_v2"],"midi_min":36,"midi_max":96,"max_polyphony":4}
```

普通练习启动 61 键正式识别：

```json
{"cmd":"start","sid":12,"protocol":2,"profile":"performance"}
```

P4 演奏/音符判断测试页保留较宽松的演示识别参数，但同样使用 v2 快照：

```json
{"cmd":"start","sid":13,"protocol":2,"profile":"demo"}
```

未请求 protocol 2 的旧控制器仍可使用 v1 `note_on`/`note_off`。当前 P4 固件
请求 v2，避免把复音压缩成一个旋律音。

## MusicLink v2 琴键快照

S3 在活动集合变化时生成完整快照：

```json
{"v":2,"type":"notes","seq":108,"sid":12,"state_id":35,"ts_ms":4260,"midis":[48,52,55],"velocities":[91,78,84],"confidences":[0.950,0.890,0.920],"set_confidence":0.920,"degraded_mic":false,"overflow":false}
```

约束：

- 三个数组长度相同，长度 0–4；空数组表示全部释放；
- MIDI 严格递增且位于 36–96，velocity 位于 1–127；
- confidence 必须是有限的 0–1 数值；
- `state_id` 在一个 sid 内从 1 单调增加；
- `degraded_mic=true` 表示当前只有一路健康麦克风参与；
- `overflow=true` 表示检测到超过四键的非支持输入；
- 同一集合持续存在不会只因 RMS 或置信度浮动产生新 state_id。

P4 每次轮询确认最后一次完整接收并原子处理的状态：

```json
{"cmd":"poll","sid":12,"ack_state_id":35}
```

S3 在确认前重复最新快照。重复快照只更新确认号，不重复产生业务事件。更新后的
完整快照可以覆盖旧的未确认状态。

## P4 集合差分与恢复

P4 严格校验 v2 数组、范围、有限数、sid、seq 和 state_id。合法新快照与本地
活动集合比较：

1. 用同一个 S3 `ts_ms` 先产生所有消失键的 `note_off`；
2. 再产生所有新增键的 `note_on`；
3. 保留键不重复起音；
4. 整批事件进入现有评分、跟谱、显示和录制入口。

一次四键集合完全替换最多需要八个事件。事件队列空间不足时整批不提交、也不
确认该 state_id；S3 下一次轮询会重传，避免只处理半个和弦。

以下情况会先释放全部本地活动键：切换 sid、收到 hello、停止音频 S3 输入、
链路连续 3 秒超时。超时后链路恢复时 P4 重新发送 start，防止重复音和卡音。

temp 工程原有的 v1 `poly` 和 `diagnostic` 消息继续保留，用于演示页诊断；它们
不会代替 v2 完整琴键集合进入评分。

## 联调检查

1. P4 日志显示 `UART1 TX=GPIO0 RX=GPIO1 at 115200 baud (50 ms)`；
2. S3 日志显示 `UART1 TX=GPIO1 RX=GPIO2 ... 115200 baud`；
3. 普通练习时 S3 显示 `recognition profile: performance`；演示测试页显示 `demo`；
4. P4 显示 `MusicLink v2 note-set confirmed`；
5. 两端周期日志中的 `state` 与 `ack` 很快相等；
6. 同时按三键时三条评分事件具有同一 `sender_ts_ms`；
7. 断开一块麦克风时快照结构不变且 `degraded_mic=true`；
8. 断开串口超过 3 秒，P4 释放全部活动键，恢复后不能重复或卡键。
