# 摄像头 S3 V2 GPIO 重映射设计

## 目标

将摄像头 ESP32-S3 U6 固件中的板级 GPIO 定义更新为当前 V2 网表。改动覆盖 OV3660 控制、时钟、同步和 DVP 数据总线，并补齐摄像头板的公共 UART、BOOT、TF 卡检测及测试 LED 定义。

## 方案

所有引脚继续集中定义在 `main/board_pins.hpp`，由现有摄像头、SDMMC 和 UART 驱动引用，不在各驱动中硬编码 GPIO。

OV3660 使用以下新映射：

- SCCB：SDA=GPIO6，SCL=GPIO7。
- 控制及时钟：XCLK=GPIO9，RESET_N=GPIO15，PWDN=GPIO17。
- 同步：VSYNC=GPIO16，HREF=GPIO18，PCLK=GPIO12。
- DVP：D0..D7=GPIO14/47/48/21/13/11/10/8。

TF 卡继续使用 SDMMC 1-bit：CMD=GPIO38、CLK=GPIO39、DAT0=GPIO40，并补充 SD_CD=GPIO41。摄像头测试 LED 定义为 GPIO42。主控 UART1 保持 TX=GPIO1、RX=GPIO2；补充 UART0 下载/日志 TX=GPIO43、RX=GPIO44 和 BOOT=GPIO0。

GPIO41 和 GPIO42 本次只增加板级常量，不新增插卡检测或 LED 控制行为。现有相机初始化流程会直接使用更新后的常量，不修改采集参数、当前配置的 XCLK 频率、UART 协议、SD 写入逻辑、AI 模型或业务状态机。

## 文档与兼容性

同步更新 `README.md` 中的 OV3660 和摄像头板辅助接口说明，避免旧引脚信息误导调试。保留现有常量名称，使摄像头驱动、SD 存储和 P4 通信调用点无需改动。

## 验证

1. 检查板级常量与 V2 映射逐项一致，摄像头 S3 内没有 GPIO 重复占用。
2. 扫描源代码和 README，确认不存在仍在生效的旧摄像头引脚映射。
3. 执行 ESP-IDF 构建，验证 GPIO 类型和现有驱动接口编译通过。

硬件烧录与实际 OV3660、TF 卡、UART 联调不在本次本地代码验证范围内。
