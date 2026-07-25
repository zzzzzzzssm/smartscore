# WT99P4C5-S1 网络模块第一阶段设计

> 实施更新：本文件中的独立 `pages/network-test` 方案已被用户后续要求替代。实际交付采用原“谱伴”UI，并只适配 `pages/device/device` 的 WT99 通信协议；以 `2026-07-15-wt99-miniprogram-protocol-adaptation-design.md` 为准。

日期：2026-07-15

## 1. 目标与验收边界

本阶段仅实现并验证以下链路：微信小程序发现并连接 BLE，使用换行分隔的 UTF-8 JSON 分片传输 Wi-Fi 凭据，ESP32-P4 经板载 ESP32-C5 连接 Wi-Fi，设备通过 BLE 返回连接状态和实际 IP，成功后启动最小 HTTP `/api/ping` 与 `/api/status`。

屏幕、LVGL、音频、MIDI、乐谱、实时跟谱、AI、评分、三块 S3、摄像头、SD 卡和正式业务 API 均不进入启动流程或网络组件依赖图。

## 2. 已确认事实与安全约束

- 旧工程位于 `D:\qianrushi\M5stack_SmartMusic.zip`，仅作为只读参考。
- 旧工程使用 `espressif/esp_hosted` 2.12.6、`espressif/esp_wifi_remote` 1.6.0，并在 M5Stack Tab5 BSP 开启 Wi-Fi 后调用 `esp_hosted_init()`。
- 旧硬件是 ESP32-P4 与 ESP32-C6，传输为 M5Stack 专属 4-bit、40 MHz SDIO；该板级配置不得用于 WT99。
- 当前工作区没有 WT99P4C5-S1 原理图、厂商网络示例或可信的 P4-C5 传输引脚资料，不能确认 SDIO、SPI、SPI-HD 或 UART，也不能确认复位、电源、极性、总线宽度和时钟。
- 当前无法确认板载 C5 固件是否包含 Wi-Fi 与 Bluetooth Hosted 能力。
- 已找到原微信小程序 `D:\qianrushi\smart_score_esp32-main`，因此不修改原项目，创建隔离测试小程序。实现完成后按用户后续要求将该工具移动到 `D:\qianrushi\smartscore_wt99_p4\miniprogram`。

## 3. 硬件阻塞策略

`board_wt99` 集中声明：

- `board_wt99_network_power_enable()`
- `board_wt99_c5_reset()`
- `board_wt99_c5_transport_prepare()`

缺少硬件依据时，这些接口不操作 GPIO，并返回 `ESP_ERR_NOT_SUPPORTED`。默认关闭 `CONFIG_SMARTSCORE_WT99_TRANSPORT_CONFIRMED`。`app_main` 检测到未确认后打印明确错误并停止进入 Hosted、Wi-Fi 和 BLE 控制器路径，但保持系统运行并周期打印阻塞状态。

ESP-Hosted 组件可能具有开发板默认配置，但应用在确认开关关闭时不得调用 `esp_hosted_init()`，避免默认引脚触碰未知硬件。后续获得资料后，只修改 `board_wt99`、其 Kconfig 和板级默认配置，不改网络业务组件。

## 4. 组件架构

### 4.1 board_wt99

只负责 C5 电源、复位和 P4-C5 传输准备。不得接收 SSID、注册 BLE、访问 NVS 或创建 HTTP 路由。

### 4.2 wifi_remote

负责一次性初始化 `esp_netif`、默认事件循环和 ESP-Hosted，初始化/停止 `esp_wifi_remote` 代理 Wi-Fi，注册并保存 Wi-Fi/IP handler instance，维护 STA/AP/APSTA netif，执行扫描、有限重试、获取实际 IP 和状态快照。

Wi-Fi 断开事件只发布内部事件。重试由独立任务按有限次数和退避执行，最多五次。只有 `IP_EVENT_STA_GOT_IP` 才发布成功。

