# WT99 网络模块第一阶段迁移报告

报告日期：2026-07-15

## 1. 审查结论与旧工程实际情况

旧参考工程来自 `D:\qianrushi\M5stack_SmartMusic.zip`。本次只读审查，没有修改或删除旧工程。

- 旧 `main/main.c` 通过 `bsp_feature_enable(BSP_FEATURE_WIFI, true)` 启用 M5Stack Tab5 BSP，再调用 `esp_hosted_init()`；网络组件随后使用远端 `esp_wifi_*` API。
- 旧硬件为 ESP32-P4 + ESP32-C6，Tab5 BSP 使用 SDIO。Tab5 的板级引脚和 C6 配置没有复制到 WT99。
- 旧 `wifi_manager` 提供 AP、STA、APSTA、扫描和状态，但断线后存在立即、无限重连，handler 生命周期不集中，部分可恢复错误使用 `ESP_ERROR_CHECK`。
- 旧 NVS namespace 是 `net_cfg`，键为 `ssid`、`password`。
- 旧 `network_manager.c` 将 HTML、五个配网路由和乐谱/评分/AI/上传等业务混在约 746 行文件中。
- 已识别旧路由：`GET /`、`POST /connect`、`GET /reset`、`GET /api/wifi/scan`、`GET /api/status`。
- 旧事件代码在 `IP_EVENT_STA_GOT_IP` 后读取 IP，但连接生命周期、有限重试和退避不足。

## 2. 已迁移和重写

- `wifi_remote`：ESP-Hosted、Wi-Fi Remote、esp_netif、事件注册、STA/AP/APSTA、扫描、IP、状态和停止。
- `network_provisioning`：异步 Queue/task 状态机，最多 5 次重试和退避，只在 GOT_IP 后成功并保存 NVS。
- NVS 改为 `smartscore_net`，键为 `wifi_ssid`、`wifi_pass`、`wifi_valid`；清除配置不重启。
- `ble_provisioning`：P4 NimBLE Host → Hosted VHCI → C5 Controller，自定义 GATT、换行 JSON、20 字节分片、512 字节上限。
- `web_provisioning`：隔离的 `SmartScore_Setup` 页面和五个配网路由；HTML 独立到 `provisioning_page.h`。
- `network_diagnostics`：连接成功后提供 `/api/ping` 和仅含网络状态的 `/api/status`。
- 正式工程/组件命名使用 `smartscore_wt99_p4`、`CONFIG_SMARTSCORE_NETWORK_ONLY` 和 `network_diagnostics`；“test”只保留在独立联调小程序页面和操作文档语义中。

## 3. 没有迁移

屏幕、M5Stack Tab5 BSP、LVGL、音频、MIDI、乐谱解析、实时跟谱、AI 识谱、豆包评分、三块 S3 通信、摄像头、SD 卡业务、上传接口和正式业务 API 均未进入本阶段 `main` 的依赖。

## 4. WT99 板级适配依据

依据如下：

