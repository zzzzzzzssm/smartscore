# WT99 板端 Wi-Fi 扫描设计

日期：2026-07-16

## 1. 目标

在现有 WT99P4C5-S1 网络工程中实现与旧项目一致的配网交互：微信小程序先通过 BLE 连接设备，再要求设备扫描附近 Wi-Fi；ESP32-P4 通过 ESP-Hosted/esp_wifi_remote 驱动板载 ESP32-C5 扫描，结果通过 BLE 返回小程序。手机自身不参与 Wi-Fi 扫描。

本设计只增加板端 Wi-Fi 扫描链路，不改变现有 `set_wifi`、`reset_wifi`、`get_status` 和 `get_device_info` 命令，也不修改旧项目 `C:\Users\17762\Desktop\ESP`。

本设计覆盖并纠正 `2026-07-15-wt99-miniprogram-protocol-adaptation-design.md` 中“使用微信手机 Wi-Fi API、设备不支持 scan_wifi”的旧结论。

## 2. 旧项目依据

旧项目已经实现正确的板端扫描流程：

- 小程序发送 `scan_wifi`。
- BLE 回调启动异步扫描工作。
- 板端主动扫描热点、跳过空 SSID、按 SSID 去重并保留更强的记录。
- 设备依次通知 `wifi_scan_start`、多条 `wifi_network`、`wifi_scan_done`。
- 小程序收到热点后继续按 SSID 去重、按 RSSI 降序显示，并允许用户选择 SSID。

参考文件：

- `C:\Users\17762\Desktop\ESP\master_board_st7796\miniprogram\pages\device\device.js`
- `C:\Users\17762\Desktop\ESP\master_board_st7796\main\ble_provisioning.c`
- `C:\Users\17762\Desktop\ESP\master_board_st7796\main\network_manager.c`

WT99 版本复用该交互方式，但不复制旧 16 位 UUID、旧板级代码或旧 Wi-Fi 驱动实现。

## 3. 端到端流程

1. 用户完成 WT99 BLE 连接、服务发现和通知订阅。
2. 用户点击“搜索 Wi-Fi”。
3. 小程序通过现有 RX 特征发送一条换行结尾的 JSON 命令：

   ```json
   {"id":5,"cmd":"scan_wifi"}
   ```

4. BLE GATT 写入回调只完成消息校验和命令入队，不执行扫描。
5. `network_provisioning` 的工作任务消费扫描命令并调用 `wifi_remote_scan()`。
6. `wifi_remote` 通过 ESP-Hosted/esp_wifi_remote 让 ESP32-C5 执行扫描。
7. 扫描结果跳过空 SSID，按 SSID 去重，保留信号更强的记录，按 RSSI 从强到弱排序，最多返回 15 个。
8. 设备通过 TX 通知逐条返回扫描状态和热点。
9. 小程序展示列表；用户选择热点后只填充 SSID，密码仍由用户输入。
10. 用户点击配网后继续使用现有 `set_wifi` 命令连接路由器。

若设备已经连接 Wi-Fi 或正在连接，小程序不主动发起扫描；用户仍可以手动输入 SSID。

## 4. BLE 协议

沿用当前固定 128 位 UUID、UTF-8 JSON、换行分帧、最大单消息 512 字节和每片最多 20 字节的规则。

### 4.1 扫描开始

```json
{"id":5,"event":"wifi_scan","status":"wifi_scan_start"}
```

### 4.2 单条热点

```json
{"id":5,"event":"wifi_network","status":"wifi_network","ssid":"Example","rssi":-48,"channel":6,"open":false}
```

### 4.3 扫描完成

```json
{"id":5,"event":"wifi_scan","status":"wifi_scan_done","count":12}
```

### 4.4 扫描失败

```json
{"id":5,"event":"wifi_scan","status":"failed","reason":"wifi_scan_failed"}
```

忙碌或任务无法接收时使用明确原因：

- `wifi_scan_busy`
- `wifi_scan_queue_full`
- `wifi_scan_failed`
- `wifi_not_ready`

保留旧项目的 `status` 字段语义，增加当前 WT99 协议使用的请求 `id` 和 `event` 字段，避免破坏现有统一消息分发。

热点逐条通知，消息之间保留约 160 ms 间隔，避免 BLE 通知队列被一次性填满。单条通知不得包含 Wi-Fi 密码。

## 5. 固件职责

### 5.1 ble_provisioning

