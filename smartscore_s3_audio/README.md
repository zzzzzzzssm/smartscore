# ESP32-S3 + ES7210 + 双 IM68A130 音乐识别调试工程

这是双 IM68A130 + ES7210 的稳定双麦版本。CN1/MIC1 与 CN2/MIC2 同步采集、分别预处理和校准，再按信噪比、削波、峰值、直流异常和连续有效性选择更可靠的一路；YIN 与 FFT 各只运行一次。任一路无信号、持续为零、削波或异常时会自动退化为另一单麦，UART NDJSON 音乐事件链路完全不感知内部选中了哪一路。工程不发送原始 PCM，也不依赖 PSRAM。

当前硬件状态：

- 系统原设计使用两块 IM68A130；
- CN1 与 CN2 均已可用，分别接 ES7210 MIC1 与 MIC2；
- 默认 `MUSIC_USE_SINGLE_MIC_CH1=0`，启用双麦质量选择路径；
- 如需诊断性回退，可将该宏改为 `1`，编译回原 CH1 单麦路径；
- 双麦运行时无需重新编译即可自动回退到任意仍健康的单路。

## 硬件与信号链

两个有效硬件输入均为 **IM68A130 模拟单端输出 MEMS 麦克风模块**。模块由板卡 VDDA 供电，向板卡提供 VDD、GND、OUT；软件不会通过 ESP32-S3 内置 ADC 采集，也不会创建 PDM 接收。

完整信号链：

```text
IM68A130 CN1/CN2 OUT
  -> ES7210 MIC1P/MIC2P 模拟输入（各自的板上交流耦合网络）
  -> ES7210 ADC
  -> ES7210 SDOUT1
  -> 双槽 I2S（启动隔离探测确认两个物理输入的 slot）
  -> ESP32-S3 I2S0 RX / 两路独立预处理与质量选择
  -> YIN 单音检测 + 单次 FFT/HPCP 复音检测
  -> 串口 SINGLE / INTERVAL / CHORD / UNKNOWN / SILENCE
```

ES7210 MICBIAS12 不给模块供电。`esp_codec_dev` 同时选择 MIC1 与 MIC2 并保持标准双槽 I2S；启动 ADC 后，工程把 ES7210 寄存器 `0x4B` 的 `PDN_MICBIAS12` 位设为 1 并回读确认。两路都配置为 21 dB，并回读 `0x43/0x44` 确认输入已启用及增益代码。

## 最新网表连接

本工程按当前 V2 网表使用以下引脚。表内数字全部是 ESP32-S3 GPIO 编号，不是模组焊盘编号：

| 信号 | ESP32-S3 GPIO |
|---|---:|
| ES7210 CDAT/SDA | 21 |
| ES7210 CCLK/SCL | 14 |
| ES7210 MCLK | 9 |
| ES7210 SCLK/BCLK | 13 |
| ES7210 LRCK/WS | 12 |
| ES7210 SDOUT1/I2S DIN | 11 |
| ES7210 INT（仅输入，不启用中断） | 10 |
| 音乐链路 UART1 TX（S3 → 外部主控） | 1 |
| 音乐链路 UART1 RX（外部主控 → S3） | 2 |

GPIO11 是 S3 的音频数据输入，因为 ES7210 通过 SDOUT1 向 S3 输出采集到的音频数据。主控通信使用 UART1 的 GPIO1 TX、GPIO2 RX；GPIO43 TX、GPIO44 RX 保留给 UART0 烧录和日志。所有 UART 均为 3.3 V TTL 电平。

ES7210 AD0、AD1 均接地，7 位地址是 `0x40`。启动时扫描完整 I2C 总线，必须发现 `0x40`，并读取 `0x3D/0x3E/0x3F`。芯片 ID 必须为 `0x72/0x10`；不匹配时音频任务不会启动。

模块接口：

| 接口 | 1 | 2 | 3 | ES7210 输入 |
|---|---|---|---|---|
| CN1 | AGND | VDDA | IM68A130 OUT | MIC1 |
| CN2 | AGND | VDDA | IM68A130 OUT | MIC2 |

最新原理图确认 CN1 的 1/2/3 脚依次为 AGND、VDDA、IM68A130 OUT。CN1 pin 3 经 C17/R17 进入 MIC1P，MIC1N 由 C16/R18 交流参考到 AGND；CN2 使用对应的 MIC2 网络。不要改变板上的交流耦合、电阻匹配或 MIC1P/MIC1N、MIC2P/MIC2N 网络，也不要并联两块模块的 OUT。

## 数字音频格式和通道验证

当前 `esp_codec_dev 1.2.x` 选择 MIC1+MIC2；选择少于三个 MIC 时，ES7210 驱动把寄存器 `0x12` 配成 `0x00`，即标准双槽 I2S，而不是四槽 TDM。数字音频格式仍为 24000 Hz、16 位有效样本、16 位槽、2 槽：

