# ESP32-S3 可分享烧录包设计

## 目标

从 `smartscore_s3_vision` 项目中整理一个独立、便于分享的 ESP32-S3 交付目录，同时提供免编译烧录包和精简源码包。分享目录不包含训练数据、训练环境、训练脚本或训练产物，但保留固件运行必需的 ESP-DL 模型。

## 输出位置与结构

输出目录为 `D:\qianrushi\smartscore_s3_vision_esp_share`，最终结构如下：

```text
smartscore_s3_vision_esp_share/
├── 01_直接烧录包/
│   ├── bootloader.bin
│   ├── partition-table.bin
│   ├── smartscore_s3_vision.bin
│   ├── flash_args
│   ├── flasher_args.json
│   ├── 一键烧录.bat
│   └── 烧录说明.md
├── 02_精简源码包/
│   ├── CMakeLists.txt
│   ├── dependencies.lock
│   ├── partitions.csv
│   ├── sdkconfig
│   ├── sdkconfig.defaults
│   ├── README.md
│   └── main/
├── SHA256SUMS.txt
└── README.md
```

同时生成 `smartscore_s3_vision_esp_share.zip`，便于直接发送。

## 直接烧录包

直接烧录包从当前已完成构建的 `build` 目录提取 ESP32-S3 烧录必需文件。烧录地址以 `build/flasher_args.json` 和 `build/flash_args` 为准，不手工猜测。Windows 一键脚本接收串口参数，并优先调用 `esptool.py`，不可用时尝试 `python -m esptool`；失败时输出明确提示，不静默忽略错误。

验证内容包括：所有烧录参数引用的二进制文件均存在、文件非空、地址与构建产物一致，以及每个交付文件的 SHA-256 校验值。

## 精简源码包

源码包保留顶层 ESP-IDF 工程配置、`main` 中的全部运行时代码、主组件清单以及 `main/models/point_direction.espdl`。该 ESP-DL 文件会被链接进应用固件，是设备运行必需资源，不属于可删除的训练产物。

源码包不复制 `build`、`managed_components`、`training`、`training_data`、任何 `training_artifacts*`、`.venv*`、缓存、编辑器配置和项目设计文档。依赖通过 `main/idf_component.yml` 与 `dependencies.lock` 固定，由 ESP-IDF Component Manager 在构建时恢复。

## 使用环境

目标芯片为 ESP32-S3，Flash 大小为 16 MB。源码构建基线为 ESP-IDF 5.4.4；依赖版本由锁文件固定。源码说明将包含 `idf.py set-target esp32s3`、`idf.py build`、`idf.py -p <串口> flash monitor` 的基本命令。

## 安全与可恢复性

迁移只复制文件和创建新 ZIP，不移动或删除原项目内容。输出目录若已存在，将停止并提示，避免无意覆盖。训练目录与原构建目录保持不变。

## 完成标准

1. 独立分享目录及 ZIP 均创建成功。
2. 分享目录不包含训练数据、训练脚本、训练权重、虚拟环境或缓存。
3. 直接烧录包包含构建参数引用的全部固件文件。
4. 精简源码包包含完整运行代码和运行必需的 `point_direction.espdl`。
5. SHA-256 清单可验证交付文件完整性。
6. 烧录脚本和源码目录结构通过静态检查；条件允许时执行 ESP-IDF 构建验证。
