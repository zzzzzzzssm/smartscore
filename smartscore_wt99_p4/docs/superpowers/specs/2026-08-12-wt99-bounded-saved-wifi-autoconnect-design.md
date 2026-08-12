# WT99 启动旧 Wi-Fi 限时自动连接设计

日期：2026-08-12

## 1. 目标

恢复设备开机后使用 NVS 中已保存 Wi-Fi 的自动连接能力，但把这段启动流程限制为最多 10 次连接触发、总计最多 5 秒。旧网络在限制内连接成功时直接进入正常联网状态；未成功时必须停止旧连接并回到 BLE 配网状态，让用户立即扫描和连接新的 Wi-Fi。

该限制只适用于开机自动连接。用户通过 BLE 主动提交 Wi-Fi 后，继续使用现有的正常连接策略，不缩短其连接时间。

## 2. 当前问题

当前工作区关闭了 `CONFIG_SMARTSCORE_AUTO_CONNECT_SAVED_WIFI`，因此源码默认不会自动使用已保存凭据。旧的自动连接路径一旦启用，会把保存凭据交给通用连接流程：最多尝试 5 次，每次最多等待 12 秒，并带有 1、2、4、8 秒退避。

这条通用流程可能长时间占用 `s_connect_pending`。连接操作占用期间，BLE `scan_wifi` 会返回忙，用户无法及时搜索和提交新网络。开机旧网络与用户主动选择的新网络需要采用两套不同的时间策略。

## 3. 选定方案

不在连接前执行十次完整 Wi-Fi 扫描，而是直接用保存凭据发起 STA 连接，并由 provisioning task 管理一个总截止时间：

- 从开始处理保存凭据时计时，总预算为 5 秒；
- 第一次 `wifi_remote_start_sta()` 计入最多 10 次连接触发；
- 驱动在截止时间前快速报告断开时，使用 `wifi_remote_retry_sta()` 再次触发连接；
- 每次等待 GOT_IP 或断开事件时，只等待当前剩余预算；
- 达到 10 次或总计 5 秒时立即结束，以先到者为准；
- 认证失败属于不可通过重复搜索恢复的错误，可立即结束启动自动连接，无需耗尽 10 次；
- 找不到 AP、关联失败、普通断开或等待超时可以在剩余预算和次数内重试。

该方案避免完整扫描对全信道和隐藏 SSID的限制，同时允许驱动在旧热点不存在时快速返回并进行多次尝试。最多 10 次是上限，不保证在所有射频环境中恰好执行 10 次；5 秒总截止时间优先。

## 4. 配置

恢复并默认启用：

```text
CONFIG_SMARTSCORE_AUTO_CONNECT_SAVED_WIFI=y
```

新增两个只控制启动保存网络的配置项：

```text
CONFIG_SMARTSCORE_SAVED_WIFI_MAX_ATTEMPTS=10
CONFIG_SMARTSCORE_SAVED_WIFI_TOTAL_TIMEOUT_SECONDS=5
```

现有配置继续只用于 BLE 或 Web 主动提交凭据后的正常连接：

```text
CONFIG_SMARTSCORE_WIFI_MAX_RETRIES=5
CONFIG_SMARTSCORE_WIFI_ATTEMPT_TIMEOUT_SECONDS=12
```

启动策略与主动配网策略不能共用超时值，防止以后调整其中一条路径时意外改变另一条路径。

## 5. 状态和数据流

### 5.1 没有保存凭据

`network_provisioning_start()` 读取 NVS 失败或没有有效 SSID时，直接进入 `NETWORK_STATE_WAITING_CREDENTIALS`。BLE 保持广播并接受 `scan_wifi`、`set_wifi` 和 `get_status`。

### 5.2 保存网络在 5 秒内连接成功

1. 从 NVS 读取 SSID 和密码，创建带有 `from_saved_credentials=true` 的连接命令。
2. 状态进入 `NETWORK_STATE_WIFI_CONNECTING`。
3. 使用启动限时策略等待 `IP_EVENT_STA_GOT_IP`。
4. 成功后进入 `NETWORK_STATE_WIFI_CONNECTED`，保留已保存凭据，并按现有流程启动设备 HTTP API。
5. BLE 继续通知 `connected` 和设备 IP。

### 5.3 保存网络未在 5 秒内连接成功

无论原因是热点不存在、关联失败、认证失败、启动错误或截止时间到达，都执行以下回退：