- MCLK = 6.144 MHz（256 × LRCK）
- BCLK = 768 kHz（2 × 16 × LRCK）
- LRCK = 24 kHz
- 名义 MIC1 = 左槽/slot 0
- 名义 MIC2 = 右槽/slot 1

DSP 任务启动前，采集层会短暂分别清除 MIC2、MIC1 的输入使能位，各丢弃一个过渡块后测量两个 slot 的一阶差分活动度，再原样恢复并回读两个增益寄存器。两次隔离结果互补且差异足够明显时锁定实测映射；环境过静或串扰使探测不明确时保留由 ES7210 标准模式确定的名义映射并打印警告。随后 `AudioCaptureTask` 每次从同一 I2S 帧同步拆出 MIC1 和 MIC2，绝不丢弃第二槽。

双麦路径不做未对齐时域平均，也不再执行旧版“两路 FFT 后按 RMS 融合频谱”。两路仅保留独立预处理和环形历史；选中主通道后，原 YIN、FFT、谐波清理、分类与稳定投票各执行一次。

## 算法

MIC1/MIC2 分别转 float、逐帧去均值，并各自维护两级约 50 Hz 一阶高通状态。上电后用约 1 秒安静音频独立估计两路 RMS 噪声底；每路动态噪声门取 `max(MUSIC_MIN_RMS, noise_rms × MUSIC_NOISE_GATE_MULTIPLIER)`，并只在该路低于门限附近时慢速跟踪环境变化。最低 RMS 为 0.0008、噪声倍数为 2.5。任意健康通道存在可信活动就开启分析；两路均失去活动后仍保持 4 个采集块（约 171 ms），避免弱音衰减过早触发静音。

每路质量评分同时考虑相对自身噪声底的 SNR、动态门限、峰值过小、接近满量程、削波率、帧均值直流异常、冲击型峰均比以及连续有效帧数，内部状态为 `INVALID/WEAK/VALID/GOOD/CLIPPED/NOISY`。一路失效而另一路有效时立即回退；两路均有效时，只有挑战通道连续 3 帧领先至少 0.15 且当前通道已保持至少 8 帧才切换，避免来回跳变。两路质量接近时保持当前主通道。

单音使用 2048 点 YIN：差分函数、CMND、首个达标局部最小值搜索和抛物线插值。标准 YIN 的首个达标谷值已经代表基频，不再偏好两倍周期，避免 A4 被错误降为 A3。最大 tau 根据 `sample_rate / minimum_frequency` 动态得到。频率再转换为 MIDI、音名、八度和相对十二平均律的 cents。最近 5 次结果至少 3 次 MIDI 相同才稳定。

复音分支使用 ESP-DSP 只对当前选中的分析环形缓冲执行一次 4096 点 float FFT，采用 Hann 窗和 1024 点 hop；不分配第二路 FFT 幅度谱，也不做跨通道频谱相加。其余局部峰、抛物线插值、谐波归属、双音/和弦判断和最近 5 帧至少 3 帧一致的稳定投票保持原算法。

当前统一使用标准调律 `MUSIC_REFERENCE_A4_HZ=440.0f`。YIN、频率与 MIDI 转换、cents、FFT 候选以及 UART `hello.a4` 都引用同一个宏。

统一分类规则：

- 两路均无可信活动且活动保持结束：`SILENCE`。
- YIN 置信度和单音谐波解释率达标：`SINGLE`；只有两项都超过更严格的 0.90 时，才允许它覆盖一个有效双音候选。
- 若 YIN 置信度至少 0.88，且谐波清理后频谱只剩一个与 YIN 同音级的独立候选，也直接判为 `SINGLE`，避免音色丰富的 C4/B3 被三次谐波误判为双音。
- 两个占主导且能量相对均衡的独立音级稳定出现：`INTERVAL`，例如同时弹 C4、B4 输出 `INTERVAL notes=[C4,B4]`。
- 至少三个独立稳定音级且和弦模板置信度达标：`CHORD`。
- 被选分析通道削波、强噪声、频谱不稳、分支冲突或置信度不足：`UNKNOWN`；另一健康通道存在时会优先切换而不是让异常通道否决结果。

所有阈值都集中在 `main/music_detector_config.h`。正式结果只在结果变化或距上次输出超过 300 ms 时打印；诊断使用 `AUDIO_DIAG`/`DSP_PERF`，正式结果使用 `RESULT`。

每次正式识别成功还会额外输出一行醒目的 `DETECTED_MUSIC`：单音形如 `SINGLE [A4]`，双音形如 `NOTES [C4 + B4]`，三和弦形如 `CHORD C:maj [C4 + E4 + G4]`。该行只显示最终稳定音符，便于从大量诊断信息中直接查找。

