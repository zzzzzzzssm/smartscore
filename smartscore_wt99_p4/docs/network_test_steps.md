# WT99 一阶段网络固件操作步骤

## 1. 打开 ESP-IDF 终端

本工程使用 ESP-IDF 5.4.4。ESP-IDF 5.1.2 的目标列表不包含 ESP32-P4，因此不能用于 WT99P4C5-S1。

推荐从 Windows 开始菜单打开 “ESP-IDF 5.4 PowerShell”，然后进入工程：

```powershell
cd D:\qianrushi\smartscore_wt99_p4
idf.py --version
```

普通 PowerShell 也可使用工程辅助脚本：

```powershell
cd D:\qianrushi\smartscore_wt99_p4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\idf54.ps1 --version
```

## 2. 配置目标并编译

```powershell
idf.py set-target esp32p4
idf.py build
```

若使用辅助脚本：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\idf54.ps1 set-target esp32p4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\idf54.ps1 build
```

成功时应看到 `Project build complete`，产物是 `build\smartscore_wt99_p4.bin`。不要手工修改生成的 `sdkconfig` 或 `dependencies.lock`。

## 3. 烧录 P4

本任务没有自动烧录。把 `COMx` 替换为连接 P4 USB 下载口后出现的实际串口：

```powershell
idf.py -p COMx flash
```

这里只烧录 P4。不要执行 C5 烧录或 `erase-flash`，除非后续另行明确确认。

## 4. 打开串口

```powershell
idf.py -p COMx monitor
```

通常按 `Ctrl+]` 退出。首次启动请保存从复位开始的完整日志，尤其是 ESP-Hosted slave 版本、chip/capability、SDIO 和 HCI 信息。

板级准备的预期日志：

```text
[NET] SmartScore WT99 stage-one network starting
[NET] preparing C5 transport
[BOARD] C5 uses the board's always-on 3.3 V rail
[BOARD] C5 reset GPIO=54; reset pulse owned by ESP-Hosted
[BOARD] C5 SDIO prepared: slot=1 width=4 clock=40000 kHz CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 reset=54
[BOARD] C5 transport prepared
```

Hosted 和 BLE 成功后应继续看到类似：

```text
[HOSTED] initialization started
[HOSTED] initialization complete
[HOSTED] Wi-Fi capability available
[HOSTED] BLE capability available
[BLE] device name: SmartScore-WT99-XXXX
[BLE] NimBLE host initialized
[BLE] advertising started
[NET] remote capabilities: WIFI=YES BLE=YES
```

日志文字可能带 ESP-IDF 的时间戳和 tag。若看到 `C5 hosted firmware does not expose Bluetooth capability`，说明当前板载 C5 没有向 P4 暴露可用 HCI，不能把 BLE 判为成功；先保存完整日志，不要直接烧 C5。

## 5. 导入微信小程序

本次将原“谱伴”小程序界面迁入 P4 总工程内的 `D:\qianrushi\smartscore_wt99_p4\miniprogram`。原首页、TabBar、业务页面、配色和图片保持不变，只适配智能设备页通信协议；原小程序目录不修改。

1. 打开微信开发者工具，选择“导入项目”。
2. 项目目录选择 `D:\qianrushi\smartscore_wt99_p4\miniprogram`。
3. 使用原项目 AppID，或换成自己有权限的 AppID 后进行真机调试。
4. 开发者工具访问局域网 HTTP 时，可在“详情 → 本地设置”启用“不校验合法域名”。
5. 真机授予微信蓝牙、定位（部分 Android 扫描需要）和本地网络权限。

## 6. BLE 搜索和连接

1. 保持原首页，点击“连接设备”卡片进入“智能设备”。
2. 在“1. 连接蓝牙”区域点击“扫描蓝牙”；小程序会自动打开蓝牙适配器。若失败，检查手机蓝牙、系统权限和微信权限。
3. 选择 `SmartScore-WT99-XXXX`。页面会按 `deviceId` 去重，不写死设备 ID。
4. 页面应依次显示 WT99 服务已发现、通知已开启、蓝牙设备已连接。
5. 原“测试蓝牙”按钮现在发送 `get_status`，可再次验证双向通信。

## 7. 发送 Wi-Fi 配置

1. 在“2. 配置 Wi-Fi”点击“搜索 Wi-Fi”。小程序通过 BLE 发送 `scan_wifi`，P4 再通过 C5 扫描附近热点；该操作不会打开手机系统 Wi-Fi 列表。也可以手动输入 SSID。
2. 等待页面显示设备返回的热点（按信号排序、最多 15 个），选择一个 SSID 后输入密码。中文 SSID 会按 UTF-8 发送；密码框为隐藏输入，不永久保存。
3. 点击“发送 Wi-Fi 配置”，应先收到 `connecting`。
4. 错误密码应在有限重试后收到 `failed` 和原因；设备不重启，BLE 保持可用，可以重新提交。
5. 正确密码应在获取 IP 后收到 `connected` 和实际 IP。只有这时凭据才写入 NVS。
6. 收到 IP 后，小程序自动填写 `http://设备IP`，并访问 `/api/status` 和 `/api/ping`。串口应出现 `[WIFI] got IP: ...` 与 `[HTTP] /api/ping ready`。

