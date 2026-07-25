# ESP32-C5 Hosted 固件要求

## 已确认资料

启明云端官方 [WT99P4C5-S1 示例仓库](https://github.com/wireless-tag-com/WT99P4C5-S1) 提供 C5 slave 固件及启动日志。该日志显示：

- ESP-Hosted MCU Slave 2.0.13；
- transport 为 SDIO；
- WLAN 可用；
- Bluetooth 为 BLE only；
- HCI 通过 SDIO；
- capabilities 为 `0xD`。

这说明厂商提供了与本目标架构相符的 Wi-Fi + Bluetooth Hosted slave 固件。它不等于已经确认用户手上这块板当前烧录的 C5 固件版本。

## 运行时判定

P4 端先初始化 ESP-Hosted 和 Wi-Fi Remote，再启动 NimBLE Host，并等待远端 Controller 同步。只有 NimBLE 实际同步成功后，程序才设置 BLE capability 并开始广播。

预期成功日志包含：

```text
[HOSTED] initialization complete
[HOSTED] Wi-Fi capability available
[HOSTED] BLE capability available
[BLE] advertising started
```

若 10 秒内无法与远端 Controller 同步，返回 `ESP_ERR_NOT_SUPPORTED`，并打印：

```text
C5 hosted firmware does not expose Bluetooth capability
```

程序不会伪报 BLE 可用。若出现该错误，需要核对 C5 的 ESP-Hosted slave 固件是否支持 WLAN + BLE/HCI，以及其 transport、协议版本是否兼容。

## 烧录边界

本阶段不自动烧录、擦除或修改 C5。即使官方仓库包含 C5 固件，也必须先保存首次上电完整串口日志，再根据实际版本另行决定是否需要更新；任何 C5 烧录都需要单独确认。
