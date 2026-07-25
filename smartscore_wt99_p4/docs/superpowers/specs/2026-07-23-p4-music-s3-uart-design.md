# P4 音频 S3 双向 UART 设计

## 目标

让 WT99 ESP32-P4 通过独立 UART 双向连接音频 S3，接收已有的 NDJSON 音乐事件，并把
`note_on`、`note_off` 转换成当前 USB MIDI 管线使用的同一种音符事件结构，经
`audio_s3` 输入源复用现有跟谱显示、演奏记录和评分算法。音频 S3 的链路层改为
主轮询响应；评分算法、乐谱解析和显示算法均不修改。

## 已确认约束

- 音频 S3 使用 UART1、TX GPIO1、RX GPIO2、460800 baud、8N1、无流控。
- 用户最终提供的 MUSIC 接线为 P4 J6-4 GPIO0/TX 接 S3 H7-8
  `MUSIC_RX_MS`（S3 GPIO1/RX），S3 H7-10 `MUSIC_TX_MS`（S3 GPIO2/TX）
  接 P4 J6-6 GPIO1/RX，因此软件将 P4 配置为 TX GPIO0、RX GPIO1，并共地。
- 音频 S3 当前消息字段是 `v/type/seq/sid/ts_ms`，`hello` 使用
  `source:"s3_audio"`；不能强制要求不存在的 `src:"music"`。
- 音频 S3 默认开启事件流，生成 `hello`、`status`、`heartbeat`、`pitch`、
  `note_on`、`note_off` 和 `poly`，但上行帧先缓存，等 P4 轮询后发送。
- P4 当前 USB MIDI 事件结构可被评分服务和屏幕适配器复用，但音频 S3 必须保留
  独立的输入源身份，不能伪装成已连接的 USB 设备。

## 方案选择

采用现有 `components/s3_bus` 组件，不把串口或 JSON 解析塞入 `main.c`，也不调用
USB MIDI 组件的私有队列。`s3_bus` 只负责物理链路、协议校验、去重和音乐事件
派发；`main` 负责把公开的 S3 音乐事件映射为公开的 `usb_midi_event_t`，分别
调用音频 S3 评分入口和现有屏幕跟谱入口。

这样保留三个边界：

1. `s3_bus` 不依赖评分、显示或 USB 主机实现。
2. USB MIDI 驱动仍只处理真实 USB 设备。
3. `audio_s3` 保留独立输入状态，但跟谱和评分继续只维护一套算法。

## 组件与数据流

`s3_bus_init()` 配置 P4 UART1，创建轮询、RX、事件队列和派发任务：

```text
S3 GPIO2/TX
  -> P4 GPIO1/RX
  -> UART 驱动环形缓冲
  -> 按换行组包
  -> cJSON 严格解析与字段范围校验
  -> sid/seq 去重
  -> S3 音乐事件队列
  -> main 回调
  -> usb_midi_event_t
  -> audio_s3 输入源 + 现有跟谱显示 + 演奏记录 + 评分
```

P4 GPIO0/TX 每 50 ms 发送 `poll`，并使用 `ping/start/stop` 控制协议。S3
GPIO2 平时是高阻输入；收到 `poll` 后才启用 UART TX、最多回复 4 帧，发送完成后
立即恢复高阻。P4 空闲时要求 S3 停止事件流但仍可由轮询取得 heartbeat；音频 S3
输入会话开始/结束时分别发送 `start`/`stop`。初始化和收到新 `hello` 时发送
`ping`，通过后续轮询取得的 `pong` 验证双向链路，并重新同步目标流状态，以覆盖
两块板上电顺序不同和 S3 运行中重启的情况。

## 协议与校验

每行最大 384 字节。接收器必须：

- 等到 `\n` 才解析完整 JSON，忽略 `\r`；
- 超长后丢弃整行，直到下一个换行恢复；
- 要求 `v == 1`，`type` 为已知类型，`seq/sid/ts_ms` 为 uint32 整数；
- `note_on` 要求 MIDI 0–127、velocity 1–127；
- `note_off` 要求 MIDI 0–127；
- 拒绝尾随非空白字符、错误 JSON、缺字段、NaN、负数和越界值；
- 同一会话只接受递增序号；收到 `hello` 或 `sid` 变化后建立新序号基线；
- 非音符消息只更新链路状态，不进入评分或显示。

接收时间使用 P4 本地 `esp_timer_get_time()`，避免两块板上电时刻不同导致绝对
时间戳错位。S3 的 `ts_ms` 保留在事件结构和状态中用于诊断。

## 错误处理与状态

UART RX 与业务派发使用不同任务。解析任务不会等待评分或显示锁；事件队列满时
丢弃该事件并增加计数。状态接口报告初始化、在线、ready、stream 状态、最后接收
时间、最后 sid/seq、有效帧、非法帧、超长行、重复帧和队列丢弃数。在线判定为
最近 3 秒内收到合法消息。

状态中额外记录目标流状态、是否收到 `pong`、最后 `pong` 时间及控制命令发送/
失败计数。初始化失败只禁用音乐 S3 输入并记录错误，不阻止屏幕、USB MIDI、网络或其他
P4 功能启动。