1. `C:\Users\17762\Desktop\SCH_WT99P4C5-S1-1V1.pdf` 第 2 页；
2. 启明云端官方 [WT99P4C5-S1 示例仓库](https://github.com/wireless-tag-com/WT99P4C5-S1) 的 Hosted 配置和日志；
3. `C:\Users\17762\Desktop\WT99P4C5-S1开发板使用指南.pdf`，用于核对版本与接口用途。

P4-C5 使用 SDIO slot 1、4-bit、40 MHz：D0=GPIO14、D1=15、D2=16、D3=17、CLK=18、CMD=19，C5 EN/reset=GPIO54。C5 3.3 V 由 U14 常供电，没有虚构软件电源控制引脚。

使用指南 J14 的 GPIO39～44 与原理图中板载 MicroSD 相符，不是 P4-C5 连接，因此不采用。所有板级差异集中在 `board_wt99`，并在 Hosted 初始化前逐项校验 target、transport、slot、width、clock、pins 和 reset。

## 5. 依赖和构建环境

依赖由 `main/idf_component.yml` 声明，组件管理器生成 `managed_components` 和 `dependencies.lock`；没有复制旧组件目录，也没有手工编辑 lock。

| 依赖 | 版本 |
|---|---|
| ESP-IDF | 5.4.4 |
| espressif/esp_hosted | 2.12.6 |
| espressif/esp_wifi_remote | 1.6.0 |
| espressif/eppp_link | 1.1.5（传递依赖） |
| espressif/esp_serial_slave_link | 1.1.2（传递依赖） |
| espressif/wifi_remote_over_eppp | 0.3.3（传递依赖） |

ESP-IDF 5.1.2 的 `idf.py --list-targets` 不包含 ESP32-P4，故不能构建本目标；最终使用 5.4.4。

## 6. BLE 与 C5 固件能力

- P4 关闭本地 Bluetooth Controller，启用 NimBLE Host、ESP-Hosted NimBLE 和 VHCI。
- Hosted co-processor target 明确为 ESP32-C5。
- 厂商官方 C5 slave 日志显示 ESP-Hosted MCU Slave 2.0.13、SDIO、WLAN、BLE only、HCI over SDIO、capabilities `0xD`，说明官方固件满足目标架构。
- 用户板上当前实际固件尚未通过串口确认；运行时仍以 Hosted 初始化和 NimBLE Controller sync 为准。失败时返回 `ESP_ERR_NOT_SUPPORTED`，不伪造 BLE capability。
- 本次没有烧录或修改 C5。

## 7. 微信小程序处理方式

工作区找到了已有“谱伴”小程序 `D:\qianrushi\smart_score_esp32-main`，旧 ZIP 中也存在小程序文件。最终按用户要求，**迁移已有小程序界面与代码**到 `D:\qianrushi\smartscore_wt99_p4\miniprogram`：首页、TabBar、乐谱、练习、记录、我的、智能设备页面、WXML/WXSS 和图片资源保持原样，只改智能设备页的 BLE/Wi-Fi 通信协议。原目录保持只读。

原首页“连接设备”仍进入 `pages/device/device`。旧的简易 `pages/network-test` 页面已移除，其 UTF-8、20 字节分片、换行拼包和 UUID 规范化逻辑迁入 `utils/ble_protocol.js` 与原设备页。

## 8. 构建结果

执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\idf54.ps1 set-target esp32p4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\idf54.ps1 build
```

结果：成功，生成 `build/smartscore_wt99_p4.bin`。构建输出显示镜像大小 `0xDB5A0`，1 MiB app 分区剩余 `0x24A60`（约 14%）。未执行 flash、erase-flash、C5 烧录或 Git commit。

本机 IDF 安装的 Git 子模块元数据指向已不存在的旧 worktree，但源码完整；辅助脚本通过 `IDF_SKIP_CHECK_SUBMODULES=1` 跳过安装层检查，实际编译和链接成功。

## 9. 当前已知问题

1. 尚未烧录 P4，未取得用户板的首次启动日志，因此 P4-C5 SDIO、实际 C5 固件版本、BLE 广播和 Wi-Fi 连接仍是“代码/构建完成，硬件待验收”。
2. 厂商资料确认官方 C5 固件支持 WLAN + BLE/HCI，但不能据此假定用户板当前固件一定相同。
3. 八项真机验收需用户手动烧录后逐项执行；第一个关键输入是从复位开始的完整串口日志。

## 10. 主要新增和修改文件

```text
smartscore_wt99_p4/
├── CMakeLists.txt
├── README.md
├── sdkconfig.defaults
├── sdkconfig.defaults.esp32p4
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── Kconfig.projbuild
│   └── main.c
├── components/
│   ├── board_wt99/{CMakeLists.txt,Kconfig,include,src}
│   ├── wifi_remote/{CMakeLists.txt,include,src}
│   ├── network_provisioning/{CMakeLists.txt,include,src}
│   ├── ble_provisioning/{CMakeLists.txt,include,src}
│   ├── web_provisioning/{CMakeLists.txt,include,src}
│   └── network_diagnostics/{CMakeLists.txt,include,src}
├── docs/
│   ├── network_migration_report.md
│   ├── ble_provisioning_protocol.md
│   ├── network_test_steps.md
│   ├── c5_firmware_requirement.md
│   ├── wt99_pinmap.md
│   └── superpowers/specs/2026-07-15-wt99-board-network-bringup-design.md
└── tools/idf54.ps1
```

总工程内的“谱伴”小程序：

```text
smartscore_wt99_p4/miniprogram/
├── app.js
├── app.json
├── app.wxss
├── sitemap.json
├── project.config.json
├── README.md
├── docs/test_steps.md
├── assets/
├── pages/
│   ├── index/
│   ├── scores/
│   ├── practice/
│   ├── history/
│   ├── device/
│   └── profile/
└── utils/{api.js,mock.js,util.js,ble_protocol.js,utf8.js}
```