`/api/ping` 期望响应：

```json
{"ok":true,"device":"SmartScore-WT99","network":"connected","ip":"192.168.1.100"}
```

BLE 收到 `connected` 和 IP 是本阶段主要成功标准，HTTP Ping 是辅助验证。Ping 失败但 BLE 已 connected 时，先检查手机和设备是否在同一局域网、微信本地网络权限和域名校验设置。

## 8. 重启与状态查询

- 默认启用 `CONFIG_SMARTSCORE_AUTO_CONNECT_SAVED_WIFI`。断电重启后，设备读取 `smartscore_net` 中的配置并尝试旧网络，最多触发 10 次连接，全部尝试共享 5 秒总预算。
- 旧网络在 5 秒内可用时，设备自动获取 IP 并恢复 `wifi_connected`；BLE `get_status` 返回已连接状态和 IP。
- 旧网络不存在或密码失效时，设备最迟在 5 秒预算结束后停止旧连接并进入 `waiting_credentials`。此时 BLE `scan_wifi` 必须可以立即扫描附近网络，用户无需重启设备即可选择并提交新 Wi-Fi。
- BLE 主动提交的新 Wi-Fi 仍使用正常的 5 次、每次 12 秒连接策略；成功后新凭据覆盖旧配置，下一次重启应自动连接新网络。

## 9. 网页备用配网

默认关闭。需要验证时运行 `idf.py menuconfig`，启用 `CONFIG_SMARTSCORE_WEB_PROVISIONING_FALLBACK`，重新编译并手动烧录。

设备等待凭据或 BLE 不可用时启动 `SmartScore_Setup`。手机连接热点后打开 `http://192.168.4.1/`，验证 Wi-Fi 扫描、SSID/密码提交、状态轮询和 IP 显示。连接成功后页面保留约 5 秒，再关闭备用 AP/网页并启动网络诊断 API。

## 10. 常见错误

| 现象 | 排查方式 |
|---|---|
| `Hosted configuration rejected` | 检查 `sdkconfig.defaults.esp32p4` 是否仍为 slot 1、4-bit、40 MHz、GPIO14～19、reset GPIO54，以及 C5 target |
| SDIO/Hosted timeout | 保存完整启动日志；核对供电、GPIO54 复位、C5 slave 固件和 transport，不使用 J14 的 GPIO39～44 |
| C5 不提供 Bluetooth | 当前 C5 固件可能不支持 BLE/HCI 或与 Host 不兼容；先核对日志，不自动烧录 C5 |
| 找不到设备 | 确认 `BLE capability available` 和 `advertising started`，检查手机权限并停止其他扫描器 |
| 找不到 Service/Characteristic | 核对固定 UUID、C5 BLE capability 和通知订阅步骤 |
| `AUTH_FAIL` | 密码错误或认证不兼容，重新提交即可，不需要重启 |
| `NO_AP_FOUND` | 检查 SSID、信号、2.4 GHz 可用性和隐藏网络 |
| BLE connected 但 Ping 失败 | 检查同一局域网、微信本地网络权限和开发者工具域名校验 |
