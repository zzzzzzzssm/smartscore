# SmartScore WT99 一阶段网络固件

这是 WT99P4C5-S1（ESP32-P4 + 板载 ESP32-C5）的正式一阶段网络工程。当前只实现 ESP-Hosted、Wi-Fi Remote、NimBLE Hosted VHCI、BLE 配网、备用网页配网和最小网络诊断接口；不初始化屏幕、LVGL、音频、MIDI、乐谱、AI、摄像头、SD 卡或 S3 通信。

## 当前状态

- 使用 ESP-IDF 5.4.4 构建成功；ESP-IDF 5.1.2 不支持 `esp32p4`，不能用于本板。
- ESP-Hosted 固定为 2.12.6，`esp_wifi_remote` 固定为 1.6.0。
- 已根据 WT99P4C5-S1 1V1 原理图和厂商示例实现 P4-C5 板级配置：SDIO slot 1、4-bit、40 MHz，GPIO14～19，C5 EN/复位 GPIO54。
- BLE 架构为 P4 NimBLE Host → ESP-Hosted VHCI → C5 Bluetooth Controller。
- 厂商提供的 C5 固件资料包含 Wi-Fi + BLE/HCI 能力；板上当前实际固件仍须通过首次启动日志和 NimBLE 同步验证。
- 固件产物为 `build/smartscore_wt99_p4.bin`。本任务没有自动烧录 P4 或 C5。

板级依据见 [docs/wt99_pinmap.md](docs/wt99_pinmap.md)，编译、烧录和联调步骤见 [docs/network_test_steps.md](docs/network_test_steps.md)，完整迁移结论见 [docs/network_migration_report.md](docs/network_migration_report.md)。

微信小程序已统一放在 `miniprogram/`：保留原“谱伴”全部 UI，只将智能设备页适配为 WT99 通信协议。在微信开发者工具中导入 `D:\qianrushi\smartscore_wt99_p4\miniprogram`。
