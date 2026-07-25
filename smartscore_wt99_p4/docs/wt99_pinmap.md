# WT99P4C5-S1 1V1 网络板级配置

## 依据

本配置同时依据：

1. `C:\Users\17762\Desktop\SCH_WT99P4C5-S1-1V1.pdf`，第 2 页 P4-C5 网络连接；
2. 启明云端官方 [WT99P4C5-S1 示例仓库](https://github.com/wireless-tag-com/WT99P4C5-S1) 的 ESP-Hosted 配置和启动日志；
3. `C:\Users\17762\Desktop\WT99P4C5-S1开发板使用指南.pdf`，用于核对开发板版本和接口用途。

## P4-C5 连接

| 信号 | ESP32-P4 GPIO |
|---|---:|
| SDIO D0 | 14 |
| SDIO D1 | 15 |
| SDIO D2 | 16 |
| SDIO D3 | 17 |
| SDIO CLK | 18 |
| SDIO CMD | 19 |
| C5 EN / reset | 54 |

传输参数：SDIO slot 1、4-bit、40 MHz。Hosted 的 reset 配置为 active-high 序列，由 ESP-Hosted 执行 GPIO54 的实际复位脉冲。

C5 的 3.3 V 电源由原理图中的 U14 随开发板 5 V 电源提供，没有独立的 P4 软件电源开关。因此 `board_wt99_network_power_enable()` 只校验配置并记录“常供电”，不会操作虚构 GPIO。

## 资料冲突处理

使用指南 J14 表格出现 GPIO39～44，但原理图表明这组引脚连接板载 MicroSD，不是 P4-C5 SDIO。P4-C5 采用 GPIO14～19 的结论同时得到原理图网络名和厂商官方 ESP-Hosted 配置印证，因此工程不使用 J14 的 GPIO39～44，也没有复制 M5Stack Tab5 的任何 C6/SDIO 引脚。

所有板级常量集中在 `components/board_wt99/include/board_wt99_pins.h`；网络业务组件不直接引用这些 GPIO。`board_wt99` 在启动前校验 C5 target、transport、slot、bus width、clock、全部数据引脚和 reset GPIO，任一不匹配都会阻止 ESP-Hosted 初始化。
