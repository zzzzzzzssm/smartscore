# DashScope API Key 环境变量注入设计

## 目标

让 ESP32-P4 固件构建直接使用当前 PowerShell 进程中的
`DASHSCOPE_API_KEY` 环境变量，同时保证真实密钥不进入受版本控制的
源码、Git 提交、CMake 缓存、编译命令行或常规构建日志。

用户采用以下工作流：

```powershell
Set-Location "D:\qianrushi\smartscore_wt99_p4_temp\smartscore_wt99_p4"
$env:DASHSCOPE_API_KEY="<PRIVATE_KEY>"
idf.py reconfigure
idf.py build
idf.py -p COM5 flash monitor
Remove-Item Env:DASHSCOPE_API_KEY
```

文档、源码、测试和示例不得包含真实密钥。

## 构建期数据流

`dashscope_omr` 组件的 CMake 配置读取进程环境变量，但不把值声明为
CMake cache variable，也不通过 `target_compile_definitions` 传递。组件把
值写入构建目录下的私有生成头文件，并把该生成目录加入组件的私有包含
路径。`dashscope_omr_config.h` 只引用生成头文件；受版本控制的文件继续
只保留公开占位符和接口约定。

真实密钥因此必然存在于待烧录固件及临时构建目录的生成头文件中，但不
出现在 `CMakeCache.txt`、`build.ninja` 或正常命令输出中。构建目录属于
临时敏感产物，不得归档或提交。

## 严格校验

配置阶段必须满足以下条件，否则 `idf.py reconfigure` 或触发重新配置的
`idf.py build` 立即失败：

- 环境变量存在且非空；
- 值不等于源码占位符；
- 不包含回车、换行、双引号或反斜杠，避免生成非法 C 字符串或注入额外
  预处理内容；
- 日志中的错误只说明缺失或格式非法，不回显输入值。

这种 fail-closed 行为避免用户误烧一个只能返回 `dashscope_key_missing`
的生产固件。

## 源码接口

固件继续通过 `DASHSCOPE_API_KEY` 编译期宏读取密钥，云请求、鉴权头生成
和日志规则不改变。原有运行时占位符检查保留为纵深防御，但正常生产构建
不会再走到占位符路径。

主机解析单元测试不构建 ESP-IDF `dashscope_omr` 组件 CMake，因此不要求
设置云端密钥，也不会生成私有头文件。

## 安全边界

- 不打印密钥、Authorization 头或生成头文件内容。
- 不把密钥放入命令行 `-D` 参数、CMake cache 或项目配置文件。
- 不改变百炼请求格式和北京地域地址。
- 不引入 NVS、串口或 HTTP 密钥配置接口；密钥轮换通过重新构建和烧录
  完成。
- 烧录完成后用户清除当前 PowerShell 会话的环境变量，并按需删除包含
  私有生成头文件的构建目录。

## 验证

实现后执行以下检查：

1. 未设置环境变量时，配置失败且输出不含密钥。
2. 设置一个明显的非真实测试值时，配置和固件构建成功。
3. 检查 `CMakeCache.txt`、`build.ninja` 和捕获的构建输出不含测试值。
4. 检查生成头文件位于构建目录，不位于源码目录。
5. 重新运行紧凑乐谱主机测试和源码密钥扫描。