为分析候选音，固件每 500 ms 输出一条紧凑的 `SPECTRUM_DEBUG`。例如 `C4@256.9/b44/r1.00/p1.20` 依次表示音名、插值后的实际峰值频率、FFT bin、相对最强候选得分和峰值突出度。只有真实局部峰才进入该列表，且一个物理峰只能出现一次；`harmonic_rejected` 表示本帧有多少个 2～5 次谐波峰已从独立音集合中移除。

工程根目录提供串口自动记录脚本。先退出 `idf.py monitor`，然后运行：

```powershell
.\capture_music_log.ps1 -Port COM3 -Label "C4 only"
```

脚本会实时显示串口，并同时覆盖工程根目录的 `music_capture_latest.log`、归档到 `music_logs/music_capture_日期_时间.log`。弹奏完成后按 `Ctrl+C`；随后可直接让 Codex 读取 `music_capture_latest.log`。串口一次只能被一个程序占用，因此记录脚本运行期间不要同时启动 IDF monitor。

## 增益与削波调试

默认 ES7210 输入增益为 21 dB；18、21、24、27 dB 都在配置头中集中列出。第一版无软件 AGC，运行中不自动改变硬件增益。

- 正常演奏峰值建议约满量程 15%～70%。
- 峰值达到 95%，或一帧中超过 98% 满量程的采样多于 0.1%，按削波处理。
- 削波时先降低输入增益或增加声源距离。
- 连续诊断中峰值长期低于 5% 才考虑提高到下一个增益档，不要为了安静输入直接把增益拉满。
- `AUDIO_DIAG` 的 `noise` 是 CH1 校准噪声底，`gate` 是实际 CH1 动态噪声门。
- `AUDIO_DIAG`/`DUAL_MIC` 分别打印两路 RMS、峰值、削波率、噪声底、门限、质量状态、SNR、当前主通道与切换计数。

## 专用 UART 音乐事件链路

正式协议使用 `UART_NUM_1`、115200 baud、8N1、无流控。最终 MUSIC 接线中，S3 GPIO2 为 RX（P4 → S3），GPIO1 为 TX（S3 → P4）：P4 J6-4 GPIO0/TX 接 S3 H7-8 `MUSIC_RX_MS`（U2模块焊盘38，S3 GPIO2/RX），S3 H7-10 `MUSIC_TX_MS`（U2模块焊盘39，S3 GPIO1/TX）接 P4 J6-6 GPIO1/RX，并连接共地。GPIO43/44 及原控制台继续只承载 `ESP_LOG` 调试日志，JSON 不会混入调试串口。不得把 5 V 串口电平接到 ESP32-S3。GPIO26/GPIO27 不再用于 MUSIC UART，因此不再与屏幕 UPDN/SHLR 冲突。

`MusicDspTask` 只以零等待方式投递结构化结果。稳定分类结果进入长度 16 的优先状态队列，连续 pitch 进入长度 8 的可丢弃队列；`MusicLinkTxTask` 固定在 core 0、优先级 6，独占 JSON 格式化。JSON 再进入长度 16 的发送缓存。S3 GPIO1 保持标准 UART 空闲高电平，收到 P4 的 `poll` 后最多发送 4 帧。任何队列满、格式化截断或 UART 短写只增加心跳中的 `tx_drop`，不会阻塞采集或 DSP。

协议为 NDJSON：每行一个 UTF-8 JSON 对象并以 `\n` 结束。所有消息都有 `v`、`type`、`seq`、`sid`、`ts_ms`，消息类型包括 `hello`、`status`、`pong`、`pitch`、`note_on`、`note_off`、`poly`、`heartbeat`。不发送 PCM，也不输出 NaN/Inf。相同 MIDI 的持续单音只产生一次 `note_on`；换音时先发旧音的 `note_off(reason=changed)` 再发新音；静音、进入复音或 UNKNOWN 持续约 300 ms 分别使用 `silence`、`polyphonic`、`timeout` 关闭活动音符。一个 `note_on` 最多对应一个 `note_off`，其 `duration_ms` 由两个事件时间戳相减得到。相同稳定复音只发送一次 `poly`。

RX 按行接受以下小型控制命令：

```json
{"cmd":"poll"}
{"cmd":"ping"}
{"cmd":"start","sid":12}
{"cmd":"stop"}
{"cmd":"stream_on"}
{"cmd":"stream_off"}
```

默认 `sid=0` 且实时流开启。`stop`/`stream_off` 会先关闭活动音符，之后停止 pitch、note 和 poly；`start` 设置新 sid、清空旧状态并重新开启流。所有上行消息（包括 heartbeat）都由 `poll` 取回，S3 不会自主持续占用 P4 GPIO1。

PC 端需要 `pyserial`：

