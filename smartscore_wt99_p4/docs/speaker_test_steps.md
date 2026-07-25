# 小程序扬声器功能测试步骤

## 构建、烧录与监视

工程目标为 ESP32-P4，使用 ESP-IDF 5.4.4：

```powershell
& 'D:\Espressif\frameworks\esp-idf-v5.4\export.ps1'
idf.py build
idf.py -p COM12 flash monitor
```

本次实现只执行了 `idf.py build`，没有自动烧录、擦除 Flash、烧录 C5 或创建 Git 提交。若端口不是 COM12，请替换为实际 P4 串口。退出监视器使用 `Ctrl+]`。

## 配网和进入声音工具

1. 给 WT99 接稳定 5V 电源，扬声器两根线插入 J7；任意一根都不要接 GND。
2. 在小程序“设备”页扫描并连接 `SmartScore-WT99-XXXX`。
3. 通过 BLE 扫描 Wi-Fi、选择 SSID、输入密码并等待设备返回 IP。
4. 手机保持在与 WT99 相同的局域网；设备页保存的地址应为 `http://<设备IP>`。
5. 返回首页，在六宫格中分别进入“节拍器与校准音”或“音乐播放模式”。原“开始练习”继续用于正式评分练习，练习页不再混放扬声器工具。

BLE 不承载音频控制。Wi-Fi 未连接或 IP 不可访问时，声音工具会显示离线，不自动回退到 BLE 控制或旧 SoftAP。

微信开发者工具调试局域网 HTTP 时，需要关闭“校验合法域名、web-view（业务域名）、TLS 版本以及 HTTPS 证书”。正式发布的小程序对请求域名和 HTTPS 有平台限制，局域网裸 IP HTTP 需结合最终发布方式另行处理。

## 功能操作

### 音量、静音和停止

1. 初次试听先把页面音量保持在约 19（对应设备音量 15）。
2. 拖动滑块，松手后才发送请求；页面范围为 0～100，按比例映射到设备 0～80。
3. 点击静音/取消静音；功放只在实际输出且未静音时启用。
4. 点击“停止播放”。重复停止应保持无声，旧播放请求不得再次启动。

### 校准音

1. 选择持续、10 秒或 30 秒。
2. 依次试听 C4、E4、G4、A4、C5。
3. 播放中选择另一音符，新音符应安全替换旧音符。
4. A4 应为 440.00 Hz；停止后 GPIO53 关闭。

### 节拍器

1. 可直接选择 60、80、90、100、120、140 BPM，也可在输入框手动填写 30～240 的整数；不再使用 BPM 滑块。
2. 选择 2/4、3/4、4/4 或 6/8。
3. 点击开始；第一拍为较高、较长的 1400 Hz 强拍，普通拍为 900 Hz。
4. 暂停后应无声；再次开始可继续使用当前 BPM/拍号；停止后拍序归零。
5. 节拍器运行时打开/刷新其他页面，网络请求不应被阻塞。

### SD WAV

1. 将 FAT/FAT32 MicroSD 插入开发板，把文件放入 `/wav` 目录。
2. 进入首页“音乐播放模式”，点击刷新；文件列表每页最多 10 首，并可使用上一页、下一页。
3. 输入中文或英文文件名关键字并搜索，应只返回匹配结果；搜索词最长 64 个 UTF-8 字节。
4. 点击播放、暂停/继续、停止。
5. 播放另一个文件应替换当前文件，长音频播放期间内存不应随文件大小增长。
6. 不支持的格式会停止并在 `/api/audio/status` 的 `last_error` 中显示 `ESP_ERR_NOT_SUPPORTED`；无 SD 卡或无目录时页面显示明确状态，不会重启。

## 预期关键日志

```text
I (...) BOARD_AUDIO: ES8311 ready: I2C1 SDA=7 SCL=8, I2S1 24kHz mono, PA GPIO=53 off, volume=15%
I (...) BOARD_SD: SD mounted on slot 0, D0..D3=39,40,41,42 CLK=43 CMD=44, capacity=...
I (...) SPEAKER: speaker service ready; mono differential output, initial volume=15%
I (...) NET: remote capabilities: WIFI=YES BLE=YES
I (...) DEVICE_API: device and speaker API ready at http://<IP>
I (...) SPEAKER: tone started: 440.00 Hz, ... ms
I (...) SPEAKER: metronome output session started; PA remains enabled
I (...) SPEAKER: metronome started: 90 BPM, 4/4
I (...) SPEAKER: WAV stream started: <文件名>, 16000 Hz
```

没有插卡时允许出现 `BOARD_SD: SD card not mounted`，但 ES8311、网络、校准音和节拍器仍应可用。

## HTTP 接口

- `GET /api/status`、`GET /api/audio/status`
- `POST /api/audio/volume`、`/api/audio/mute`、`/api/audio/tone`、`/api/audio/stop`
- `POST /api/metronome/start`、`/api/metronome/pause`、`/api/metronome/stop`
- `GET /api/audio/files?page=1&page_size=10&search=<URL 编码关键字>`
- `POST /api/audio/file/play`、`/api/audio/file/pause`、`/api/audio/file/stop`

所有播放 POST 只负责校验并入队，正常返回 `202 Accepted`，不会在 HTTP 处理函数中等待音频播放完成。
