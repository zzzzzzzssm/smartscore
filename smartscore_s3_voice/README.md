# SmartScore ESP32-S3 离线语音识别

本工程面向 ESP32-S3-N16R8，使用单个 INMP441、ESP-IDF 标准 I2S RX 接口和乐鑫官方 `espressif/esp-sr` 2.4.6，实现完全本地的中文唤醒与命令识别。

## 接线

| INMP441 | ESP32-S3 |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| SCK / BCLK | GPIO10 |
| WS / LRCL | GPIO11 |
| SD / DOUT | GPIO9 |
| L/R | GND（左声道） |

I2S0 工作在主机、仅 RX、16000 Hz、32 位原始采样、左声道模式。SCK/WS/SD 引脚按 `Netlist_PCB1_2026-07-21.tel` 中 `U4` 的 `INMP441_SCK/INMP441_WS/INMP441_SD` 更新。采集数据会转换为 ESP-SR 所需的 16 kHz、`int16_t`、单声道 PCM。串口约每秒输出一次音频 RMS，用于确认麦克风是否采集到声音。

与 WT99 P4 使用独立 UART1、115200 baud、8N1、无流控通信：S3 GPIO1/TX（H7-11 `VOICE_TX_MS`）连接 P4 GPIO33/RX（J6-22），S3 GPIO2/RX（H7-13 `VOICE_RX_MS`）连接 P4 GPIO32/TX（J6-20），并连接公共地。原 `V1,WAKE`、`V1,TIMEOUT`、`V1,CMD,<id>` 和 P4 返回的 `V1,ACK,<id>,OK|IGNORED` 文本帧保持不变；开放式对话音频复用同一 UART，以独立 IMA-ADPCM 二进制帧传输，并包含序号、长度、头/载荷 CRC16 及 ACK/NACK 重传。UART0 调试日志保持不变。

唤醒后仍优先执行原本地命令识别；本地命令命中时音频候选流立即取消，行为与原工程一致。只有原 6 秒 MultiNet 识别超时才发送 `V2,AI,BEGIN` 进入开放式对话，并持续传送 16 kHz 单声道语音；P4 发送 `V2,AI,STOP` 后恢复本地等待唤醒状态。

## 唤醒词和命令

官方 WakeNet 模型：`wn9_nihaoxiaozhi_tts`

唤醒词：**你好小智**

唤醒成功后串口提示：

```text
唤醒成功，请说出命令
```

随后只识别一条命令。每个动作支持 15 种自然说法，共 105 条识别短语：

| 规范命令 | 支持说法示例 | 串口纯数字输出 |
| --- | --- | ---: |
| 开始练习 | 开始训练、进入练习、我要开始练习 | 1 |
| 停止练习 | 结束练习、暂停练习、不练了 | 2 |
| 下一页 | 翻下一页、往后翻一页、下一张 | 3 |
| 上一页 | 翻上一页、往前翻一页、上一张 | 4 |
| 重新开始 | 从头开始、再来一次、再做一遍 | 5 |
| 查看评分 | 查看分数、显示成绩、我的分数 | 6 |
| 返回首页 | 回到首页、返回主界面、回到主菜单 | 7 |

完整的 105 条中文词表和冲突控制说明见
[`docs/superpowers/specs/2026-07-11-command-aliases-design.md`](docs/superpowers/specs/2026-07-11-command-aliases-design.md)。

命令识别成功时，数字通过 `printf` 单独占一行输出。仅说唤醒词、环境噪声、识别超时和非法结果都不会输出数字。每次成功或超时后会重置 MultiNet 并返回等待下一次唤醒。

ESP-SR 2.4.6 当前 `mn7_cn` 模型的动态命令注册使用空格分隔的中文拼音。工程中 105 条短语的拼音均由组件自带的 `tool/multinet_pinyin.py` 生成，没有手工猜测；其中“重新开始”生成结果为 `chong xin kai shi`。串口日志仍显示七条规范中文命令。

## 构建

准备 ESP-IDF v5.1 或更高的 5.x 环境后执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次配置时，ESP-IDF Component Manager 会根据 `main/idf_component.yml` 下载并锁定官方 ESP-SR 2.4.6 及其依赖。工程使用 16 MB Flash 自定义分区表，其中 `model` 分区用于烧录 WakeNet、VadNet 和 MultiNet 模型。

> Windows 注意：ESP-IDF v5.1 的部分 Python/CMake 脚本不能可靠处理含中文的工程绝对路径。如果构建阶段出现路径乱码或 `Failed to determine sizeof(time_t)`，请从纯 ASCII 路径打开同一工程，或使用已修复该问题的较新 ESP-IDF 5.x 环境。

本仓库只完成了软件构建验证；仍需在实际开发板上确认 INMP441 数据对齐、环境增益和识别效果。
