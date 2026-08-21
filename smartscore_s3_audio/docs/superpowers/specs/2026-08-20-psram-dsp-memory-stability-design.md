# PSRAM DSP 内存稳定性修复设计

## 背景与目标

实机已确认运行新的固定双槽采集代码，但仍反复在
`i2s_dma_rx_callback -> xQueueGenericSendFromISR -> xTaskRemoveFromEventList`
路径触发 `StoreProhibited`。该路径是内存破坏的暴露点，不等于 I2S ISR
本身是写坏内存的位置。同期性能日志显示 `MusicDspTask` 的栈高水位仅
208 字节；ESP-IDF 5.5 在 ESP32-S3 上以字节报告该值。DSP 调用链同时
包含多组 FFT、61 键评分、音符跟踪局部数组和较大的结果结构，当前又未
启用编译器栈检查或任务栈末端 watchpoint。

硬件已确认为 ESP32-S3 N16R8，具有 8 MB Octal PSRAM，但工程当前在
`sdkconfig` 和 `sdkconfig.defaults` 中关闭 PSRAM。本次目标是消除 DSP
栈越界及其造成的 FreeRTOS 队列/任务控制块破坏，同时利用 PSRAM 降低
内部 RAM 压力。按用户要求只修改源码和配置，不执行编译、烧录或实机
测试。

## 采用方案

采用“PSRAM 工作区 + 内部 RAM 实时控制面 + 扩大 DSP 栈”的混合方案：

- 为 N16R8 启用 Octal PSRAM，并允许 `heap_caps_malloc()` 从外部 RAM
  分配普通 8-bit 数据。
- 在识别任务启动前分配并清零 DSP 工作区。FFT 输入、双麦幅度谱、融合
  频谱、窗函数、谐波选择位图、61 键评分与峰值临时数组等非 DMA 数据
  放入 PSRAM。
- 将 `MusicDspTask` 中跨循环存活的分类器、预处理器、音符跟踪器和频谱
  调试结果集中到一个 PSRAM 上下文，避免它们长期占用任务栈。
- 将音符跟踪的 61 键 `evidence` 临时数组并入复用工作区，不再在每次调用
  时创建栈数组。
- `MusicDspTask` 栈从 6144 字节增至 12288 字节，并继续分配在内部 RAM。
  任务控制块、I2S DMA 描述符、驱动事件队列和采集缓冲的内存属性不变。
- 保持 FFT/识别串行执行，工作区只允许 `MusicDspTask` 使用，不增加锁和
  并发访问。

不采用“整个 DSP 任务栈放入 PSRAM”，因为实时任务栈依赖外部 Cache，
会扩大故障面；也不采用“只增大任务栈”，因为它不能释放 FFT 和双麦
频谱占用的内部 RAM，也会保留大局部数组风险。

## 初始化与错误处理

1. 系统启动时先验证 PSRAM 已初始化，并记录总容量与可用容量。
2. `chord_detector_init()` 分配专用分析工作区；`music_detector_start()`
   分配任务上下文。所有分配必须使用 `MALLOC_CAP_SPIRAM |
   MALLOC_CAP_8BIT`，并检查返回值。
3. 任一必要工作区分配失败时，识别模块返回 `ESP_ERR_NO_MEM`，不启动采集
   和 DSP 任务，避免在不完整状态下继续运行。
4. 已成功分配但后续初始化失败时释放相应工作区，保证重复启动不会泄漏。
5. 工作区指针只在初始化完成后发布，避免任务看到半初始化数据。

## 栈与内存诊断

- 将现有 `stack_min` 日志和变量命名修正为字节，低栈告警阈值也按字节
  表达。
- 启用 FreeRTOS 任务栈末端 watchpoint，并保留现有栈溢出 canary。
- 启用编译器基本栈保护，使局部栈破坏尽量在写坏位置被发现，而不是稍后
  在 I2S ISR 中暴露。
- 启动日志输出 PSRAM 总量、DSP 工作区大小、任务上下文大小和内部 RAM
  余量；周期诊断继续输出 DSP 栈最小剩余字节。
- 不启用全面 heap poisoning，避免给实时双 FFT 路径增加持续性能开销。

## 行为边界

本修复不改变：

- 环境门限、校准准入和输入增益策略；
- YIN、FFT、泛音、单音/和弦及 61 键置信度公式；
- 双麦选择、I2S 针脚、采样率和 DMA 配置；
- P4 UART 帧、协议版本、轮询与确认机制。

因此修复后识别结果应与同一输入下的当前算法一致；变化只应体现在不再
发生随机重启、内部 RAM 更充足，以及诊断更准确。

## 源码级验收条件

- `sdkconfig` 与 `sdkconfig.defaults` 均启用 N16R8 Octal PSRAM。
- FFT、双麦幅度谱和 61 键分析临时数组不再位于 `MusicDspTask` 栈。
- `MusicDspTask` 栈大小为 12288 字节，并保留在内部 RAM。
- PSRAM 分配失败会阻止识别任务启动，并输出明确错误。
- I2S DMA、ISR 队列和任务控制数据未迁入 PSRAM。
- 栈高水位日志明确使用字节，启用 canary、末端 watchpoint 和基本编译器
  栈保护。
- 音符算法常量、环境门限和 P4 协议没有被本修复修改。
- 不执行编译、烧录或实机测试。
