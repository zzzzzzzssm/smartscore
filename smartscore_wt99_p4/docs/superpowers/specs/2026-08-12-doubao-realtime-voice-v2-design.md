# 豆包端到端实时语音助手 V2 设计

日期：2026-08-12  
状态：用户已确认  
工程：

- P4：`D:\qianrushi\smartscore_wt99_p4_temp\smartscore_wt99_p4`
- 语音 S3：`D:\qianrushi\smartscore_s3_voice_temp`

## 1. 目标和边界

从零实现独立的实时语音助手 V2，严格使用豆包端到端实时语音全双工 V3 接口。保留旧
`voice_assistant.c/.h` 供回滚，但默认构建和运行路径只启用 V2；旧助手不初始化、不创建任务、
不建立 WebSocket，也不分配运行时内存。

V2 只能使用旧助手停用后释放的内存，并复用现有 speaker 服务已经分配的 256 KiB PSRAM
PCM ring。不得缩减、挪用或改变 UI、MIDI、评分、识谱、练习建议、Wi-Fi/BLE、HTTP、摄像头、
C5、板间通信等模块的内存配置。

本次不改变：

- S3 本地唤醒词、命令词、命令 ID、识别优先级和原有行为；
- AI 识谱与 AI 练习建议的模型、鉴权、接口和业务逻辑；
- UI、评分、MIDI、跟谱、小程序、Wi-Fi/BLE 和其它业务；
- 24 kHz、16 bit、单声道的豆包输出播放格式；
- 现有 P4 speaker 256 KiB PSRAM ring 的容量。

用户明确要求本轮只修改代码，不编译、不烧录、不执行硬件测试。

## 2. 选定方案

采用“构建期后端选择、V2 默认启用”方案：

- 新增 `voice_assistant_v2.c/.h`；
- 新增 `voice_v2_protocol.c/.h`，负责协议事件生成、完整消息解析和增量 Base64 解码；
- `main` 只做必要的 V2 初始化和 S3 事件接入；
- 旧 V1 文件继续保留，但不与 V2 同时初始化；
- 不使用同时运行两个助手的运行时切换，避免重复任务、重复 WebSocket 和重复内存占用；
- 不在旧 V1 上继续修补，避免继承旧的超时重试、双 ring 和 staging 状态。

## 3. 官方协议约束

WebSocket 地址：

`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`

音频格式：

- 输入：PCM signed 16-bit little-endian，16 kHz，单声道；
- 输出：PCM signed 16-bit little-endian，24 kHz，单声道。

V2 只使用官方 V3 事件语义，包括：

- 客户端：`session.create`、`input_audio_buffer.append`、
  `input_audio_buffer.commit`、`response.cancel`、`session.close`；
- 服务端：`session.created`、ASR 事件、`input_audio_buffer.committed`、
  `response.output_text.*`、`response.output_audio.started`、
  `response.output_audio.delta`、`response.output_audio.done`、
  `response.done`、`session.closed`、`error`。

`input_audio_buffer.committed` 仅作为输入提交确认，不改变正在进行的 SPEAKING 状态。
`response.output_audio.done` 表示模型音频生成结束，但只有 speaker ring 和 I2S 尾音真正排空后，
本轮才能结束。`response.done` 只记录里程碑，不允许清空尚未播放的 PCM。

## 4. 所有权与并发模型

只有单一 `voice_v2_task` 拥有以下状态：

- WebSocket 业务发送；
- cloud session 创建、提交、取消和关闭；
- V2 会话状态；
- 当前 session ID、里程碑和统计；
- 输入 PCM 的发送游标；
- 完整服务端业务消息的消费与协议解析。

其它执行上下文只能投递事件：

- S3 UART 音频回调：校验、复制一个已接受 PCM 块并入队；
- S3 控制事件：投递 WAKE、LOCAL_COMMAND、AI_BEGIN、SPEECH_END；
- WebSocket 回调：按 `payload_len`、`payload_offset`、`data_len`、`op_code`、`fin`
  重组完整消息并将消息槽入队；
- speaker task：只消费现有 speaker ring 并通过现有 I2S/ES8311 播放。