1. 调用 `wifi_remote_stop()`，确保 C5 不再后台寻找旧热点；
2. 清除本次连接的临时密码缓冲；
3. 将状态切换为 `NETWORK_STATE_WAITING_CREDENTIALS`，IP 重置为 `0.0.0.0`；
4. 从 `process_connect()` 返回，由 provisioning task 释放 `s_connect_pending`；
5. BLE 客户端随后可以立即提交 `scan_wifi` 或 `set_wifi`。

启动失败不删除 NVS 中的旧凭据。用户成功连接新网络时，现有 `network_credentials_save()` 会覆盖旧 SSID 和密码；在此之前再次重启，设备仍会对旧网络执行一次最多 5 秒的尝试。

### 5.4 BLE 主动配网

BLE `set_wifi` 创建的命令保持 `from_saved_credentials=false`，继续执行现有策略：最多 5 次、每次最多 12 秒，并保留现有退避、失败原因、NVS 保存和状态通知行为。

在启动自动连接的最多 5 秒内，BLE 可以连接和查询状态，但 Wi-Fi 扫描仍可能暂时返回忙。自动连接一结束，连接占用必须释放，用户无需重启设备即可扫描新网络。

## 6. 代码边界

- `network_provisioning_start()` 继续负责读取 NVS 和标记保存凭据命令。
- `process_connect()` 根据 `from_saved_credentials` 选择启动限时策略或主动配网策略。
- 启动限时策略只管理连接次数、截止时间、错误分类和回退状态，不改变 `wifi_remote` 的公共接口职责。
- `wifi_remote_events.c` 继续只发布 GOT_IP、断开和扫描完成事件；不在事件回调中直接重连。
- BLE UUID、换行 JSON 协议、小程序页面和 C5 Hosted slave 固件保持不变。

## 7. 错误处理和安全

- 5 秒使用 FreeRTOS 单调 tick 计算，不使用墙上时钟；tick 回绕按 FreeRTOS 的有界时间比较处理。
- 每次等待前重新计算剩余预算，绝不为单次等待重新获得完整 5 秒。
- `wifi_remote_start_sta()` 或 `wifi_remote_retry_sta()` 返回错误时，保存网络路径回到等待配网，不进入长期失败占用状态。
- `wifi_remote_stop()` 失败时记录错误，但仍释放 provisioning 连接占用并进入 `NETWORK_STATE_WAITING_CREDENTIALS`；不得形成无限重试循环。
- 日志可记录 SSID、尝试次数、已用时间和失败类型，不得记录密码或完整 BLE 请求。
- 临时密码缓冲继续在所有成功、失败和排队退出路径清零。

## 8. 验证

### 8.1 构建和静态验证

1. ESP-IDF 全量或增量构建通过，无新增告警。
2. 解析后的 `sdkconfig` 显示自动连接启用、最多 10 次、总超时 5 秒。
3. 检查保存凭据路径和 BLE 主动路径使用不同策略。
4. 检查所有保存密码的临时缓冲仍有清零路径。

### 8.2 运行场景

1. **旧热点可用**：设备在 5 秒内获取 IP，BLE 返回 `connected`，HTTP `/api/ping` 可访问。
2. **旧热点不存在**：从启动自动连接开始不超过 5 秒进入 `waiting_credentials`；串口停止输出旧网络连接尝试。
3. **旧密码错误**：认证失败后尽快进入 `waiting_credentials`，不等待完整普通配网重试周期。
4. **BLE 扫描释放**：启动回退后立即发送 `scan_wifi`，C5 返回附近网络，不再持续返回 `wifi_scan_busy`。
5. **连接新网络**：BLE `set_wifi` 使用正常连接策略，成功后保存新凭据并返回新 IP。
6. **再次重启**：设备在 5 秒内自动连接刚保存的新网络。
7. **无保存凭据**：开机直接进入 `waiting_credentials`，不启动无意义连接。
8. **5 秒硬上限**：即使没有收到断开事件，单次等待也在总截止时间到达时退出。

## 9. 非目标

- 不实现连接成功后的长期断线自动重连；该问题应作为独立状态机改动处理。
- 不验证公网、DNS 或云服务可达性；当前联网成功仍以获取 STA IP 为准。
- 不修改 C5 固件、ESP-Hosted、SDIO、VHCI 或 NimBLE 初始化。
- 不自动删除旧 Wi-Fi 凭据，不擦除 NVS 分区。
- 不修改 Web 备用配网默认开关。
