# 小程序与屏幕模式联动实施计划

依据：
`docs/superpowers/specs/2026-07-24-miniprogram-screen-mode-integration-design.md`

## 1. 设备端 SD 乐谱目录能力

- 扩展屏幕乐谱存储接口，增加只扫描 `/sdcard/scores`、只读取 SD 文件和安全文件名
  校验的公共函数。
- 保持现有屏幕 `score_storage_scan()` 的内置/SPIFFS/SD 行为不变。
- 为设备 API 提供分页所需的稳定元数据，不把内置测试乐谱暴露给小程序。

涉及文件：

- `vendor/smartmusic_screen/SmartMusic/components/smartscore_core/score_storage.h`
- `vendor/smartmusic_screen/SmartMusic/components/smartscore_core/score_storage.c`
- `components/screen_legacy_storage/CMakeLists.txt`

## 2. 创作者共享控制

- 扩展 `creator_mode`，让屏幕事件和远程命令复用同一组开始、暂停、继续、结束保存和
  取消操作。
- 增加公开状态结构，包含配置、录制状态、音符数、小节数和最近保存结果。
- 使用互斥和显示锁保护 HTTP、LVGL 与 MIDI 事件的并发访问。
- 在 `screen_adapter` 中提供线程安全包装，并阻止练习与创作者并发启动。

涉及文件：

- `vendor/smartmusic_screen/SmartMusic/main/creator_mode.h`
- `vendor/smartmusic_screen/SmartMusic/main/creator_mode.c`
- `components/screen_adapter/include/screen_adapter.h`
- `components/screen_adapter/src/screen_adapter.c`

## 3. 设备 HTTP API

- 增加：
  - `GET /api/scores/sd`
  - `POST /api/scores/sd/select`
  - `GET /api/scores/sd/file`
  - `GET /api/creator/status`
  - `POST /api/creator/start`
  - `POST /api/creator/action`
- 增加一致的 JSON 错误码、分页、搜索、文件名校验和 409 模式冲突响应。
- 扩充 HTTP server URI handler 容量。
- 让既有 `/api/start` 在创作者活动时拒绝开始。

涉及文件：

- `components/device_api/CMakeLists.txt`
- `components/device_api/src/device_api.c`

## 4. 小程序数据层

- 新增本地乐谱库工具，负责规范化、持久化索引、用户目录 JSON、删除和示例迁移。
- `app.js` 移除 mock 初始化并恢复真实当前乐谱。
- 删除 `utils/mock.js`。
- `api.js` 增加 SD 乐谱和创作者接口。

涉及文件：

- `miniprogram/app.js`
- `miniprogram/utils/api.js`
- `miniprogram/utils/score_library.js`
- `miniprogram/utils/mock.js`

## 5. 小程序页面

- 首页保留原 UI，把连接设备改为标题右侧小按钮，原卡片改为创作者。
- 乐谱页改为上传、设备 SD 卡、手机本地三个来源；上传成功进入本地库。
- 新增独立创作者页及每秒状态轮询。
- 练习页把操作按钮移到顶部、评分移到图谱之后，并按输入模式精简实时状态。
- “我的”页轻量调整，增加设备入口并显示真实空统计。

涉及文件：

- `miniprogram/app.json`
- `miniprogram/pages/index/*`
- `miniprogram/pages/scores/*`
- `miniprogram/pages/creator/*`
- `miniprogram/pages/practice/*`
- `miniprogram/pages/profile/*`

## 6. 测试与文档

- 增加纯 JavaScript 测试覆盖示例迁移、乐谱规范化和本地索引。
- 扩展创作者组件测试或加入可编译的状态转换测试。
- 更新小程序测试步骤和 README，列出未实现的屏幕专属能力。
- 运行：
  - 所有小程序 JavaScript 的 `node --check`
  - 小程序纯逻辑测试
  - `git diff --check`
  - ESP-IDF 完整构建
- 记录未执行的真机 SD、USB MIDI 和触摸回归项。

涉及文件：

- `miniprogram/docs/test_steps.md`
- `miniprogram/README.md`
- `tests/` 或小程序纯逻辑测试目录