```powershell
python -m pip install pyserial
python capture_music_link.py --port COM8
python capture_music_link.py --port COM8 --send ping
python capture_music_link.py --port COM8 --send start --sid 12
python capture_music_link.py --port COM8 --send stop
```

脚本逐行执行 `json.loads`，显示各类事件，并在退出时汇总非法 JSON、重复 `note_on`、无配对 `note_off` 与仍未关闭的活动音符。

## 构建、烧录与监视

需要 ESP-IDF 5.3 或更新版本：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

组件管理器会解析 `espressif/esp_codec_dev` 和 `espressif/esp-dsp`。CPU 默认 240 MHz，Performance 优化，不启用 PSRAM。

双麦模式保留两路 PCM 块和 4096 点环形历史，但只保留一份 FFT 工作区与有效频段幅度谱。预处理结果直接写入环形缓冲，不再经过两份 1024 点临时 float 帧；4096 点 Hann 窗只存储对称的一半；FFT 幅度计算只覆盖 0～2000 Hz 的识别频段及一个边界 bin（343 个 bin，而不是全部 2049 个）。这些修改不改变 24 kHz 采样率、1024 点 hop、4096 点 FFT、2048 点 YIN 窗口和稳定投票参数。

ESP-IDF 5.5.4 优化后实际构建统计为 `.bss=105496`、`.data=8520`、DIRAM 剩余 187401 bytes，应用 bin 为 310480 bytes；相比优化前 `.bss` 减少 24912 bytes，FFT 幅度开方次数由每帧 2049 次降为 343 次。DSP 循环中没有新增 `malloc/free`。运行时平均/最大 DSP 帧耗时、队列深度、丢块、缓冲耗尽、deadline miss、栈余量和通道切换数由 `DSP_PERF` 输出，端到端延迟与准确率仍必须以 ESP32-S3 目标板日志和跟谱实测为准。

## 实机测试顺序

1. 安静环境上电，检查隔离槽位探测或明确的回退警告，等待双路 calibration finished，确认两路 RMS、噪声底和噪声门合理。
2. 在两块 IM68A130 附近播放 A4（标准调律 440 Hz），确认输出 MIDI 69/A4 且主通道不会频繁切换。
3. 依次测试 C4≈261.63、E4≈329.63、G4≈392.00、A4=440.00、C5≈523.25 Hz。
4. 同时测试 C4+B4，确认输出 `INTERVAL notes=[C4,B4]`（允许实测八度偶尔偏差，但音级应为 C、B）。
5. 测试 C4-E4-G4、D4-F4-A4 和 G3-B3-D4，确认输出相应大/小三和弦。
6. 拍手和人声应为 `UNKNOWN`，而不是强行输出音符。
7. 检查 `clip`；若持续削波，降低增益或增加声源距离。
8. 检查 `DSP_PERF` 的一次 FFT、队列 dropped、deadline miss 和总处理时间。
9. 分别遮挡或断开 CN1、CN2，确认自动选择另一健康通道且 UART JSON 格式不变。

上电后优先检查：`microphone mode: DUAL quality-selected`、I2C 地址 `0x40`、ES7210 `0x7210` ID/版本、`MIC1 and MIC2 enabled`、两路增益回读、格式/slot/时钟回读、隔离槽位探测、MICBIAS12 已掉电、双路 noise/gate，以及 `DSP_PERF` 中只有一次 FFT。任一硬件身份、输入恢复或关键寄存器回读失败时，不会启动识别任务。

## 能力边界

- 单音输出频率、MIDI、音名、八度、cents 和置信度。
- 双音输出 `INTERVAL`、两个音名/MIDI 与音级；音级判断通常比具体八度更可靠。
- 三音和弦优先识别常见大三和弦、小三和弦；输出音级集合，不保证恢复每个音的具体八度。
- 一块麦克风可以完成单音、双音和常见大/小三和弦检测；复音识别不依赖多个麦克风才能运行。
- 双麦克风原本用于提高信噪比、稳定性和频谱覆盖，不会自动分离混合声源。
- 双麦选择阈值、两路实际增益差和不同安装位置仍需在目标机上调参；本策略不会做波束形成或声源分离。
- 不实现任意四音以上钢琴复音转 MIDI，也不专门处理延音踏板造成的长时间音符重叠。
- 真实钢琴混响、多人说话和强背景音乐仍需实机调参。
- 当前只完成软件构建验证；默认 slot 映射、噪声阈值和音乐置信度仍必须通过目标 PCB 与 CH1 实机验证。

优先实机调整顺序：先确认 CH1 slot 映射和削波，再调 ES7210 增益、CH1 噪声门倍数/最低 RMS、YIN 阈值与置信度、单音谐波解释率、活动音级阈值、和弦置信度，最后调整稳定投票和重复输出间隔。
