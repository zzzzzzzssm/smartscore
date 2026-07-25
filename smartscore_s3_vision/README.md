# SmartScore ESP32-S3 Vision

动作编号：

- `1`：食指向右，上一页
- `2`：食指向左，下一页
- `3`：OK 手势，请求 P4 开始练习
- `4`：五指张开，请求 P4 暂停练习
- `5`：竖大拇指，查看评分

OV3660 引脚：XCLK=15、SIOD=4、SIOC=5、D0..D7=11/9/8/10/12/18/17/16、VSYNC=6、HREF=7、PCLK=13。PWDN 和 RESET 未连接。

microSD 使用 SDMMC 1-bit：CMD=38、CLK=39、D0=40，挂载点为 `/sdcard`。

动作 1/2 在 Hand Detect 检出控制区内且置信度至少 50% 的单手后，由此前实机验证稳定的 MobileNetV2-0.5 ESP-DL INT16 模型判断左右方向。左右统一使用 87% 置信度和 8% 第一二名分差，一次合格推理立即触发。官方分类 Top-3 中置信度达到 25% 的 OK、五指张开或竖大拇指会阻止方向模型抢判；动作 3/4/5 同样保持单次合格推理触发。模型嵌入 Flash，不从 SD 卡加载。

照片保存在 `/sdcard/score/SNNNN/`，记录期间每 3000 ms 写入 `INNNNNN.JPG`，会话摘要为 `SESSION.TXT`。

识别成功后会通过独立 UART1（115200、8N1）向 WT99 P4 发送命令。S3 GPIO1/TX 经 H7-17 `CAM_TX_MS` 接 P4 J6-27 GPIO48/RX；P4 J6-25 GPIO47/TX 经 H7-15 `CAM_RX_MS` 接 S3 GPIO2/RX，并必须共地。P4 返回 `OK` 或 `IGNORED` 反馈页面动作，并主动同步 `PLAYING`、`PAUSED`、`FINISHED` 演奏状态。SD 记录由 P4 的真实状态控制：`PLAYING` 新建或继续当前会话，`PAUSED` 暂停拍照但保留同一会话，`FINISHED` 等待后台写完后关闭会话。因此触屏、语音和手势启动的练习都会使用同一套记录逻辑。
