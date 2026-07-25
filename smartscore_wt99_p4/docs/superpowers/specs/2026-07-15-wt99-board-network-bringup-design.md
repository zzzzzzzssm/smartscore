# WT99P4C5-S1 正式网络第一步板级实现设计

日期：2026-07-15

## 目标

把当前因缺少资料而安全阻塞的网络工程，改成基于 WT99P4C5-S1 1V1 原理图和启明云端官方示例的可上板版本。本步只验证 BLE 配网、ESP32-C5 Wi-Fi、IP 通知和 `/api/ping`，不启动屏幕、音频、MIDI、乐谱、AI、摄像头、SD 卡或 S3 通信。

## 板级依据

依据文件与官方实现：

- `C:\Users\17762\Desktop\SCH_WT99P4C5-S1-1V1.pdf` 第 2 页；
- `C:\Users\17762\Desktop\WT99P4C5-S1开发板使用指南.pdf`；
- 启明云端官方仓库 `wireless-tag-com/WT99P4C5-S1` 的 `sdkconfig.defaults` 和启动日志。

确认配置：

| 信号 | ESP32-P4 GPIO |
|---|---:|
| SDIO D0 | 14 |
| SDIO D1 | 15 |
| SDIO D2 | 16 |
| SDIO D3 | 17 |
| SDIO CLK | 18 |
| SDIO CMD | 19 |
| C5 EN/reset | 54 |

传输为 SDIO slot 1、4-bit、40 MHz。使用指南 J14 表格中 GPIO39-44 对应的是板载 MicroSD 连接，与原理图和官方 Hosted 配置冲突，因此不用于 P4-C5。

## 板级职责

`board_wt99` 集中保存上述常量并校验生成配置：

- C5 由板载 U14 3.3 V LDO持续供电，`board_wt99_network_power_enable()` 记录固定供电并返回成功，不伪造可控电源开关。
- C5 reset/enable 为 GPIO54，实际复位脉冲由 ESP-Hosted transport 初始化执行；`board_wt99_c5_reset()` 校验 reset GPIO 配置并把复位所有权交给 Hosted，避免重复脉冲。
- `board_wt99_c5_transport_prepare()` 校验 SDIO pin、slot、bus width、frequency 和 C5 target；任何一项不一致都返回错误，阻止 Hosted 接触错误引脚。

## 正式命名

- CMake 工程名改为 `smartscore_wt99_p4`。
- `network_test_api` 组件改为 `network_diagnostics`。
- `CONFIG_SMARTSCORE_NETWORK_TEST_ONLY` 改为 `CONFIG_SMARTSCORE_NETWORK_ONLY`。
- BLE device info 固件标识改为 `smartscore-network-stage1`。
- `/api/ping` 与网络 `/api/status` 继续作为正式诊断端点，不接入业务状态。

## C5 固件

官方仓库提供的 C5 slave 日志报告 ESP-Hosted-MCU 2.0.13、SDIO、WLAN、BLE only、HCI over SDIO、capabilities `0xD`。本步不自动烧录或修改 C5；运行时仍以远端初始化信息和 NimBLE 同步结果判断实际板载固件能力。

## 验证

1. 通过组件管理器重新配置 ESP32-C5 target 和 Hosted SDIO 参数。
2. 使用 ESP-IDF 5.4.4 执行 `set-target esp32p4`、`reconfigure` 和 `build`。
3. 静态检查正式主程序不依赖屏幕、音频、乐谱和 M5Stack BSP。
4. 不自动执行 P4 flash、C5 flash、erase-flash 或 Git commit。
5. 构建完成后交付手工烧录、串口、BLE 和 HTTP 验证步骤。

## 成功标准

- `board_wt99` 不再返回资料缺失错误，并能拒绝错误的 Hosted 编译配置。
- P4 固件完整编译链接成功。
- 上板后 Hosted 日志显示 slot 1、4-bit、40 MHz 和 GPIO 14-19/54。
- C5 报告 Wi-Fi 和 BLE 后，NimBLE 开始 `SmartScore-WT99-XXXX` 广播。
- 小程序可发送 Wi-Fi 凭据，设备 GOT_IP 后通知 IP，并开放 `/api/ping`。
