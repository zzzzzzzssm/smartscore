# 谱伴小程序 WT99 通信协议适配设计

日期：2026-07-15

## 1. 目标

以 `D:\qianrushi\smart_score_esp32-main` 中现有“谱伴”微信小程序为界面和功能底稿，在 `D:\qianrushi\smartscore_wt99_p4\miniprogram` 中保留原首页、TabBar、乐谱、练习、记录、我的和智能设备页面，只把智能设备页的通信协议适配为 WT99P4C5-S1 一阶段网络固件。

本次小程序用于验证：发现 WT99、BLE 连接、发送 Wi-Fi SSID/密码、接收连接状态和 IP、访问 `/api/status` 与 `/api/ping`。

## 2. 不变内容

- `app.json` 的原页面顺序、TabBar、标题和主题颜色不变；只使用原有“连接设备”入口。
- 原 `assets/`、页面 WXML/WXSS、首页卡片、图标、排版和业务页面保持不变。
- 不新增独立测试风格页面，不将网络测试页加入 TabBar。
- 乐谱、练习、历史记录和个人页的现有逻辑不在本次重构范围内。
- 原项目 `D:\qianrushi\smart_score_esp32-main` 保持只读，不直接修改。

## 3. 迁入总工程的文件

从原小程序复制 `app.js`、`app.json`、`app.wxss`、`sitemap.json`、`project.config.json`、`assets/`、`pages/` 和 `utils/` 到 `smartscore_wt99_p4/miniprogram/`。不复制原工程的 ESP-IDF `main/`、`build/`、`managed_components/`、`sdkconfig` 或其他固件内容。

当前简易网络测试页不再作为小程序入口；其中经过验证的 UTF-8 编解码、20 字节分片和换行通知拼包逻辑迁入 WT99 通信模块后删除简易页面。

## 4. BLE 协议适配

设备页只优先显示名称以 `SmartScore-WT99-` 开头的设备，并继续按 `deviceId` 去重。

固定 UUID：

| 用途 | UUID |
|---|---|
| Service | `7A6E0001-5C5A-4B11-9A4A-53534D415254` |
| RX，手机写入 | `7A6E0002-5C5A-4B11-9A4A-53534D415254` |
| TX，设备通知 | `7A6E0003-5C5A-4B11-9A4A-53534D415254` |

UUID 比较统一转为大写并处理手机返回格式差异。发现服务时必须找到指定 Service、RX 和 TX，不能退回任意第一个 GATT 服务或任意可写特征。

BLE 消息为 UTF-8 JSON 加换行符。发送端编码完整消息后按最多 20 字节顺序写入，每片间短延时；接收端先合并原始字节，遇到换行后再 UTF-8 解码和 JSON 解析。单条消息最大 512 字节，超过后清空到下一换行并显示错误，不打印敏感原文。

## 5. 命令与页面行为

- 完成通知订阅后发送 `{"id":1,"cmd":"get_status"}`。
- “发送 Wi-Fi 配置”发送 `set_wifi`，包含递增请求 ID、SSID 和密码。
- 原“测试蓝牙”按钮发送 `get_status`，用于确认双向 BLE 通信。
- 手机 Wi-Fi 列表由微信 Wi-Fi API 获取；不再向设备发送 WT99 固件不支持的 `scan_wifi` 命令。用户仍可手动输入 SSID。
- 收到 `wifi_state/connecting` 时，原页面显示正在连接。
- 收到 `wifi_state/connected` 时，保存 `http://<ip>` 为设备地址，清空内存中的密码，并自动请求 `/api/status`。
- 收到 `wifi_state/failed` 时，显示失败原因，保留 BLE 连接供用户重新提交。
- 收到 `waiting_credentials` 时回到等待配网状态。
- 收到 `device_info` 时更新 BLE/Wi-Fi 能力提示，不改变页面布局。

## 6. HTTP 适配

设备页只使用本阶段存在的接口：

- `GET /api/status`：判断网络服务可达并显示连接状态、IP、重试次数。
- `GET /api/ping`：作为 HTTP 辅助验证。

其他业务页面保留原样；本阶段固件没有正式乐谱、评分或 AI API，因此这些页面不作为本次硬件验收对象，也不为适配它们而扩展 P4 固件范围。

## 7. 生命周期与安全

- BLE 监听器只注册一次；页面卸载时停止扫描、清理定时器、关闭 BLE 连接和适配器，并在 API 可用时注销监听器。
- 不写死 `deviceId` 或设备 IP。
- 密码输入保持 `password` 类型，不写入 storage，不输出 console，不放入原始日志。
- 连接或认证失败不自动重启设备，小程序允许重新提交。

## 8. 验证

1. 比较原小程序与迁入版本的 UI 文件和资源，确认本次协议适配没有改变 WXML/WXSS、TabBar 和图标。
2. 对全部 JavaScript 执行语法检查。
3. 静态检查必需微信 BLE API、固定 UUID、20 字节分片、换行拼包和卸载清理。
4. 静态检查不存在密码日志、旧 `FFF0/FFF1/FFF2` 协议和 `scan_wifi` BLE 命令。
5. 重新构建 ESP-IDF 5.4.4 固件，确认小程序迁入总工程不影响 P4 构建。
6. 真机验收由用户烧录 P4 后完成：BLE 搜索、连接、错误密码重试、正确密码获 IP、HTTP Ping、断电自动连接。

## 9. 交付边界

交付结果是原“谱伴”UI 加 WT99 通信适配，不是重新设计 UI，也不是实现乐谱、评分、AI 等 P4 正式业务接口。原小程序源目录和 C5 固件均不修改。
