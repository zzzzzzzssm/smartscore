# WT99 关闭已保存 Wi-Fi 自动连接实施计划

日期：2026-07-16

1. 在 `main/Kconfig.projbuild` 增加 `SMARTSCORE_AUTO_CONNECT_SAVED_WIFI`，默认关闭，并在 `sdkconfig.defaults` 明确保持关闭。
2. 在 `network_provisioning_start()` 中按该配置选择是否读取 NVS 和排队自动连接；关闭时直接进入 `waiting_credentials`。
3. 保留连接成功后的 NVS 保存和 `reset_wifi` 清除逻辑，不修改 BLE、小程序或 C5。
4. 更新网络测试文档，说明测试模式每次启动都等待 BLE；启用开关可恢复正式自动连接。
5. 检查没有密码日志或启动清除 NVS，并使用 ESP-IDF 5.4.4 重新构建 P4 固件。
