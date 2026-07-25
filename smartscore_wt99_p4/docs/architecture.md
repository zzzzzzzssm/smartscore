# 当前 P4 网络与扬声器架构

ESP32-P4 是应用主控，板载 ESP32-C5 继续运行 ESP-Hosted Slave 2.12.6。P4 通过 SDIO slot 1 控制 C5 的 Wi-Fi，并通过 Hosted VHCI 使用 C5 蓝牙控制器。

连接流程固定为：

1. 小程序通过 BLE 发现 `SmartScore-WT99-XXXX`；
2. BLE 只负责 Wi-Fi 扫描、SSID/密码下发和 IP 通知；
3. 手机与 WT99 进入同一局域网；
4. 音量、校准音、节拍器和 SD WAV 均通过 P4 上的局域网 HTTP API 控制。

音频调用链：

```text
微信小程序 audio 页面
        │ HTTP/JSON
        ▼
device_api（非阻塞命令接口）
        │ FreeRTOS queue
        ▼
speaker_service（唯一主播放源）
        ├── 独立 metronome task
        ├── 实时正弦生成
        └── WAV 分块双缓冲
        ▼
board_audio：ES8311 + I2S1 + NS4150B GPIO53
```

新的主播放请求会清理尚未执行的旧播放命令并替换当前音源；紧急停止会先清空播放队列，避免旧命令在停止后重新发声。音量与静音也通过同一音频队列执行。网页配网和旧测试工程 SoftAP 没有迁入本工程。

当前采用单一 8 MiB factory 应用分区，保留原应用偏移 `0x10000`。这是现阶段最少引入启动状态和 OTA 回滚变量的方案；正式 OTA 需求确定后再调整分区表。
