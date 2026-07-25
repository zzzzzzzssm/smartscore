# WT99 Hosted Wi-Fi 扫描与 DHCP 恢复设计

## 实机证据

- 扫描期间日志出现 `Timeout waiting for Resp for Req_GetMACAddress`，随后默认 STA netif 报 `esp_wifi_get_mac failed`。
- 扫描完成事件到达，但结果始终为 0。
- 手动提交正确配置后 C5 报 `Station mode: Connected`，P4 始终没有收到 `IP_EVENT_STA_GOT_IP`。
- BLE 在扫描期间保持连接，之前的 provisioning task 栈溢出已经消失。

## 根因

当前代码使用 `esp_wifi_scan_start(..., true)`。ESP-Hosted slave 同一时刻只能处理一个同步 RPC；阻塞扫描占用 slave 命令处理约 19 秒，同时默认 netif 在 `WIFI_EVENT_STA_START` 中发起的 MAC 查询超时，导致 STA netif/DHCP 初始化不完整。

此外，当前工程在 `esp_wifi_init()` 之后才创建默认 STA/AP netif，与 ESP-Hosted 官方 host 示例的顺序相反。小程序在新一轮连接开始或失败后仍保留旧 IP，又造成“地址不可达”的误导。

## 方案

采用事件驱动的非阻塞扫描：

1. 调用 `esp_wifi_scan_start(..., false)` 后立即释放 Hosted RPC。
2. 在已有 Wi-Fi 事件处理器中接收 `WIFI_EVENT_SCAN_DONE`，通过 EventGroup 唤醒 provisioning task。
3. 最长等待 30 秒；收到事件后再读取扫描结果。超时或 C5 返回失败时，向 BLE 返回扫描失败，不重启设备。
4. 在 `esp_wifi_init()` 前创建默认 STA/AP netif；失败清理和反初始化统一使用 `esp_netif_destroy_default_wifi()`。
5. Wi-Fi 初始化后尝试选择 `WIFI_BAND_MODE_AUTO`，让 ESP32-C5 同时扫描 2.4 GHz 和 5 GHz；若当前 Hosted 固件不支持，只记录警告，不阻止 2.4 GHz 工作。
6. 小程序收到 `connecting` 或 `failed` 时清除旧 HTTP 地址，只在 BLE 收到 `connected` 和实际 IP 后保存新地址。

## 未采用方案

- 仅增加 RPC 超时时间：扫描仍会独占 slave，无法修复并发 MAC 查询和 DHCP 初始化。
- 保留阻塞扫描并暂停 BLE：不符合 BLE 配网链路要求，也不能保证默认 netif 正常建立。
- 取消板端扫描、只允许手输：不符合当前交互目标。

## 验证

- 构建必须使用 ESP-IDF 5.4.x，目标为 `esp32p4`。
- 检查扫描日志中不再出现 `Req_GetMACAddress` 超时。
- 扫描结束返回可见 AP；手动配网后必须出现 `got IP`，BLE 通知带实际 IP。
- 本轮只生成 P4 固件，不自动烧录。
