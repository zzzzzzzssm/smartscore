# 练习建议后台生成、设备查看与小程序历史实施计划

设计文档：`docs/superpowers/specs/2026-08-19-background-practice-advice-history-design.md`

## 实施原则

- 先完成设备端单一状态源，再接屏幕和小程序，避免两个客户端各自推断任务状态。
- 同一练习最多调用 DeepSeek 一次，同一时刻最多一个 DeepSeek HTTPS 请求。
- 所有状态关联使用设备生成的 `session_id`，不得用标题或时间猜测。
- 设备只保存当前练习建议；小程序只把匹配会话的建议写入参与过的记录。
- 保留工作区现有屏幕创作器和语音返回主页修改，不覆盖、不回退，也不把它们混入本功能提交。

## 1. 新增设备端建议服务和状态测试

新增：

- `components/practice_advice_service/CMakeLists.txt`
- `components/practice_advice_service/include/practice_advice_service.h`
- `components/practice_advice_service/src/practice_advice_service.c`
- `components/practice_advice_service/src/practice_advice_state.h`
- `components/practice_advice_service/src/practice_advice_state.c`
- `tests/practice_advice_service_test/CMakeLists.txt`
- `tests/practice_advice_service_test/test_practice_advice_state.c`
- `tests/practice_advice_service_test/run_tests.ps1`

实现：

- 定义 `none/waiting_score/running/ready/skipped_offline/failed` 状态、稳定错误码、中文消息和快照结构。
- `begin_session()` 生成启动周期内唯一的会话 ID，增加 generation，清除旧可见结果和过期待处理作业。
- `submit_score()` 在评分完成时只检查一次 `NETWORK_STATE_WIFI_CONNECTED`；离线直接终止为 `skipped_offline`。
- 使用一个常驻 FreeRTOS worker 和一个可替换的最新待处理槽串行调用 `deepseek_advice_generate()`。
- worker 提交成功或失败前比较 generation；旧会话结果只释放，不改变当前状态。
- 评分输入与建议结果优先放 PSRAM；公开读取接口返回拥有明确所有权的副本或在锁内调用 consumer。
- 把纯 generation/state 转换放进无 FreeRTOS 依赖的 `practice_advice_state.c`，为主机测试覆盖新会话、离线、运行、完成、失败和旧结果丢弃。

验证：

- 运行 `tests/practice_advice_service_test/run_tests.ps1`。
- 对新增 C 文件运行格式/静态语法检查，并执行 `git diff --check`。

## 2. 把建议生命周期接入评分服务

修改：

- `components/scoring_service/CMakeLists.txt`
- `components/scoring_service/src/scoring_service.c`
- `components/scoring_service/include/scoring_service.h`

实现：

- 初始化评分服务时初始化 `practice_advice_service`。
- 每次 `scoring_service_start()` 真正成功后开始新建议会话，并把 `session_id` 保存到评分状态。
- 本地评分 JSON 成功提交后调用 `submit_score()`；评分失败则调用建议服务的评分失败终止接口。
- 在 `scoring_service_status_t` 中暴露当前 `practice_session_id`，使 HTTP 评分响应与建议状态使用同一 ID。
- reset 只重置评分状态；旧建议在下一次成功开始练习时清除，保持结果页可查看当前建议。

验证：

- 静态检查所有评分成功、失败和重启路径都只通知一次。
- 构建设备固件，确认组件依赖无循环。

## 3. 增加设备 API 状态与兼容路由

修改：

- `components/device_api/CMakeLists.txt`
- `components/device_api/src/device_api.c`

实现：

- `/api/status` 增加 `practice_session_id`、`advice_state` 和 `advice_ready`。
- 新增 `GET /api/practice/advice`，所有业务状态均返回稳定 JSON；只有 `ready` 携带完整建议对象。
- 修改 `POST /api/ai/score` 为只读兼容接口，按设计规定的 200/202/409/502/503 返回，不再直接调用 `deepseek_advice_generate()`。
- `/api/stop` 和 `/api/result` 在评分 JSON 根对象增加 `practice_session_id`，保持原评分字段位于顶层。
- 复用短时快照，不在 HTTP server task 中等待后台建议完成。

验证：

- 静态检查路由只注册一次，旧 POST 中不存在模型调用。
- 检查 JSON 响应在空状态、运行、离线、失败和完成时均有效。

## 4. 在设备评分页增加建议按钮和滚动页面

新增：

- `components/practice_advice_view/CMakeLists.txt`
- `components/practice_advice_view/include/practice_advice_view.h`
- `components/practice_advice_view/src/practice_advice_view.c`

