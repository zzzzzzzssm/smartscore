# WT99 板端 Wi-Fi 扫描实施计划

日期：2026-07-16

1. 修改 `wifi_remote_events.c`，取消 `WIFI_EVENT_STA_START` 中的无条件连接；修改 `wifi_remote_start_sta()` 显式连接，并让 `wifi_remote_scan()` 能安全启动仅用于扫描的 STA。
2. 在 `network_provisioning` 中定义扫描事件、观察器和 `NETWORK_COMMAND_SCAN`，由现有 provisioning task 阻塞执行扫描、去重、RSSI 排序并最多发布 15 个热点。
3. 在 `ble_provisioning` 中解析 `scan_wifi`，只把请求放入队列；将扫描开始、单条热点、完成和错误事件编码为换行 JSON 并通过 TX 分片通知。
4. 修改设备页，删除微信手机 Wi-Fi API，改为通过 BLE 发送 `scan_wifi` 并合并板端返回的热点。
5. 更新 BLE 协议文档，检查不存在手机 Wi-Fi 扫描调用或密码日志。
6. 使用项目当前 ESP-IDF 5.4.4 构建 ESP32-P4 固件，修复真实编译错误；不执行烧录。
