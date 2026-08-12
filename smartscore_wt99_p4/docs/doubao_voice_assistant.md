# 豆包端到端实时语音全双工助手

本功能仅新增实时语音助手，不改变现有 AI 识谱、评分、AI 练习建议、
本地唤醒词和七条本地指令的模型、接口或业务逻辑。

## 鉴权配置

API Key 不进入源码、`sdkconfig` 或 Git。编译 P4 前在同一个 PowerShell
会话中设置：

```powershell
$env:DOUBAO_VOICE_API_KEY='在豆包语音新版控制台获取的 API Key'
idf.py build
```

不要把真实 Key 写进本文件、`sdkconfig.defaults` 或任何提交文件。音色、
系统提示词、缓冲时长和重连间隔可通过 `idf.py menuconfig` 的
`Doubao full-duplex voice assistant` 菜单调整；协议模型保持官方固定值
`1.2.6.1`。

## 兼容与分流规则

- 原语音 UART 仍为 UART2/UART1、GPIO32/33 与 GPIO1/2、115200、8N1。
- 原 `V1,WAKE`、`V1,TIMEOUT`、`V1,CMD`、`V1,ACK` 文本帧保持不变。
- 唤醒后，S3 继续用原 MultiNet 优先识别本地指令，同时把 AFE 的
  16 kHz 单声道 PCM 旁路压缩为独立 IMA-ADPCM 块。
- 新二进制帧含版本、类型、序号、长度、头 CRC16 和载荷 CRC16；P4
  对每帧 ACK，发现损坏或序号缺口时 NACK，S3 从短期缓存重传。
- P4 在本地识别窗口内只缓存音频。本地指令命中即丢弃缓存并走原逻辑；
  MultiNet 超时后才建立豆包 WebSocket 并刷新缓存。

## 官方协议参数

- WebSocket：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`
- 请求头：`X-Api-Key`
- 会话模型：`1.2.6.1`
- 上行：`input_audio_buffer.append`，Base64 PCM，16 kHz、s16le、单声道
- 下行：`response.output_audio.delta`，Base64 PCM，24 kHz、s16le、单声道
- 用户插话：收到 ASR `started` 后停止当前扬声器流并发送
  `response.cancel`

`voice_assistant_set_context_provider()` 是预留的只读上下文接口。以后可在
不修改评分逻辑的前提下传入当前曲目和最近评分摘要。