WebSocket 回调禁止调用 speaker、关闭 session、使用 `portMAX_DELAY`、长期持有 mutex 或输出高频日志。
本地命令的取消也必须投递给 owner task，不能跨任务直接调用 WebSocket send/close。

## 5. 会话状态机

V2 仅使用下列状态：

1. `IDLE`：没有用户会话，warm WebSocket 可保持连接；
2. `CANDIDATE`：收到 WAKE，立即创建候选 cloud session，实时上传 S3 PCM，同时等待本地命令判断；
3. `LISTENING`：AI 路由已确认，继续上传同一会话的新 PCM；
4. `WAIT_RESPONSE`：已发送或已确认输入 commit，等待模型输出；
5. `SPEAKING`：收到 `response.output_audio.started` 或首块有效音频，持续边收边播；
6. `DRAINING`：收到 `response.output_audio.done`，等待所有已接收 PCM 播放完；
7. `CLOSING`：owner 串行发送 `session.close` 并等待结束，然后回到 `IDLE`。

状态要求：

- WAKE 立即进入 CANDIDATE，不等待旧的单轮大缓存积满；
- CANDIDATE 最长约 2.8 秒；若没有本地命令且检测到有效语音，则默认走 AI；
- LOCAL_COMMAND 始终优先，继续执行原本地行为，并由 owner 取消/关闭候选 cloud session、丢弃候选输出；
- AI_BEGIN 复用同一个候选 session，不重新上传历史 PCM；
- SPEECH_END 由 owner 发送一次 `input_audio_buffer.commit`；
- 正常响应期间不设置会主动 cancel/close/recreate/replay 的首 PCM 超时；
- WebSocket 断线、官方 error、异常 session close 或明确网络错误才作为会话失败；
- 上游长间隔仅记录 `UPSTREAM_AUDIO_GAP`，不得主动终止正常回答。

## 6. 输入 PCM 恰好一次

现有 S3→P4 音频帧序号、ACK/NACK 和 CRC/长度检查继续使用：

- P4 只有在一个 PCM 块成功复制到 V2 PSRAM 输入块池后才向 S3 确认接受；
- 已确认的 UART 重复序号不再次投递给 V2；
- owner 每个输入块只执行一次 WebSocket append，发送成功后立即释放块；
- WebSocket 发送失败使当前 session 失败，不把同一用户录音重新排队上传；
- AI_BEGIN 不回放 CANDIDATE 历史音频，因为这些帧已经实时上传；
- 输入块池只吸收短时网络/调度抖动，不作为整句录音缓存，不设置 256000 字节单轮容量。

S3 如需新增 SPEECH_END，必须作为与 PCM 同一发送队列中的有序控制标记：最后一帧 PCM 得到协议层
接受后再发送 SPEECH_END。不得改写 S3 的唤醒、MultiNet、本地命令或其它识别状态。

## 7. 输出 PCM 单 ring 流水线

唯一输出链路：

`WebSocket完整消息 → V2协议解析/Base64增量解码 → 现有speaker 256KiB PSRAM ring → speaker task → I2S → ES8311`

明确禁止：

- voice 专用 TTS ring；
- 完整回答 staging；
- 第二个 64 KiB/128 KiB/256 KiB PCM ring；
- 在 WebSocket 回调中等待 speaker 空间或 I2S 播放。

完整业务消息位于 V2 的 PSRAM WebSocket 消息槽中。owner 对一条 audio delta 分块解码，使用小型
scratch 将 PCM 非阻塞写入现有 speaker ring；ring 暂满时保留当前消息和解码游标，让出调度，稍后
继续，不复制到第二个 PCM 缓存。

候选阶段允许服务端提前生成音频，但不得抢占本地命令：V2 使用 speaker 服务的 hold/release 控制，
候选 PCM 仍直接进入同一个 speaker ring，speaker task 暂停消费；AI 路由确认后 release，命中本地命令
则 abort 并丢弃。预缓冲目标约 24–32 KiB，发生 underrun 后恢复阈值约 16–24 KiB。rebuffer 只暂停
I2S 消费，绝不阻止 producer 写入；达到阈值自动恢复。