Hosted 能力检查分两层：使用 Hosted 初始化日志与协处理器信息记录 Wi-Fi/HCI 能力；NimBLE Host 必须在规定时间内完成 controller sync。若 Hosted 未暴露 HCI/BLE 或 sync 超时，BLE 初始化返回 `ESP_ERR_NOT_SUPPORTED`，打印 `C5 hosted firmware does not expose Bluetooth capability`。不得仅因 P4 端编译启用了 NimBLE 就声称 C5 支持 BLE。

### 4.3 network_provisioning

拥有唯一 provisioning task、命令 Queue、网络状态机、NVS 凭据和观察者广播。

NVS 使用命名空间 `smartscore_net`，键为 `wifi_ssid`、`wifi_pass`、`wifi_valid`。只有获取 IP 后才写入三个键并提交。清除操作擦除键、断开 STA、停止测试 HTTP，并返回等待凭据状态，不重启设备。

状态包括：

- `NETWORK_STATE_UNINITIALIZED`
- `NETWORK_STATE_HOSTED_STARTING`
- `NETWORK_STATE_HOSTED_READY`
- `NETWORK_STATE_BLE_ADVERTISING`
- `NETWORK_STATE_BLE_CONNECTED`
- `NETWORK_STATE_WAITING_CREDENTIALS`
- `NETWORK_STATE_WIFI_CONNECTING`
- `NETWORK_STATE_WIFI_CONNECTED`
- `NETWORK_STATE_WIFI_FAILED`
- `NETWORK_STATE_WEB_FALLBACK`
- `NETWORK_STATE_ERROR`

状态和 IP 由 mutex 保护。BLE 与网页模块通过提交命令和订阅状态访问该组件，不能直接操作 Wi-Fi。

### 4.4 ble_provisioning

ESP32-P4 运行 NimBLE Host，通过 ESP-Hosted VHCI 使用 C5 Bluetooth Controller。BLE GATT 写回调仅完成分片累积、边界检查、完整消息解析和 Queue 投递，不启动 Wi-Fi、不等待事件、不写 NVS。

广播名称为 `SmartScore-WT99-XXXX`，后缀使用设备 MAC 最后两字节。UUID 只在 `ble_provisioning_protocol.h` 定义：

- Service：`7A6E0001-5C5A-4B11-9A4A-53534D415254`
- RX：`7A6E0002-5C5A-4B11-9A4A-53534D415254`
- TX：`7A6E0003-5C5A-4B11-9A4A-53534D415254`

RX 支持 WRITE 和 WRITE WITHOUT RESPONSE，TX 支持 READ 和 NOTIFY。收发消息均为 UTF-8 JSON 加 `\n`，最多 512 字节；通知和小程序写入均按最多 20 字节分片。超限立即清空接收缓存并返回 `MESSAGE_TOO_LARGE`。

### 4.5 web_provisioning

由 `CONFIG_SMARTSCORE_WEB_PROVISIONING_FALLBACK` 控制，默认关闭。启用后提供 `SmartScore_Setup` AP 和以下路由：

- `GET /`
- `POST /connect`
- `GET /reset`
- `GET /api/wifi/scan`
- `GET /api/status`

页面放在 `provisioning_page.h`，显示扫描结果、连接中状态、轮询状态和成功 IP。网页与 BLE 共用 `network_provisioning` 命令接口，不依赖任何旧业务组件。

### 4.6 network_test_api

只在获得 STA IP 后启动。提供：

- `GET /api/ping`
- `GET /api/status`

`/api/ping` 返回设备名、connected 状态和实际 IP。若网页 fallback HTTP 服务器正在运行，状态机先停止 fallback server，再启动测试 server，避免端口冲突。

## 5. 数据流与并发

1. `app_main` 初始化 NVS，调用 `board_wt99` 准备硬件。
2. 板级准备成功后，`wifi_remote` 初始化 Hosted、netif 和事件。
3. Hosted 确认 HCI 路径后启动 NimBLE Host 和广播。
4. BLE RX 回调按字节累积，收到换行后解析 JSON。
5. `set_wifi`、`reset_wifi` 等命令进入 provisioning Queue，GATT 回调立即返回。
6. provisioning task 执行 Wi-Fi 生命周期，等待事件组结果，进行有限重试。
7. 获取 IP 后保存 NVS，广播 connected 状态，启动最小 HTTP API。
8. 启动时若 `wifi_valid` 为真，provisioning task 自动提交一次连接命令；BLE 仍可广播、连接和查询状态。

