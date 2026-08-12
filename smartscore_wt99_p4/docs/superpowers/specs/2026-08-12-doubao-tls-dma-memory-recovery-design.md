# 豆包 TLS/DMA 内存恢复设计

## 问题与边界

实机日志显示 S3 音频序号连续、证书校验成功，但 TLS 握手在 P4
硬件 AES 申请 DMA 缓冲时失败。修复只调整新增实时语音助手和内存放置，
不修改识谱、评分、练习建议、UI、网络协议、板间引脚或本地命令逻辑。

## 选择的方案

保留硬件 AES 和证书验证。mbedTLS 从“全部内部内存”改为默认平衡分配，
配合较低的外部内存阈值，让大型 TLS 缓冲进入 PSRAM、小型上下文留在
内部 RAM。内部/DMA 保留池增至 96 KiB，ESP-Hosted 默认任务栈和
voice_assistant worker 栈迁入 PSRAM，为 SDIO 发送副本和 AES DMA
留下连续内部空间。

不采用关闭硬件 AES 的方案，因为它增加 CPU 占用和语音时延；也不只扩大
输入音频环形缓冲，因为该缓冲原本就在 PSRAM，无法解决 DMA 枯竭。

## 运行时保护

开始 WebSocket 前记录内部、DMA 和 PSRAM 的空闲量及最大 DMA 连续块。
若 DMA 空闲量低于 48 KiB或最大连续块低于12 KiB，拒绝本轮 AI 会话并
通知 S3 恢复等待唤醒。连接错误最多自动重试三次，随后停止会话，避免
持续输入导致环形缓冲覆盖。

P4语音UART初始化后主动发送 `V2,AI,STOP,P4_READY`，用于终止P4复位前
遗留在S3中的流会话。该帧不改变原 `V1` 命令协议。

## 验证

静态检查确认Kconfig选项互斥关系、PSRAM栈创建失败的清理路径、WebSocket
错误计数和S3停止路径。实机应确认TLS证书校验后不再出现
`esp-aes: Failed to allocate memory`，并检查本地命令、评分、UI和Hosted
网络行为保持不变。
