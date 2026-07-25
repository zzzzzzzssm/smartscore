# WT99 扬声器模块说明

## 硬件依据

实现只采用以下 WT99 资料，没有复制其他开发板 GPIO：

1. `C:\Users\17762\Desktop\SCH_WT99P4C5-S1-1V1.pdf`
   - 第 2 页：P4 与 C5 的 SDIO、C5 reset；
   - 第 3 页：ES8311、I2S、NS4150B、`PA_CTRL`、J7 差分扬声器；
   - 第 5 页：MicroSD SDMMC 与扬声器接口相关连接。
2. `C:\Users\17762\Desktop\WT99P4C5-S1开发板使用指南.pdf` 第 5～13 页，用于核对板型、接口和 C5 UART/扩展接口说明。
3. 厂商 `wireless-tag-com/WT99P4C5-S1` 示例，本地核对提交 `cfedb303b3c21a5e4d15ab28d320aec9f0f5b707` 的 BSP 引脚和 ESP-Hosted 配置。
4. 已实机验证的独立工程 `C:\Users\17762\Desktop\voice\wt99_speaker_web_test`，尤其是 `board_wt99`、`audio_driver`、`audio_player`、`metronome` 与 `wav_stream`。

## 已确认连接

| 功能 | ESP32-P4 连接 |
|---|---|
| ES8311 I2C | I2C1，SDA GPIO7，SCL GPIO8，7-bit `0x18` |
| ES8311 组件地址 | `esp_codec_dev` legacy 地址 `0x30` |
| I2S MCLK/BCLK/LRCK | GPIO13 / GPIO12 / GPIO10 |
| P4 I2S TX → ES8311 DSDIN | GPIO9 |
| ES8311 ASDOUT → P4 RX | GPIO11，本扬声器模块不启用输入 |
| NS4150B PA enable | GPIO53，高有效 |
| MicroSD D0..D3 | GPIO39 / 40 / 41 / 42 |
| MicroSD CLK/CMD | GPIO43 / GPIO44，SDMMC slot 0，LDO channel 4 |
| P4↔C5 SDIO D0..D3 | GPIO14 / 15 / 16 / 17 |
| P4↔C5 SDIO CLK/CMD/reset | GPIO18 / GPIO19 / GPIO54，slot 1、4-bit、40 MHz |

ES8311 的单路输出进入 NS4150B，因此用户界面按“单声道”处理。外接扬声器应为 4Ω、3W、PH2.0-2P 无源扬声器；两根线都接板载 J7 扬声器接口。J7 是差分输出，任意一根都不得单独接 GND。

初次测试从设备音量 15% 开始，设备软件限制最大 80%。小程序显示 0～100，并按比例映射到设备 0～80。推荐稳定 5V 电源；若出现重启、屏幕闪烁或 USB 断开，先检查供电和扬声器接线。

## 模块边界

- `board_wt99/board_audio`：I2C、ES8311、I2S TX、PA 和安全静音；
- `board_wt99/board_sdcard`：只使用确认的 SDMMC slot 0 引脚，失败不格式化；
- `speaker_service`：唯一播放任务、命令队列、主音源替换、正弦音、节拍器和 WAV；
- `device_api`：局域网 JSON 接口，不托管测试网页；
- `miniprogram/pages/audio`：面向使用的声音工具页面，不显示扫频、日志或压力测试。

麦克风输入和声学回环没有迁移，也没有启用 GPIO11 RX，避免产生啸叫或误把原理图存在当作已验证输入通路。
