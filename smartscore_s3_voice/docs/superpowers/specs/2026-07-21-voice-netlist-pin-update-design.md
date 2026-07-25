# 语音识别 S3 最新网表引脚更新

仅修改 `smartscore_s3_voice` 的 INMP441 I2S 引脚，不改变采样格式、识别模型或任务结构。

依据 `Netlist_PCB1_2026-07-21.tel`：语音识别处理器为 U4，`INMP441_SCK -> U4.18 -> GPIO10`、`INMP441_WS -> U4.19 -> GPIO11`、`INMP441_SD -> U4.17 -> GPIO9`。

因此 BCLK/WS/DIN 从 GPIO1/GPIO2/GPIO42 更新为 GPIO10/GPIO11/GPIO9，并同步 README。按用户要求不执行编译、烧录或 Git 提交。