## 8. WebSocket 分片和消息内存

V2 为 WebSocket 完整消息使用有上限的 PSRAM 槽池，容量来自旧助手释放的预算。回调处理规则：

- 以 `payload_offset == 0` 开始一个消息；
- 以 `payload_len` 校验声明的完整长度；
- 每个事件将 `data_len` 字节复制到正确 offset；
- 校验 op code、边界、重叠/缺口和 `fin`；
- 只有完整消息才交给 owner；
- 槽池不足或消息越界时返回明确错误，绝不覆盖相邻内存；
- PING/PONG 不进入业务 watchdog；有效业务事件更新时间戳，但时间戳只用于诊断。

协议解析避免用 cJSON 复制超大 Base64 字符串到内部 DRAM。解析器只定位顶层事件字段和 audio delta
字符串区间，随后增量解码到小 scratch。

## 9. 音频焦点和原功能保护

从 WAKE/CANDIDATE 开始通过现有 speaker service 获取 AI 音频焦点，暂停可能争用扬声器的节拍器、
校准音、WAV/提示音；退出 V2 后按进入前的原状态恢复。不得重建扬声器驱动，不得改变其它播放功能
的业务接口。LOCAL_COMMAND 命中时优先释放 V2 候选音频，再执行原有命令播放或动作。

## 10. 内存边界

V2 运行时内存预算只来自旧助手不再初始化后释放的空间，主要包括：

- 有界 PSRAM 输入块池：仅覆盖短时传输抖动；
- 有界 PSRAM WebSocket 完整消息槽池；
- V2 owner task 栈、事件队列和小型协议 scratch；
- 现有 speaker 256 KiB ring：直接复用，不重复分配。

禁止调整其它模块的 task stack、queue 长度、PSRAM/内部 RAM 配额或缓存大小来为 V2 腾空间。
初始化若无法在预算内完成，V2 必须记录失败并保持其它功能运行，而不是回退到侵占其它模块内存。

## 11. 日志与里程碑

正常热路径每秒最多输出一次摘要：

`VOICE_V2 state ws pcm_up pcm_down speaker_buffered played underrun`

记录一次性的里程碑时间：

- wake；
- session_created；
- ai_begin；
- speech_end；
- input_commit；
- asr_done；
- audio_started；
- first_pcm；
- speaker_started；
- audio_done；
- response_done；
- playback_done。

每个完整 audio delta 记录 index、解码字节数、与上一块间隔、累计字节；间隔超过 800 ms 打印
`UPSTREAM_AUDIO_GAP`，但不取消回答。会话结束打印阶段耗时和输入/接收/播放/underrun 总计。
未知服务端事件打印一次类型；error 和边界错误始终保留完整原因。

## 12. 接入范围

P4 允许的最小接入修改：

- `main/CMakeLists.txt`：加入 V2 源文件并从默认源列表移除 V1 实现；
- `main/main.c`：将旧助手 API 注册点替换为 V2，保留现有事件行为；
- speaker service：只增加 V2 所需的 hold/release、排空状态或动态采样率控制，不改变 ring 容量和其它播放 API；
- S3 voice UART 解析：只转发已有 WAKE/COMMAND/AI_BEGIN 和新增的有序 SPEECH_END。

S3 只在无法由现有事件准确表达用户说话结束时，最小新增 SPEECH_END 的排队与发送；其它文件和行为
不修改。

## 13. 完成判据

代码审查应能静态证明：

- 旧 V1 没有运行时入口；
- cloud session 只有单 owner；
- 每个已接受输入 PCM 块最多上传一次；
- 本地命令不会播放候选 AI 输出；
- 输出 PCM 只有现有 speaker 256 KiB ring；
- `audio.done` 后会排空播放，不被 `response.done` 提前清除；
- 没有首 PCM 5 秒 cancel/retry、录音重放、完整回答 staging 或双 ring；
- WebSocket 回调无阻塞 speaker 写、session close 和高频日志；
- 没有修改其它功能的内存配置或业务逻辑。

根据用户最新指示，本轮交付不包含编译、烧录和实机测试结论。