## 6. BLE 消息行为

支持 `get_status`、`set_wifi`、`reset_wifi` 和 `get_device_info`。响应保持请求 `id`，状态通知使用 `event: "wifi_state"`，设备信息使用 `event: "device_info"`。

密码不得写入日志、BLE 原始日志、HTTP 响应或测试文档。日志只允许记录 SSID。认证失败原因映射为稳定字符串，如 `AUTH_FAIL`、`NO_AP_FOUND`、`TIMEOUT` 和 `HOSTED_UNAVAILABLE`。

## 7. 小程序设计

创建独立测试项目（最终位置为 `D:\qianrushi\smartscore_wt99_p4\miniprogram`）。它不修改原小程序，实现单一 `pages/network-test/index` 页面及 `ble_protocol.js`、`utf8.js`、`network_api.js`。

页面显式展示蓝牙适配器、扫描、连接、GATT 服务、通知订阅、Wi-Fi 状态和 IP。设备列表按 `SmartScore-WT99-` 前缀优先排序并按 deviceId 去重，不写死 deviceId 或 IP。

UUID 比较统一转大写，并对 16/32/128 位字符串进行规范化。BLE 事件监听器只注册一次；页面卸载时停止扫描、关闭连接并移除可移除的监听器。UTF-8 编解码在字节层完成，支持中文 SSID和跨通知分片的多字节字符。密码仅保存在页面内存，输入框为 password，不写控制台或永久存储。

HTTP Ping 仅在收到 connected 与 IP 后启用，并提示开发者工具域名校验和真机本地网络权限限制。

## 8. 构建与依赖

使用本机 ESP-IDF 5.4 环境，不使用当前错误指向的 5.1.2。依赖固定为旧工程验证版本：

- `espressif/esp_hosted: 2.12.6`
- `espressif/esp_wifi_remote: 1.6.0`

不声明 `m5stack_tab5`，不复制 `managed_components`，不手工编写 `dependencies.lock`。组件管理器可在 reconfigure/build 时生成依赖目录和锁文件。`sdkconfig.defaults.esp32p4` 只放通用 NimBLE Hosted VHCI、网络测试与安全开关，不写任何 WT99 传输 GPIO、极性或时钟。

顶层构建限制组件集合，使 `main` 仅依赖 `board_wt99`、`wifi_remote`、`network_provisioning`、`ble_provisioning`、`web_provisioning` 和 `network_test_api`，不会编译或链接屏幕、音频、乐谱和业务组件。

## 9. 验证策略

首先执行静态检查和可在主机运行的消息编解码测试，再执行：

1. 激活 ESP-IDF 5.4。
2. `idf.py set-target esp32p4`。
3. `idf.py reconfigure`。
4. `idf.py build`。

若构建失败，从第一个真实编译错误开始修复，不屏蔽必要组件。由于缺少板级资料，不执行 flash，也不宣称完成实际 P4-C5、BLE 或 Wi-Fi 硬件验收。

## 10. 交付文档

生成：

- `docs/network_migration_report.md`
- `docs/ble_provisioning_protocol.md`
- `docs/network_test_steps.md`
- `docs/c5_firmware_requirement.md`

迁移报告必须区分“编译完成”“板级适配阻塞”和“硬件未验证”，记录实际依赖版本、小程序处理方式、构建结果和第一个剩余错误。

## 11. 明确不实施的事项

- 不复制或修改旧工程与其 `managed_components`。
- 不采用 M5Stack Tab5、ESP32-P4 Function EV Board 或 ESP32-P4-C5 Core Board的默认引脚作为 WT99 引脚。
- 不自动烧录 P4 或 C5，不修改 C5 固件。
- 不生成或提交 `sdkconfig`，不执行 Git commit。
- 不伪造 Hosted capability、BLE 广播、Wi-Fi IP 或硬件测试通过结果。
