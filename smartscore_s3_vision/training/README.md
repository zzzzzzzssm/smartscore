# 左右指向模型训练工具

该目录只用于电脑端训练，不参与 ESP-IDF 固件编译。原始照片位于 `training_data/point_dataset`，脚本不会覆盖原图。

## 当前方案

- 类别固定为 `point_right=0`、`point_left=1`、`other=2`。
- 数据按人员执行八折留一验证，留出人员不参与训练、早停或阈值选择。
- 排除 `landmark_direction_disagrees` 样本；水平翻转时同步交换左右标签。
- 比较 96×96 TinyPointNet、MobileNetV3-Small 0.5 和 0.75，选择满足全部门槛且预计运算量最低的模型。
- TinyPointNet 使用各留人折对应的 MobileNetV3 教师模型蒸馏，避免验证人员信息泄漏。
- 优先导出 ESP32-S3 INT8 `.espdl`，部署前自动检查准确率、量化一致性、误触发率和文件大小。

候选模型门槛：整人留出准确率和宏平均 F1 均不低于 90%，阈值后 `other` 误触发率不高于 2%，最差人员准确率不低于旧基线 80.65%；量化后准确率和宏平均 F1 均不低于 90%，预测一致率不低于 97%。

## 一键复现

在 PowerShell 中执行：

```powershell
cd D:\qianrushi\smartscore_s3_vision
powershell -ExecutionPolicy Bypass -File training\run_tiny_fast_pipeline.ps1 -Step all
```

流水线支持以下步骤：

```powershell
powershell -ExecutionPolicy Bypass -File training\run_tiny_fast_pipeline.ps1 -Step train
powershell -ExecutionPolicy Bypass -File training\run_tiny_fast_pipeline.ps1 -Step export
powershell -ExecutionPolicy Bypass -File training\run_tiny_fast_pipeline.ps1 -Step quantize
powershell -ExecutionPolicy Bypass -File training\run_tiny_fast_pipeline.ps1 -Step validate
powershell -ExecutionPolicy Bypass -File training\run_tiny_fast_pipeline.ps1 -Step deploy
```

训练可断点续跑。中间结果保存在 `training_artifacts_tiny_fast/run_96`，不会提交到 Git；固件只保存通过门槛的最终 `.espdl` 模型和指标摘要。

## 当前入选模型

TinyPointNet INT8：50,263 参数、3.57M MAC、86,944 字节；八折整人留出准确率 93.40%、宏平均 F1 93.46%、最差人员准确率 88.33%；1200 张量化评估准确率 96.75%、宏平均 F1 96.73%。完整指标保存在 `main/models/point_direction_metrics.json`。

以后采集的新人员应先作为完全独立测试集验证，再决定是否加入下一轮训练。
