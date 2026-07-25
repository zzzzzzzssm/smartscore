# SmartMusic 完整屏幕功能迁移设计

日期：2026-07-22  
状态：已获用户批准  
迁移前基线：`3951404de71fcc2721859a860ca0e49b17d6e408`

## 1. 目标

将 `SmartMusic (1).zip` 中与屏幕、乐谱显示、页面流程和创作模式相关的全部代码、资源和测试整理到当前 `smartscore_wt99_p4` 工程，并使其通过独立适配层连接当前已经存在的乐谱、评分、USB MIDI、输入源和扬声器服务。

迁移必须同时满足：

- 不因代码尚未完成或存在已知问题而删除屏幕相关功能。
- 从压缩包迁入的原始文件保持字节级不变，并可通过清单追溯。
- 不替换、不回退当前 Wi-Fi、蓝牙、微信小程序、扬声器、USB MIDI 和评分实现。
- 旧工程中重复的硬件初始化代码不得进入当前启动路径。
- 屏幕初始化失败不得阻塞当前网络、蓝牙、音频和设备 API。
- 完成可重复构建、回归检查和 Git 备份。

## 2. 输入基准

源文件：`C:\Users\17762\Downloads\SmartMusic (1).zip`

- 文件大小：`724885719` 字节
- 最后修改时间：`2026-07-22 22:16:12`
- SHA-256：`4C0ADF5DD4E13D1EB066A8B894E0842E76054FF0C167CE864C83BD43F8599E2B`
- ZIP 完整性：必须在迁移脚本开始时再次执行完整校验。

## 3. 方案选择

采用“完整原样快照 + 构建包装组件 + 适配层”的方案。

不采用直接覆盖方案，因为源工程的 `main`、网络、音频和 USB MIDI 初始化会与当前工程重复并可能抢占硬件。不采用双固件切换方案，因为它不能形成一套结合后的产品功能。

## 4. 文件布局

### 4.1 原样源快照

在 `vendor/smartmusic_screen/` 保存屏幕功能的原始依赖闭包。至少包含下列完整目录或文件：

- `components/app_font/`
- `components/creator_core/`
- `components/midi_parser/`
- `components/musicxml_renderer/`
- `components/music_display/`
- `components/ui-guider/`
- `components/smartscore_core/`
- `components/wt99p4c5_s1_board/`
- `main/creator_mode.c`
- `main/creator_mode.h`
- 原工程中上述代码直接引用的其他屏幕资源和头文件

目录内的源码、CMake 文件、测试、字体、图片以及随组件保存的诊断资源全部保留。`build/`、`.git/`、`managed_components/`、`otherESP/` 和与屏幕依赖无关的网络、小程序副本不纳入快照。

迁移工具生成 `docs/migration/smartmusic_screen_source_manifest.sha256`，记录每个迁入文件相对路径和 SHA-256。该清单用于证明原始快照未被修改。

### 4.2 当前工程构建组件

现有占位组件用于组织构建，不复制修改原始源码：

- `components/music_display/`：包装简谱、五线谱和绘制资源。
- `components/midi_parser/`：包装原 MIDI 乐谱解析器。
- `components/ui_generated/`：包装 UI Guider 页面、字体和图片。
- `components/ui_app/`：负责页面生命周期和页面事件装配。
- `components/screen_adapter/`：新增的唯一业务适配组件。
- `components/display_board_adapter/`：只负责显示屏、触摸和 LVGL/BSP 的初始化，不接管音频、SD 卡、Wi-Fi 或蓝牙。

包装组件的 CMake 可以引用 `vendor/smartmusic_screen/` 中的文件，但不得修改快照内容。若原组件包含与当前工程冲突的源文件，只在包装组件中选择构建集合；冲突文件仍完整保存在快照中。

## 5. 运行时架构

### 5.1 硬件所有权

当前工程继续拥有以下硬件和服务：

- Wi-Fi 与 ESP-Hosted：`wifi_remote`、`network_provisioning`
- 蓝牙配网：`ble_provisioning`
- 微信小程序 HTTP API：`device_api`
- 扬声器、音量、节拍器和 WAV：`speaker_service`
- USB MIDI：`usb_midi`
- 输入源：`input_source_manager`
- 乐谱和评分：`score_data`、`scoring_service`

迁入代码只拥有显示屏、触摸、LVGL 对象、页面状态、谱面布局和创作页面状态。压缩包中的旧网络、旧音频、旧 USB MIDI 和旧应用入口不得启动。

### 5.2 启动顺序

保留当前 `app_main()` 的所有既有初始化语句和顺序，只增加一次非阻塞的 `screen_adapter_start()` 调用。调用位置位于当前乐谱、评分、USB MIDI和扬声器服务可用之后，进入长期网络状态循环之前。

屏幕初始化在自己的任务中完成。初始化失败时记录错误并结束屏幕任务，不进入当前工程的阻塞错误循环，也不停止其他服务。

