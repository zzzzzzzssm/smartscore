# USB MIDI 输入与评分系统迁移设计

## 目标与边界

在现有 ESP32-P4 固件中增量接入 USB MIDI 输入、标准谱上传、演奏记录和异步评分。现有 ESP32-C5、ESP-Hosted、Wi-Fi、BLE 配网、网页配网、扬声器和小程序 UI 保持原逻辑，不移植 USB 测试工程的大型网页记录器，也不实现 S3 音频输入或延音踏板。

第一阶段只接受 USB-MIDI 1.0 Note On、Note Off，以及 velocity 为 0 的 Note On（按 Note Off 处理）。评分只使用 MIDI 完整音符，不使用音频帧恢复、八度纠正或音频稳定帧合并。

## 方案选择

采用独立组件流水线：

`USB Host -> 轻量回调解析/事件队列 -> MIDI 处理任务 -> performance_recorder -> scoring_service -> score_engine -> device_api`

不直接移植 M5 的整体 `app_state`，避免引入音频采集状态和第二套评分路径；不把实时 MIDI 事件接入全局事件总线，避免扩大第一阶段改动范围。

## 组件职责

### usb_midi

- 移植参考 USB Host 的枚举、MIDI Streaming 接口识别、IN 端点申领、异步传输、热插拔和端点恢复。
- 传输回调只把 USB-MIDI 四字节事件解析为带单调时间戳的 Note On/Off，并以非阻塞方式写入 FreeRTOS 队列。
- 独立任务消费队列并调用演奏记录器；队列溢出设置明确错误状态。
- 维护连接状态、VID、PID、product 和错误信息。
- 仅允许一个活动 USB MIDI 设备。

### performance_recorder

- 维护最多 64 项活动音符，键为 channel + midi，不使用 `active[16][128]`。
- Note On 建立活动音符；重复 Note On 先关闭旧音符再建立新音符；Note Off 完成音符。
- 统一完整音符字段为 `midi/start_ms/duration_ms/velocity/channel/source`，当前 source 为 `USB_MIDI`。
- 完整音符缓冲最多 1024 项，在每次开始记录时动态分配，优先 `MALLOC_CAP_SPIRAM`，无 PSRAM时回退内部 8-bit 堆。
- 缓冲满、分配失败、队列丢事件或 USB 断开均停止本次记录并保存明确错误；绝不循环覆盖。

### score_data 与 scoring_service

- `score_json_parser` 接受原 M5 JSON：顶层可含 `title`、`bpm`，必须含 `notes` 数组；每项包含 `midi/start/duration`，时间单位保持秒以兼容旧小程序。
- 标准谱动态保存最多 1024 个有效音符，按 start、midi 排序。超过上限直接失败，不截断。
- 预留 `input_source` 和评分 profile；当前只允许 `usb_midi` 和 `midi_strict`。
- `/api/stop` 关闭活动音符并把不可变快照交给独立评分任务，等待本地评分完成后在同一响应中返回完整结果；屏幕实时跟谱属于另一条后续路径。
- 评分任务异步生成并保存最近结果；`/api/result` 返回 idle/scoring/ready/error 状态及完整结果。

### score_engine

- 以原 M5 整体对齐和评分逻辑为基础，只保留一套音符级 MIDI 路径。
- 保留 start_offset、tempo_scale、漏音、多音、错音、节奏、完整度计算和原综合分权重。
- 删除音频 frame 恢复、八度纠正、稳定帧合并、频率/置信度修正等音频专用路径。
- 音高以整数 MIDI 半音差严格判断；相同 MIDI 才计为音高正确。
- 结果保留指定汇总字段，并为每条 detail 输出 MIDI、起止时间、时值、音高误差、力度和结果。

## API 语义

- `POST /api/score`：上传并原子替换标准谱。失败时保留旧谱。请求体上限按 1024 音符的 JSON 规模设置，正文动态分配。
- `POST /api/start`：要求标准谱已加载、USB MIDI 已连接且没有正在记录或评分；可选 `input_source` 和 `profile`，缺省分别为 `usb_midi`、`midi_strict`。
- `POST /api/stop`：结束记录并排队到独立评分任务，等待最多 10 秒后直接返回完整评分 JSON；评分并不在 HTTP 任务栈中执行。
- `GET /api/result`：保留为最近结果重复读取和超时恢复接口；评分中返回状态，完成后返回完整评分 JSON。
- `GET /api/status`：在现有字段基础上增加 `usb_midi_connected/vid/pid/product/active_source` 以及练习状态，不删除或改名现有字段。

所有错误响应保留 `ok/error/message`。USB 练习中拔出时状态为 error，错误码为 `usb_midi_disconnected`，不自动选择其他输入。

## 初始化与隔离

在现有 `app_main` 中只增加评分状态、记录器和 USB MIDI 的初始化调用，不改变已有函数的相对顺序、参数或错误策略。USB/评分初始化失败只记录模块错误并允许原网络流程继续运行。

HTTP 服务仍由现有网络状态机启动和停止；仅在现有路由表追加四个评分路由并提高路由数量。现有音频和网络路由实现保持不变。

## 分阶段验证

1. USB Host 接入和连接状态：编译。
2. Note On/Off 到完整音符：编译，并做解析/记录器主机侧单元检查（可行时）。
3. 标准谱上传：编译。
4. 开始/停止记录与异步任务：编译。
5. 单一 MIDI 评分引擎：编译。
6. `/api/result` 完整结果：编译。
7. 全量 clean build 或等价完整构建，检查镜像大小和未修改边界。

不自动烧录、不擦除 Flash、不修改 C5 固件、不创建 Git 提交。

## 验收标准

- 所有七个阶段均能通过 ESP-IDF 5.4 构建，或明确记录无法由代码解决的环境阻塞。
- USB 回调中不执行评分、锁等待、堆分配或记录器业务。
- 活动音符不超过 64，完整音符不超过 1024，溢出无静默覆盖。
- 仅存在一个参与构建的评分引擎。
- 现有网络、BLE、网页配网、扬声器代码文件不被修改；`main` 与 `device_api` 只包含增量挂接。
