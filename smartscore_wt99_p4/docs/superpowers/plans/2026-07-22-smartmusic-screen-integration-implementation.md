# SmartMusic 完整屏幕功能迁移实施记录

依据：`docs/superpowers/specs/2026-07-22-smartmusic-screen-integration-design.md`
迁移前基线：`3951404de71fcc2721859a860ca0e49b17d6e408`

## 阶段一：源快照与可追溯性

- [x] 校验 `SmartMusic (1).zip` 完整性和 SHA-256。
- [x] 将屏幕功能依赖闭包原样提取到 `vendor/smartmusic_screen/SmartMusic/`。
- [x] 纳入 UI、字体、图片、乐谱显示、MusicXML、创作、存储、显示 BSP、音频及 USB 等全部相关源码。
- [x] 生成 186 项 SHA-256 清单并逐项回读验证，缺失、额外和哈希不符均为 0。

## 阶段二：构建包装

- [x] 为原始组件建立只引用快照源码的 CMake 包装。
- [x] 增加显示 BSP 适配组件，并共享现有音频 I2C 总线。
- [x] 加入固定版本的 LVGL、显示、触摸和 Expat 依赖。
- [x] 解决同名解析符号冲突，正式固件只链接白名单源码。

## 阶段三：业务适配

- [x] 增加异步 `screen_adapter`，显示失败不阻塞原工程服务。
- [x] 接入主页、选曲、演奏准备、乐谱、结果、设置和创作完整页面流程。
- [x] 通过公开接口连接现有 `score_data`、`scoring_service`、`input_source_manager`、`speaker_service` 和 `usb_midi`。
- [x] 复用现有唯一 USB Host，并将 MIDI 事件同时分发给评分和屏幕创作流程。
- [x] 接入练习速度换算、节拍器、实时音符着色、结果页及音量控制。

## 阶段四：保护与验证

- [x] 受保护 Wi-Fi、蓝牙、小程序、扬声器、USB MIDI 和板级音频目录相对基线零改动。
- [x] 链接清单中旧网络、旧音频、旧 USB Host、旧评分引擎和第二个 `app_main` 均为 0。
- [x] `git diff --check` 通过。
- [x] 186 个源快照文件逐项哈希复核通过。
- [x] ESP-IDF 5.5.3 正式固件全量构建成功，应用分区剩余 15%。
- [x] 压缩包自带的 7 个 Creator 和 4 个 MusicXML 测试用例全部编译、链接成功。

## 阶段五：交付

- [x] 记录迁移结构、验证数据、构建环境和板上验收事项。
- [x] 清理临时测试工程，正式构建产物保留在忽略的 `tmp/build_screen/`。
- [x] 创建最终 Git 提交并确认工作区干净。
