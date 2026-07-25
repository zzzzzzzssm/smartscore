# WT99 ESP32-P4 PSRAM 启动恢复设计

## 背景

ESP32-P4 固件已使用 ESP-IDF 5.5.3 成功编译和烧录，但真机在启动
ESP-Hosted SDIO 传输时因 DMA 内存池分配失败而触发断言：

```text
E (...) HS_MP: mempool create failed: no mem
assert failed: sdio_mempool_create sdio_drv.c:249 (buf_mp_g)
```

启动日志没有 PSRAM 初始化或容量信息。工程的 `sdkconfig.defaults` 和迁入的
SmartMusic 原始工程都要求启用 ESP32-P4 HEX PSRAM、80 MHz，但当前生成的
`sdkconfig` 明确保存了 `# CONFIG_SPIRAM is not set`。已有 `sdkconfig` 的显式
关闭值覆盖了 defaults，导致固件只使用片内 RAM。

## 目标

- 恢复 WT99P4C5-S1 板载 PSRAM 初始化和堆分配能力。
- 保留现有 ESP-Hosted、Wi-Fi Remote、BLE、屏幕、触摸、扬声器、USB MIDI、
  小程序协议和评分业务逻辑。
- 只修改内存配置，不修改任何组件源代码。
- 按用户要求，本轮不执行编译、烧录或串口监视。

## 方案

采用最小配置修复：在当前 `sdkconfig` 中启用 `CONFIG_SPIRAM`，使用 P4 支持且
原始屏幕工程已采用的 `CONFIG_SPIRAM_MODE_HEX` 和
`CONFIG_SPIRAM_SPEED_80M`。同时启用启动初始化和通用堆分配，使 LVGL、评分
缓冲及其他使用 `MALLOC_CAP_SPIRAM` 的现有代码能够取得外部内存。

不采用以下替代方案：

- 不重新生成整个 `sdkconfig`，避免覆盖已验证的网络、音频和板级参数。
- 不降低 ESP-Hosted SDIO 队列大小，因为这只能缓解片内 RAM 压力，无法满足
  1024×600 显示缓冲和既有 PSRAM 优先分配策略。
- 不修改 `managed_components` 中的 ESP-Hosted 内存池实现。

## 配置边界

允许变化的配置仅限 ESP32-P4 PSRAM 主开关、HEX/80 MHz模式，以及 IDF 在该
主开关启用后生成的直接依赖项。`sdkconfig.defaults` 中现有 PSRAM声明保持为
持久默认值。

以下内容不得改变：

- ESP32-P4 与 ESP32-C5 的 SDIO GPIO、总线宽度、时钟和复位极性；
- Wi-Fi Remote、NimBLE VHCI、配网服务和小程序协议；
- 显示、触摸、音频、SD 卡和 USB MIDI 引脚；
- 分区表、应用入口及业务组件源代码。

## 验证

本轮仅进行静态验证：

1. 检查 `sdkconfig` 中 PSRAM 主开关、HEX 模式、80 MHz、启动初始化和堆分配
   配置均已启用。
2. 检查 Git 差异只包含预期的 PSRAM 配置和本设计文档。
3. 不运行 `idf.py build`、`idf.py flash` 或 `idf.py monitor`。

后续由用户编译并烧录后，真机验收标准为：

- 启动日志出现 PSRAM 探测、初始化和可用容量信息；
- `sdio_mempool_create` 不再因无内存触发断言；
- 系统继续进入 ESP-Hosted 连接及应用初始化流程。

