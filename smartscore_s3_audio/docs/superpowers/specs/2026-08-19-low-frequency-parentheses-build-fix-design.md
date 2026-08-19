# 低频八度条件编译修复设计

## 问题

ESP-IDF 5.5.3 使用 `-Werror=parentheses` 编译
`low_frequency_analyzer.c`。八度连续性表达式混合使用 `||` 和 `&&`，
虽然 C 运算优先级已有确定含义，但缺少显式分组，因而警告升级为编译错误，
导致 `ninja flash` 在生成应用程序对象文件时停止。

## 方案

保持现有语义，将表达式明确写成：

```c
unconfirmed_octave = unconfirmed_octave ||
    (previous_midi_is_valid && octave_distance_is_12 && !onset);
```

同时仅静态检查本次新增的低频分析源码是否还有相同的混合逻辑表达式。
不关闭编译器警告，不调整阈值、识别流程或其他模块。

## 验证边界

按用户此前要求，本轮只修改源码并执行文本/差异检查，不运行编译、烧录或测试。
修复后不能据此声称固件已经构建成功；需要用户后续重新运行原命令确认。
