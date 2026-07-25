# `/api/stop` 返回本地评分结果设计

## 范围

本次只修正评分结果返回流程，不实现本地屏幕驱动、实时跟谱匹配、谱面绘制或小程序实时跟谱。

USB MIDI 演奏仍由 ESP32-P4 本地记录和评分。小程序不参与评分计算，也不需要轮询才能取得正常结果。

## 接口语义

- `POST /api/start`：开始 USB MIDI 演奏记录。
- `POST /api/stop`：结束记录，将不可变快照交给独立评分任务，等待本地评分完成，并在同一次 HTTP 响应中返回完整评分 JSON。
- `GET /api/result`：继续保留，用于重复读取最近一次完成结果和故障恢复；它不是正常小程序流程的必需步骤。
- `GET /api/status`：继续提供 USB MIDI、记录和评分状态。

小程序现有 `stopPractice()` 可继续直接使用 `/api/stop` 返回的 `total_score`、`pitch_score`、`rhythm_score`、`complete_score` 和 `details`，不修改页面布局或图表逻辑。

## 本地执行与并发

评分引擎继续运行在现有独立 FreeRTOS 评分任务中，不移入 HTTP handler。`/api/stop` 只负责停止记录、提交任务并等待完成事件，因此 USB 回调仍只解析和入队，评分也不会运行在 USB 回调或 MIDI 消费任务中。

等待设置 10 秒上限，低于小程序现有 12 秒 HTTP 超时：

- 完成：HTTP 200，直接发送完整评分 JSON。
- 评分失败：返回稳定的 `ok=false/error/message`。
- 等待超时：返回 HTTP 504 和 `scoring_timeout`；后台任务可以继续完成，随后仍可通过 `/api/result` 读取。

每次 `/api/start` 清除上一次完成事件和旧结果，避免误返回旧评分。评分任务在写入新结果或错误状态后再通知等待者。

## 保持不变的边界

- 不修改 ESP32-C5、ESP-Hosted、Wi-Fi、BLE、网页配网和扬声器实现。
- 不修改小程序页面、折线图和评分展示代码。
- 不实现 `realtime_follow`、`music_display` 或 `board_display`。
- 不改变评分算法、权重、容量、PSRAM 优先策略和结果字段。
- 不烧录、不擦除 Flash、不修改 C5 固件、不创建 Git 提交。

## 验证

1. 编译检查评分服务同步等待接口。
2. 编译检查 `/api/stop` 直接返回完整结果。
3. 检查 `/api/result` 仍可重复读取相同结果。
4. 检查错误、超时、USB 拔出不会返回旧结果。
5. 从独立构建目录执行完整 ESP-IDF 5.4 构建。
