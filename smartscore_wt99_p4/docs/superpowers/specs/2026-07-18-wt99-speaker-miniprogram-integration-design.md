# WT99 扬声器模块与小程序音频中心设计

日期：2026-07-18

## 1. 目标

把独立测试工程 `wt99_speaker_web_test` 中已经验证的 WT99P4C5-S1 扬声器输出能力迁入 SmartScore P4 主工程，并作为可复用的扬声器模块使用。

面向用户的小程序仅提供：

- 音量与静音控制；
- 节拍器；
- 校准音；
- SD 卡 WAV 播放。

扫频、实时日志和压力测试仅保留为受编译配置控制的工程诊断能力，默认不出现在小程序中。

## 2. 已确认边界

- P4 目标仍使用 ESP-IDF 5.4.x；C5 固件保持 ESP-Hosted 从机 v2.12.6。
- 不迁移测试工程的 SoftAP、`network_manager`、静态网页或网页服务器实现。
- 保持主工程现有通信方式：P4 NimBLE Host 通过 ESP-Hosted/SDIO 使用 C5 的蓝牙完成发现和配网；联网后小程序通过设备局域网 IP 的 HTTP API 控制音频。
- BLE 第一版不承载音频播放命令，Wi-Fi 不可用时不提供 BLE 音频控制。
- 不实现麦克风回环、TTS、MP3/OGG 解码、摄像头、S3 节点或评分业务。
- 不修改 C5 固件，不烧录，不擦除 Flash，不创建 Git 提交。

## 3. 硬件事实

引脚来自 WT99P4C5-S1 1V1 原理图和已验证测试工程，不使用其他开发板引脚：

| 功能 | P4 引脚/参数 |
| --- | --- |
| ES8311 I2C | I2C1，SDA GPIO7，SCL GPIO8，7-bit 地址 `0x18` |
| I2S | I2S1，MCLK GPIO13，BCLK GPIO12，LRCK GPIO10，DOUT GPIO9，DIN GPIO11 |
| NS4150B 功放使能 | GPIO53，高电平有效 |
| SDMMC | D0~D3 GPIO39~42，CLK GPIO43，CMD GPIO44，4-bit |
| C5 Hosted SDIO | D0~D3 GPIO14~17，CLK GPIO18，CMD GPIO19，RESET GPIO54 |

板载 NS4150B 驱动 PH2.0-2P 差分扬声器输出。扬声器两根线均连接板载扬声器接口，任意一端都不能单独接 GND。

## 4. 组件设计

### 4.1 `board_wt99`

`board_wt99` 只拥有硬件事实和底层板级适配：

- 扩展 `board_wt99_pins.h`，集中定义音频和 SDMMC 引脚；
- `board_audio` 管理 P4 I2C 总线、ES8311、I2S TX 和 NS4150B 使能；
- `board_sdcard` 管理 SDMMC 挂载和卸载；
- 保持现有 C5 Hosted 校验与初始化代码不变。

### 4.2 `speaker_service`

新增统一扬声器服务，包含：

- 独立 FreeRTOS 音频任务；
- 所有控制命令进入同一个队列；
- 任意时刻只有一个主播放源；
- 新播放请求安全停止并替换旧播放；
- 正弦校准音实时生成；
- 节拍器使用独立定时任务驱动，音频输出会话在拍点之间保持开启；
- WAV 采用分块读取和双缓冲；
- 音量、静音、播放状态、最近错误和写入错误计数统一保存；
- 初始设备音量 15%，设备软件最大音量 80%；小程序显示 0～100 并按比例映射。

### 4.3 `audio_prompt`

`audio_prompt` 作为以后正式业务提示音的上层封装，依赖 `speaker_service`，不直接操作 I2S、Codec 或功放。

### 4.4 `device_api`

`device_api` 作为联网后的唯一 HTTP Server 所有者，统一注册网络状态和音频接口。现有 `network_diagnostics` 不再单独创建第二个 HTTP Server。

## 5. 小程序通信与页面

现有设备页继续完成 BLE 发现、C5 Wi-Fi 扫描、凭据下发和设备 IP 保存。新增非 TabBar 页面 `pages/audio/audio`，从设备页和练习页进入。

音频中心按以下顺序展示：

1. 设备、Codec、功放、SD 卡和当前播放状态；
2. 全局音量、静音和停止全部播放；
3. 节拍器：30~240 BPM、2/4、3/4、4/4、6/8、开始/暂停/停止和节拍动画；
4. 校准音：C4、E4、G4、A4、C5，支持持续、10 秒和 30 秒；
5. SD WAV 文件列表、搜索、播放、暂停和停止；
6. 页面底部固定当前播放控制条。

扫频、日志和压力测试不创建入口。工程诊断路由只有在 `CONFIG_SMARTSCORE_AUDIO_DIAGNOSTICS` 打开时才注册。

## 6. HTTP API

公开接口：

```text
GET  /api/status
GET  /api/audio/status
POST /api/audio/volume
POST /api/audio/mute
POST /api/audio/tone
POST /api/audio/stop
POST /api/metronome/start
POST /api/metronome/pause
POST /api/metronome/stop
GET  /api/audio/files
POST /api/audio/file/play
POST /api/audio/file/pause
POST /api/audio/file/stop
```

接口异步返回“命令已接收”，小程序通过 `/api/audio/status` 获取最终状态。错误统一返回机器可读错误码和中文说明，不因可恢复错误重启设备。

## 7. SD 与音频格式

- SD 挂载点：`/sdcard`；
- 用户音频目录：`/sdcard/wav`；
- 只允许安全的单层相对文件名，拒绝绝对路径、`..` 和反斜杠；
- 第一版支持 RIFF/WAVE、单声道、16-bit PCM、16 kHz 或 24 kHz；
- FATFS 启用堆分配长文件名、最大 255 字符、CP936 和 UTF-8 API；
- 文件列表和播放接口使用 UTF-8 文件名；
- 不把完整 WAV 一次性载入 RAM。

## 8. 生命周期和异常处理

- 音频硬件在启动时初始化，但功放保持关闭；
- 开始播放时取消 Codec 硬件静音并打开功放；
- 停止、播放完成、错误或换源时写入短静音、静音 Codec 并关闭功放；
- 重复停止返回成功；重复开始按新请求替换；
- 小程序离开页面不会自动停止播放，但会停止状态轮询；
- Wi-Fi 断开不强制打断当前本地音频，HTTP 服务随联网状态停止并在重连后恢复；
- 音频任务不阻塞 BLE、Wi-Fi、HTTP 或主循环。

## 9. 分区与发布风险

阶段一镜像已接近原 1 MiB 应用分区上限。为降低当前集成风险，P4 先采用单个 8 MiB factory 应用分区，并保持应用起始地址 `0x10000` 不变；外置 SD 卡保存音频，内部剩余空间保留给配置和诊断数据。本阶段不实现 OTA，正式增加 OTA 前再通过独立迁移设计切换双应用分区。

小程序开发和局域网真机测试继续使用设备 IP。正式发布前必须在目标 Android/iOS 微信版本上验证局域网 HTTP、权限提示、同网段判断和后台切换行为。
