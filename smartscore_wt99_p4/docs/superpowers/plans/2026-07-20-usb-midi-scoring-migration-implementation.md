# USB MIDI 与评分迁移实施计划

## 受保护边界

- 不修改 `components/wifi_remote`、`components/network_provisioning`、`components/ble_provisioning`、`components/web_provisioning`、`components/board_wt99`、`components/speaker_service`、`c5_hosted_slave` 和 `miniprogram`。
- `main/main.c` 只添加模块 include 和初始化调用。
- `components/device_api/src/device_api.c` 只添加评分 include、辅助函数、四个 handler、状态字段和路由。
- 不修改 C5 固件，不烧录，不擦除，不提交 Git。

## 阶段 1：USB MIDI Host 与状态

1. 实现 `components/usb_midi` 公共状态接口。
2. 移植 P4 USB Host 枚举、描述符解析、IN 端点异步传输、断开和恢复。
3. 创建原始 MIDI 事件队列和处理任务；回调只解析和入队。
4. 在 `main` 增量初始化，在 `/api/status` 追加 USB 字段。
5. 运行 ESP-IDF 5.4 build。

## 阶段 2：完整演奏音符

1. 定义统一 input source、MIDI event 和 performance note 类型。
2. 实现 recorder 的 64 项活动音符表与 1024 项动态完整音符缓冲。
3. 实现容量满、分配失败、队列丢失和 USB 断开的错误状态。
4. 将 MIDI 处理任务接到 recorder。
5. 构建并检查回调中没有阻塞锁、堆分配和评分。

## 阶段 3：标准谱上传

1. 实现动态标准谱存储与原子替换。
2. 迁移并严格校验 `score_json_parser`。
3. 添加大请求体读取和 `POST /api/score`。
4. 构建。

## 阶段 4：开始/停止记录

1. 新建 `scoring_service`，管理 idle/recording/scoring/ready/error。
2. 实现 profile/source 校验与记录生命周期。
3. 添加 `POST /api/start`；`POST /api/stop` 将评分交给独立任务并等待完整结果返回。
4. 构建。

## 阶段 5：单一 MIDI 评分引擎

1. 新建唯一参与构建的 `score_engine` 组件。
2. 从 M5 音符级整体对齐迁移 tempo/offset 估计、对齐、缺失/多余/错音、节奏与完整度。
3. 删除 frame、八度纠正、稳定帧合并和置信度路径。
4. 保留原总分权重，输出指定汇总字段和 MIDI details。
5. 构建。

## 阶段 6：结果接口

1. 评分任务保存完整结果 JSON 和稳定状态。
2. 保留 `GET /api/result` 作为最近结果重复读取和超时恢复接口，评分中不返回伪结果。
3. 构建并核对旧字段及新增毫秒字段。

## 阶段 7：完整检查

1. 运行完整 ESP-IDF build；必要时 fullclean 后重建，但不删除用户源文件。
2. 运行 `git diff --check`、受保护路径检查、评分引擎唯一性检查。
3. 汇总修改文件、迁移来源、P4/MIDI 适配、内存上限、构建结果、烧录/监视命令、USB 测试和 API 顺序。
