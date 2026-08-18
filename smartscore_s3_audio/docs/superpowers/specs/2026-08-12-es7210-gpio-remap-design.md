# ES7210 GPIO 映射更新设计

## 目标

将 `smartscore_s3_audio` 中音频 ESP32-S3 U2 的固件引脚更新为 V2 网表对应的 ESP32-S3 GPIO 编号，并同步更正硬件说明。此次修改仅覆盖 U2/ES7210，不新增语音 S3 U4（INMP441）或摄像头 S3 U6（OV3660）的代码。

## 引脚映射

| 功能 | 旧 GPIO | 新 GPIO | S3 方向 |
|---|---:|---:|---|
| ES7210 CCLK/SCL | GPIO5 | GPIO14 | 开漏输出 |
| ES7210 CDATA/SDA | GPIO4 | GPIO21 | 双向开漏 |
| ES7210 MCLK | GPIO9 | GPIO9 | 输出 |
| ES7210 INT | GPIO42 | GPIO10 | 输入 |
| ES7210 SDOUT1/I2S DIN | GPIO12 | GPIO11 | 输入 |
| ES7210 LRCK/WS | GPIO11 | GPIO12 | 输出 |
| ES7210 SCLK/BCLK | GPIO10 | GPIO13 | 输出 |

主控通信继续使用 UART1：GPIO1 TX、GPIO2 RX。UART0 下载和日志继续使用 GPIO43 TX、GPIO44 RX。GPIO0 BOOT、GPIO39 MUSIC_LED 及 EN 复位信号不改变；当前固件未使用 MUSIC_LED、BOOT 和 EN 的 GPIO 宏，因此不新增与本次采集链路无关的初始化代码。

## 实现

在 `main/board_pins.h` 中替换 I²C、I²S 和 ES7210 中断宏的 GPIO 值，并将注释改成 V2 映射描述。I²C 控制器、I²S 控制器、UART 端口、I²C 速率、采样率、位宽、声道数和 MCLK 倍频保持不变，ES7210 初始化和音频识别算法不需要改动。

同步更新 `README.md` 的硬件引脚表和旧网表说明，明确表内数字均为 ESP32-S3 GPIO 编号，不是模组焊盘编号。删除将 `U2.35` 解释为 GPIO42 的过时结论，避免后续接线和维护继续引用旧映射。

## 验证

1. 全文检索旧的 GPIO4、GPIO5、GPIO42 以及旧 I²S 映射说明，确认活动代码和当前硬件文档不再引用旧引脚。
2. 检查 U2 内部的新 GPIO 分配没有重复占用，GPIO1/2 的主控 UART 与 GPIO43/44 的下载日志 UART 保持独立。
3. 执行 ESP-IDF 构建，确认 GPIO 常量、I²C/I²S 初始化和其余固件代码均可编译。
4. 实机验证由硬件侧完成：启动日志应能识别 ES7210 地址 `0x40` 和芯片 ID，并持续收到有效 I²S 数据。固件构建不能替代实机波形与采集验证。

## 不在范围内

- 不修改 U4/INMP441 或 U6/OV3660 固件。
- 不修改 MUSIC UART 协议、DSP 参数、双麦选择逻辑或 ES7210 寄存器配置。
- 不根据模组焊盘编号重新推导 GPIO；本设计直接采用用户提供的 V2 GPIO 映射。
