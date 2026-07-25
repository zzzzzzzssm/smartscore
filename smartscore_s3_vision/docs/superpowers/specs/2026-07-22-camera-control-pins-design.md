# 摄像头 PWDN/RESET 引脚接入设计

## 背景

新板上的 ESP32-S3 已能正常启动，Flash、8 MB PSRAM、UART 和 SDMMC 均工作正常，但 `esp_camera_init()` 返回 `ESP_ERR_NOT_SUPPORTED`。网表显示摄像头的 `CAM_PWDN` 连接到 U6 GPIO14，`CAM_CAMEN` 连接到 U6 GPIO41；当前固件却把 `CAM_PIN_PWDN` 和 `CAM_PIN_RESET` 都配置为 `-1`，因此摄像头驱动不会主动执行掉电和复位时序。

## 方案比较

1. **直接配置驱动控制脚（采用）**：将 `CAM_PIN_PWDN` 设为 GPIO14，将 `CAM_PIN_RESET` 设为 GPIO41，复用 `esp_camera` 已有的标准时序。改动最小，控制逻辑集中在驱动中。
2. 在 `CameraDriver::init()` 中手动配置和翻转 GPIO，再继续向 `esp_camera` 传入 `-1`。该方案重复组件已有逻辑，后续维护时容易出现时序不一致。
3. 继续依赖板上的下拉和上拉电阻。当前启动日志已经证明这种方式未能可靠完成传感器探测，因此不采用。

## 设计

只修改 `main/board_pins.hpp`：

- `CAM_PIN_PWDN`：`GPIO_NUM_14`
- `CAM_PIN_RESET`：`GPIO_NUM_41`

`CameraDriver::init()` 已把这两个常量传给 `camera_config_t`，无需增加业务逻辑。初始化时，`esp_camera` 会先把 PWDN 拉高再拉低，并把 RESET 拉低再拉高，然后通过 GPIO4/GPIO5 的 SCCB 总线探测传感器。

不修改摄像头数据总线、XCLK、SDMMC、模型、状态机或图像处理参数。

## 验证

1. 执行 ESP-IDF 全量构建，确保 GPIO 类型和组件接口编译通过。
2. 烧录到新板并观察启动日志；成功标准是出现受支持摄像头的 PID/型号和 `camera ready`，且不再出现 `Detected camera not supported`。
3. 若仍探测失败，则保留本次引脚修正，继续检查 FPC 方向、1.2 V/2.8 V 电源、GPIO14/41 电平和 20 MHz XCLK；这些属于硬件诊断，不扩大本次代码修改范围。
