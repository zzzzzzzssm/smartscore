# WT99 ESP32-C5 Hosted 固件构建与烧录

本目录是板载 ESP32-C5 的 ESP-Hosted slave 固件，不是 ESP32-P4 主程序。

## 已确认配置

- ESP-IDF：5.5.3
- 目标：ESP32-C5
- ESP-Hosted slave：2.12.6
- P4-C5 传输：SDIO
- C5 功能：Wi-Fi + BLE Controller
- BLE HCI：经 SDIO 送到 P4 端 Hosted VHCI/NimBLE Host
- 串口：外接 USB-UART 的 COM8

## VS Code 中打开 C5 专用终端

1. 选择“终端 -> 新建终端”旁边的下拉箭头。
2. 选择 `ESP-IDF 5.5.3 C5 PowerShell`。
3. 终端会自动进入本目录。
4. 输入 `idf.py --version`，应显示 `ESP-IDF v5.5.3`。

## 让 C5 进入下载模式

板上的 BOOT 按钮属于 P4，不能用它控制 C5。使用 C5 调试排针：

1. USB-UART 的 GND 与板上 GND 已共地；不要连接 USB-UART 的 VCC。
2. 将 C5 BOOT（J5-6）暂时接到 GND（J5-4）。
3. 将 C5 EN（J5-1）短暂接到 GND（J5-4），随后先释放 EN。
4. 再释放 BOOT。此时 C5 应停在 ROM 下载模式。

## 烧录

在 C5 专用终端中执行：

```powershell
idf.py -p COM8 -b 115200 flash
```

烧录成功时最后会出现 `Hash of data verified` 和 `Hard resetting via RTS pin...` 等提示。如果自动复位不起作用，手动将 C5 EN（J5-1）短接 GND 后释放。

不要执行 `erase-flash`，也不要把本固件烧到 P4 的 COM12。

## 查看 C5 日志

确保 BOOT 已释放，然后手动复位一次 C5，再执行：

```powershell
idf.py -p COM8 monitor
```

按 `Ctrl+]` 退出监视器。预期能看到：

```text
ESP-Hosted-MCU Slave FW version :: 2.12.6
Transport used :: SDIO only
Supported features are:
- WLAN over SDIO
- BT/BLE
   - HCI Over SDIO
   - BLE only
capabilities: 0xd
```

## 实际烧录地址

`idf.py flash` 会从 `build/flash_args` 自动读取以下地址：

| 地址 | 文件 |
|---|---|
| `0x2000` | `build/bootloader/bootloader.bin` |
| `0xC000` | `build/partition_table/partition-table.bin` |
| `0x16000` | `build/ota_data_initial.bin` |
| `0x20000` | `build/network_adapter.bin` |

## 已构建镜像 SHA-256

```text
97858D019629056CC546F6E65854AC52A9A98AF6423BB8899BD6B6BEFBD7733A  bootloader.bin
6139E8FA7F2A8D22F53F7C503ECF18D0FF453B4016064B75F38BEB71D3F8C752  partition-table.bin
7D2C7AC4888BFD75CD5F56E8D61F69595121183AFC81556C876732FD3782C62F  ota_data_initial.bin
9ADF5DE8FA3FD905CE92376D9C1F72E032B7B31F5A64F0F7A1F95A4812B602BA  network_adapter.bin
```
