# BLE 配网协议

## 架构

ESP32-P4 运行 NimBLE Host，通过 ESP-Hosted VHCI 使用板载 ESP32-C5 的 Bluetooth Controller。P4 不启用本地蓝牙控制器。

广播名为 `SmartScore-WT99-XXXX`，其中 `XXXX` 取设备出厂 MAC 地址最后两个字节的大写十六进制表示。

## GATT

| 用途 | UUID | 属性 |
|---|---|---|
| Service | `7A6E0001-5C5A-4B11-9A4A-53534D415254` | Primary Service |
| RX（手机到设备） | `7A6E0002-5C5A-4B11-9A4A-53534D415254` | WRITE、WRITE WITHOUT RESPONSE |
| TX（设备到手机） | `7A6E0003-5C5A-4B11-9A4A-53534D415254` | READ、NOTIFY |

UUID 唯一定义位于 `components/ble_provisioning/include/ble_provisioning_protocol.h`。

## 帧格式

- 一条消息是 UTF-8 JSON，末尾必须有换行符 `\n`。
- 双向均按最多 20 字节分片；分片边界与 UTF-8 字符边界无关。
- 接收方先累积原始字节，遇到 `\n` 后才进行 UTF-8 解码和 JSON 解析。
- 单条消息最大 512 字节，不包含换行符。
- 超限后设备清空缓存、丢弃至下一换行符，并通知 `MESSAGE_TOO_LARGE`，不会越界或重启。

## 手机命令

```json
{"id":1,"cmd":"get_status"}
```

```json
{"id":2,"cmd":"set_wifi","ssid":"用户输入的SSID","password":"用户输入的密码"}
```

```json
{"id":3,"cmd":"reset_wifi"}
```

```json
{"id":4,"cmd":"get_device_info"}
```

请求 WT99 板载 C5 扫描附近 Wi-Fi（不是扫描手机 Wi-Fi）：

```json
{"id":5,"cmd":"scan_wifi"}
```

## 设备事件

```json
{"id":2,"event":"wifi_state","state":"connecting"}
```

```json
{"id":2,"event":"wifi_state","state":"connected","ip":"192.168.1.100"}
```

```json
{"id":2,"event":"wifi_state","state":"failed","reason":"AUTH_FAIL"}
```

```json
{"id":3,"event":"wifi_state","state":"waiting_credentials"}
```

```json
{"id":4,"event":"device_info","device":"SmartScore-WT99","firmware":"smartscore-network-stage1","ble":true,"wifi":true}
```

板端扫描按开始、单条热点和完成顺序通知，最多返回信号最强的 15 个非隐藏 SSID：

```json
{"id":5,"event":"wifi_scan","status":"wifi_scan_start"}
```

```json
{"id":5,"event":"wifi_network","status":"wifi_network","ssid":"Example","rssi":-48,"channel":6,"open":false}
```

```json
{"id":5,"event":"wifi_scan","status":"wifi_scan_done","count":12}
```

```json
{"id":5,"event":"wifi_scan","status":"failed","reason":"wifi_scan_failed"}
```

通用错误格式：

```json
{"id":0,"event":"error","error":"MESSAGE_TOO_LARGE"}
```

可能的失败原因包括 `AUTH_FAIL`、`NO_AP_FOUND`、`ASSOC_FAIL`、`TIMEOUT`、`CONNECT_FAILED`、`START_FAILED`、`RETRY_FAILED`、`NVS_SAVE_FAILED`、`wifi_scan_busy`、`wifi_scan_queue_full`、`wifi_scan_failed` 和 `wifi_not_ready`。

## 安全与并发

BLE 写回调只做分片累积、JSON 校验和 FreeRTOS Queue 投递，不执行 Wi-Fi 连接或扫描。专用 provisioning task 负责扫描、连接、有限重试、取得 IP、写 NVS 和状态广播。扫描、连接和清除命令串行执行。日志允许记录 SSID，但绝不记录密码或完整敏感 JSON；临时密码缓冲区在使用后清零。
