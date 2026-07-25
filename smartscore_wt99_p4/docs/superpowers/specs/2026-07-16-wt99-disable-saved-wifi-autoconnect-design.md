# WT99 测试模式关闭已保存 Wi-Fi 自动连接设计

日期：2026-07-16

## 1. 目标

为 WT99P4C5-S1 当前网络联调版本关闭开机自动连接，使设备每次启动后都等待微信小程序通过 BLE 完成“连接设备、扫描 Wi-Fi、选择 SSID、发送密码、获得 IP”的完整流程。

已保存的 SSID、密码和有效标记继续保留在 NVS 中，不因启动测试模式而删除。当前可用的自动连接版本已保存为 Git 提交 `965dbed`。

## 2. 配置方式

在 `main/Kconfig.projbuild` 中新增布尔配置：

```text
CONFIG_SMARTSCORE_AUTO_CONNECT_SAVED_WIFI
```

该配置默认关闭，并在 `sdkconfig.defaults` 中明确保持未启用。当前测试固件因此不会自动连接；将来正式联调需要恢复自动连接时，只需启用该选项，不需要重新修改连接代码。

不手动提交生成的 `sdkconfig`。

## 3. 启动行为

`network_provisioning_start()` 创建 provisioning task 后按配置选择行为：

- 配置关闭：不调用 `network_credentials_load()`，不把密码读入 RAM，不创建 `NETWORK_COMMAND_CONNECT`，直接进入 `NETWORK_STATE_WAITING_CREDENTIALS`。
- 配置开启：保持现有行为，读取有效 NVS 凭据并将自动连接命令放入队列；没有有效凭据时进入等待配网状态。

配置关闭时打印：

```text
saved Wi-Fi auto-connect disabled; waiting for BLE credentials
```

不得打印 NVS 密码。

## 4. NVS 与运行期行为

- BLE `set_wifi` 成功获取 IP 后仍保存 `wifi_ssid`、`wifi_pass` 和 `wifi_valid`。
- 当前运行周期在配网成功后保持 Wi-Fi 连接并启动 HTTP 测试接口。
- 再次断电或复位后，由于自动连接配置仍关闭，设备重新进入等待 BLE 配网状态。
- BLE `reset_wifi` 继续真正删除 NVS 网络配置。
- 关闭自动连接不改变 NVS 命名空间和键名。

## 5. 不变内容

- 不修改 C5 Hosted slave 固件。
- 不修改 ESP-Hosted、SDIO、VHCI 或 NimBLE 初始化。
- 不修改 BLE UUID、JSON 消息格式和 Wi-Fi 扫描协议。
- 不修改微信小程序页面和操作流程。
- 不降低连接重试次数，不删除 NVS 保存能力。
- 不在启动时擦除整个 NVS 分区。

## 6. 验证

1. 使用当前已经保存 `iPhone` 凭据的 P4 启动。
2. 串口不再出现 `saved Wi-Fi found, auto-connect queued`、`credentials accepted` 或自动 `wifi_connecting`。
3. 串口出现 `saved Wi-Fi auto-connect disabled; waiting for BLE credentials`，状态为 `waiting_credentials`。
4. 小程序仍能发现并连接 `SmartScore-WT99-XXXX`。
5. 点击“搜索 Wi-Fi”能够取得 C5 扫描结果。
6. 选择 Wi-Fi 并发送正确密码后，设备获取 IP，BLE 返回 `connected`，HTTP `/api/ping` 可用。
7. 再次重启后仍回到 `waiting_credentials`，不会自动连接刚保存的网络。
8. 临时启用 `CONFIG_SMARTSCORE_AUTO_CONNECT_SAVED_WIFI` 重新构建时，原有自动连接能力可以恢复。

## 7. 交付边界

本次只改变 P4 启动时是否消费已保存凭据。网络扫描、BLE 配网、连接保存和 HTTP 验证保持现有实现；不扩展正式业务功能。
