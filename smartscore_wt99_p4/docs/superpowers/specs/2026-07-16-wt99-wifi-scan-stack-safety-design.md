# WT99 Wi-Fi 扫描栈安全修复设计

日期：2026-07-16

## 1. 问题与证据

微信小程序通过 BLE 发送 `scan_wifi` 后，ESP32-P4 出现 panic，随后日志打印
`SW_CPU_RESET` 并重新执行完整启动流程。BLE 断开是 P4 重启的结果，不是手机主动
断开连接。

当前 `process_scan()` 在 provisioning task 栈中同时创建 32 条原始扫描记录和 15 条
筛选记录。编译后的函数栈帧约为 4640 字节，而 provisioning task 的总栈大小为
6144 字节。函数继续调用 ESP-Hosted、Wi-Fi Remote、日志和 BLE 通知路径时，可用栈
空间不足，存在明确的栈溢出风险。

C5 固件已经启用 Wi-Fi/Bluetooth 软件共存，因此本次先修复 P4 本地栈使用问题，不
修改 C5 固件、SDIO 配置或 BLE 消息协议。

## 2. 方案比较

### 方案 A：扫描缓冲区使用堆内存（采用）

在 `process_scan()` 中动态申请原始扫描记录和筛选结果缓冲区，退出函数前统一释放。
这样可以从根本上移除约 4 KB 以上的大型局部数组，同时保留现有 32 条原始记录、
15 条返回结果和 RSSI 排序行为。

### 方案 B：只扩大 provisioning task 栈

实现简单，但任务整个生命周期都会占用更多内部 RAM，也不能防止后续扫描结构增长
再次逼近栈上限，因此不采用。

### 方案 C：减少扫描记录数量

能够降低栈占用，但会丢失附近热点，且仍然保留大数组压栈的结构性风险，因此不采用。

## 3. 代码设计

只修改 `components/network_provisioning/src/network_provisioning.c`：

1. `process_scan()` 在发布扫描开始事件后，为原始记录和筛选记录分别申请清零的堆缓冲。
2. 任一申请失败时，释放已经申请成功的缓冲，并发布扫描失败通知。
3. 内存不足原因使用 `wifi_scan_no_memory`，不触发 abort、重启或 BLE 断开。
4. 扫描成功、Wi-Fi API 失败、BLE 通知失败等所有退出路径都统一释放缓冲。
5. 保留当前异步命令队列、去重、按 RSSI 排序、最多 15 条结果和 160 ms 通知间隔。
6. 不改变 `scan_wifi` 命令及 `wifi_scan_start`、`wifi_network`、`wifi_scan_done` 消息。
7. 在开始和结束扫描时记录 provisioning task 的栈高水位；日志不包含 Wi-Fi 密码。

小程序只增加 `wifi_scan_no_memory` 的中文错误提示，不改变 BLE 调用流程。

## 4. 资源与错误处理

扫描缓冲只在一次扫描期间存在，完成或失败后立即释放。所有清理通过单一出口完成，
防止早退造成内存泄漏。扫描申请失败时保持当前 BLE 连接和通知订阅状态，用户可以稍后
重新搜索或手动输入 SSID。

不通过 `ESP_ERROR_CHECK` 处理扫描可恢复错误，也不重启 P4 或 C5。

## 5. 验证标准

1. ESP-IDF 5.4.4 完整构建成功。
2. 反汇编中 `process_scan()` 不再包含约 4 KB 的栈扩展。
3. 连续触发 Wi-Fi 扫描 5 次，P4 不出现 panic 或重启。
4. 每次扫描期间 BLE 连接和通知保持可用。
5. 每次收到 `wifi_scan_start`、0 到 15 条 `wifi_network` 和 `wifi_scan_done`。
6. 扫描后仍能选择热点、发送密码、获取 IP 并访问 `/api/ping`。
7. 人为制造内存不足时返回 `wifi_scan_no_memory`，设备不重启。

## 6. 范围外内容

本修复不调整 C5 固件、Hosted 版本、SDIO 引脚或时钟，不改变 NVS 自动连接策略，
也不迁移屏幕、音频、MIDI、乐谱或其他业务模块。