- 识别 `scan_wifi` 命令。
- 校验当前 BLE 会话和消息格式。
- 将包含请求 ID 的扫描命令投递到 `network_provisioning` 队列。
- 立即返回 GATT 写入回调。
- 提供扫描状态和单条热点通知编码，不直接调用阻塞扫描。

### 5.2 network_provisioning

- 新增 `NETWORK_COMMAND_SCAN`。
- 只允许一个扫描操作运行。
- 在 provisioning task 中执行扫描并组织通知。
- 扫描失败不重启设备，不断开 BLE，不破坏已有凭据。
- 扫描不保存 SSID 或密码到 NVS。

### 5.3 wifi_remote

- 复用现有 `wifi_remote_scan()` 和远端 Wi-Fi 驱动。
- 确保扫描前 Wi-Fi Remote 已初始化且 STA 能力可用。
- 将“启动 STA”和“发起连接”解耦：`WIFI_EVENT_STA_START` 只记录启动状态，不再无条件调用 `esp_wifi_connect()`；`wifi_remote_start_sta()` 在写入有效配置并成功启动 STA 后显式调用 `esp_wifi_connect()`。
- `wifi_remote_scan()` 在 Wi-Fi 尚未启动时可以启动不带连接动作的 STA 扫描模式；扫描所需的 Wi-Fi 启动不能触发对空配置或旧配置的意外自动连接。
- 若扫描前 Wi-Fi 已因网页 AP/APSTA 或其他有效网络状态运行，则保持原模式执行扫描，不为扫描破坏当前接口状态。
- 返回 SSID、RSSI、信道和认证模式；上层将认证模式转换为 `open`。
- 不包含任何 BLE 协议或小程序行为。

## 6. 小程序改动

- 删除设备页 Wi-Fi 搜索流程中的 `wx.startWifi`、`wx.getWifiList`、`wx.onGetWifiList` 和对应解绑逻辑。
- “搜索 Wi-Fi”按钮改为发送 `scan_wifi` BLE 命令。
- 处理 `wifi_scan_start`、`wifi_network`、`wifi_scan_done` 和失败原因。
- 热点按 SSID 去重，保留更强 RSSI，按信号从强到弱排序，最多显示 15 个。
- UUID 比较、20 字节 BLE 分片、换行拼包和 UTF-8 解码继续沿用现有实现。
- 不请求或引导手机进入 Wi-Fi 列表权限；BLE 权限和本地网络权限仍按实际功能处理。
- 保留手动 SSID 输入；隐藏网络不进入扫描列表。
- 选择开放热点时清空密码；密码输入和发送过程中不写控制台、不持久化。
- 扫描超时后恢复按钮状态并提示用户重试或手动输入。

## 7. 并发与错误处理

- BLE 回调不得阻塞。
- 扫描、连接和清除配置都由同一受控命令队列串行化，避免扫描与连接竞争远端 Wi-Fi 驱动。
- Wi-Fi 正在连接时拒绝扫描并返回 `wifi_scan_busy`。
- 扫描期间收到新的扫描请求时返回 `wifi_scan_busy`。
- BLE 中途断开时扫描可以安全结束，但停止发送后续通知；不能访问失效连接句柄。
- 通知队列满时不得崩溃；扫描任务应限时等待或结束并报告错误。
- 扫描失败后继续保持 BLE 广播或现有 BLE 连接。

## 8. 验证标准

1. iPhone 点击“搜索 Wi-Fi”不再跳到微信 Wi-Fi 权限设置页。
2. 串口日志能看到 BLE 收到 `scan_wifi`、P4 发起远端扫描以及返回热点数量。
3. C5 附近的热点出现在小程序列表，最多 15 个，按 RSSI 降序且无重复 SSID。
4. 中文 SSID 能正确显示。
5. 隐藏 SSID 不显示，但可以手动输入。
6. 选择热点并发送密码后仍通过现有 `set_wifi` 完成连接。
7. 扫描期间 BLE GATT 回调不发生长时间阻塞。
8. 连续快速点击扫描不会创建多个扫描任务或导致设备崩溃。
9. 扫描失败后 BLE 保持可用，可以重新扫描或手动配网。
10. 日志和 BLE 原始日志不包含 Wi-Fi 密码。

## 9. 不在本次范围内

- 手机自身的 Wi-Fi 扫描与系统 Wi-Fi 选择页面。
- 修改或重新烧录 C5 固件。
- 网页备用配网流程改造。
- 屏幕、音频、乐谱、评分、AI、SD 卡及其他业务模块。