修改：

- `components/screen_adapter/CMakeLists.txt`
- `components/screen_adapter/src/screen_adapter.c`

实现：

- 在当前评分页右上区域动态创建“练习建议”按钮，避开现有“返回选曲”按钮和评分内容。
- 点击时读取服务快照：运行中显示等待提示，离线显示不补跑提示，失败显示错误映射，未完成评分显示引导。
- 完成时复制建议 JSON，解析现有 schema 并生成 LVGL 全屏可滚动内容；展示总结、重点练习、证据、参数、目标、下次计划、鼓励和数据不足说明。
- 使用 `app_font_chinese_22()` 及现有字体回退，提供“返回评分”按钮。
- UI 对象只在 LVGL/display 锁内创建或销毁；后台 worker 不接触 LVGL。
- 不修改 `ui-guider/generated` 文件。

验证：

- 构建固件检查 LVGL 9 API 和组件依赖。
- 静态检查按钮事件不会触发 DeepSeek，仅消费状态。

## 5. 改造小程序建议同步和练习记录

新增：

- `miniprogram/utils/practice_advice.js`
- `miniprogram/tests/practice_advice_sync.test.js`

修改：

- `miniprogram/utils/api.js`
- `miniprogram/pages/practice/practice.js`
- `miniprogram/pages/practice/practice.wxml`
- `miniprogram/pages/practice/practice.wxss`
- `miniprogram/tests/practice_advice.test.js`
- `miniprogram/tests/practice_socket_budget.test.js`

实现：

- 把建议格式化和记录更新逻辑集中到 `utils/practice_advice.js`，保留页面测试所需导出。
- 新增 `getPracticeAdvice()`；保留 `requestAiScore()` 仅作只读兼容包装。
- 停止练习后用响应中的 `practice_session_id` 创建记录，并写入固定建议字段。
- 把 `/api/status` 的建议摘要接入现有非重叠轮询；终态只处理一次，`ready` 时再请求一次完整建议。
- 严格按 `practiceSessionId` 更新记录；设备会话不匹配时写入 `sync_missed`。
- 原手动生成按钮改成状态/查看按钮。运行中点击显示等待提示，完成后打开建议，离线和失败显示保存原因。
- 不为建议新增高频定时器，不恢复已移除的长连接式同步生成请求。

验证：

- 覆盖自动同步、去重、会话匹配、终态持久化、页面隐藏恢复和错误提示。
- 更新 socket-budget 测试，确认建议读取不会暂停评分状态轮询或重复创建设备请求。

## 6. 增加历史回看和安全清理缓存

修改：

- `miniprogram/pages/history/history.js`
- `miniprogram/pages/history/history.wxml`
- `miniprogram/pages/history/history.wxss`
- `miniprogram/pages/profile/profile.js`
- `miniprogram/pages/profile/profile.wxml`
- `miniprogram/pages/profile/profile.wxss`
- `miniprogram/tests/profile_modal_content.test.js`

新增：

- `miniprogram/tests/profile_clear_cache.test.js`

实现：

- 历史卡片显示建议状态标记，详情面板展示保存的完整建议文本或未生成原因。
- 重命名保留建议字段；删除记录自然删除嵌入的建议。
- 用户页新增“清理缓存”，二次确认后只删除 `apiBaseUrl`、`bluetoothDevice` 和 `deviceState`。
- 同步清空 `getApp().globalData.apiBaseUrl`；不得调用 `wx.clearStorage()`。
- 测试确认 `scores`、`currentScoreId`、`currentExternalScore`、`practiceRecords` 和迁移标记均保留。

## 7. 全量验证与提交

验证命令：

- 对 `miniprogram/**/*.js` 运行 `node --check`。
- 逐个运行 `miniprogram/tests/*.test.js`。
- 运行 `tests/practice_advice_service_test/run_tests.ps1`。
- 运行 `git diff --check`。
- 在当前 ESP-IDF 环境执行完整 `idf.py build`；如果工具链不可用，记录精确阻塞信息并至少完成组件静态检查。

人工审查：

- 搜索确认 `deepseek_advice_generate()` 只从建议服务 worker 调用。
- 搜索确认小程序不再通过按钮生成建议。
- 检查真实 API 密钥未进入源码、日志、测试或提交。
- 检查本功能提交没有包含原有创作器/语音返回主页的未提交文件。

建议提交边界：

1. `feat: generate practice advice in device background`
2. `feat: show and retain practice advice in mini program`

硬件验收按设计文档列出的八个场景执行；本地无法代替真实设备验证触摸、联网和字体显示时，在交付说明中明确列出待验收项。