### 5.3 数据流

- 选曲和乐谱加载：屏幕事件 → `screen_adapter` → `score_data`
- 开始、停止和最终评分：屏幕事件 → `screen_adapter` → `scoring_service`
- USB MIDI 演奏：当前 `usb_midi` → 当前 `scoring_service` → `screen_adapter` → 实时音符着色
- 麦克风/USB 输入选择：屏幕事件 → `input_source_manager`
- 节拍器、音量、提示音：屏幕事件 → `speaker_service`
- 最终评分 JSON：`scoring_service` → `screen_adapter` → `music_display_show_score()`
- 创作和只读浏览：保留原页面流程；保存或演奏动作通过适配层进入当前数据服务。

优先使用当前组件已经公开的 API。若当前组件没有所需观察接口，先在适配层通过轮询或状态快照连接；不得为了屏幕迁移修改受保护子系统内部实现。原功能暂时无法映射时保留页面和原始代码，返回明确的“不支持/未就绪”状态并记录日志，不伪造成功。

## 6. 冲突处理规则

- 同名评分引擎、应用状态、网络、音频和 USB MIDI实现：原样保存在快照中，但不链接进正式固件。
- 同名符号：由包装组件白名单选择源文件，禁止通过删除或改名快照源码解决。
- 依赖版本：优先沿用当前 ESP-IDF 5.4.4 和现有 ESP-Hosted 版本；仅添加屏幕所需 LVGL、显示 BSP 和字体依赖。
- 板级引脚：显示适配器只声明屏幕与触摸需要的资源；不得改写当前 C5、音频功放、SD 卡或 USB MIDI 引脚。
- 内存不足：通过构建映射和运行时分配检查定位；不得通过删除页面、字体或乐谱能力规避。

## 7. 受保护范围

迁移完成后，下列路径相对于基线提交必须无源码差异：

- `components/wifi_remote/`
- `components/network_provisioning/`
- `components/ble_provisioning/`
- `components/web_provisioning/`
- `components/device_api/`
- `components/speaker_service/`
- `components/usb_midi/`
- `components/board_wt99/src/board_audio.c`
- `miniprogram/`
- `c5_hosted_slave/`

允许变化的既有文件仅限顶层/主组件构建元数据、依赖锁文件、默认配置以及 `main/main.c` 中的一次屏幕适配层启动接入。任何额外变化都必须在提交前撤销或明确说明。

## 8. 测试与验收

### 8.1 静态验收

- ZIP 完整性校验通过。
- 快照文件与源 ZIP 对应文件逐一进行大小和 SHA-256 校验。
- 迁移清单无遗漏、无零字节替换、无路径碰撞。
- 正式固件链接图中不存在旧网络、旧音频、旧 USB MIDI 和第二个 `app_main`。
- 对受保护路径执行基线差异检查，结果为空。

### 8.2 构建验收

- 使用当前 ESP-IDF 5.4.4 配置执行干净构建。
- 构建目标仍为 `esp32p4`，当前 ESP-Hosted/Wi-Fi Remote 版本不变。
- 生成 `.bin`、`.elf` 和 `.map`，记录固件大小及剩余分区空间。
- 编译迁入的 creator core 和 MusicXML renderer 测试；可以运行的主机/Unity 测试必须执行。

### 8.3 功能回归清单

- 固件仍包含现有 Wi-Fi、蓝牙配网、HTTP API、扬声器、USB MIDI、评分和小程序协议组件。
- 屏幕完整页面可创建：主页、选曲、演奏准备、演奏、结果、设置和创作模式。
- 简谱/五线谱、分页、手势、只读返回、字体和图片资源进入固件。
- 实时评分及结果页适配调用存在且不会重复初始化 USB MIDI。
- 屏幕启动失败时，网络和设备 API 初始化路径仍继续执行。

没有实体板卡时，构建与静态验证可以证明集成完整性，但不能替代屏幕、触摸、音频和无线共存的上板验证；未完成的板上项目必须形成明确测试清单。

## 9. Git 备份策略

1. 已创建迁移前基线提交 `3951404`。
2. 单独提交本设计文档。
3. 实施完成后提交屏幕迁移与适配结果。
4. 最终工作区必须干净，并记录最终提交哈希、基线到最终提交的差异摘要和构建结果。

不执行远程推送，除非用户提供或确认远程仓库及推送目标。本地 Git 提交已经提供可回退备份。

## 10. 非目标

- 不在本次迁移中修改源屏幕代码内部的两秒提示等历史缺陷；评分结果和实时着色属于本次适配范围，必须在当前公开服务能力允许的范围内接通。
- 不重构当前网络、蓝牙、小程序、音频、USB MIDI 或评分组件。
- 不烧录板卡，不改变 C5 固件。
- 不提交源 ZIP 中的构建产物、仓库历史和第三方依赖缓存。
