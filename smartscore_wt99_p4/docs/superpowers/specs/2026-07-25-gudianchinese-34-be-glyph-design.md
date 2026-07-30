# gudianChinese 34px “备”字补充设计

## 目标

修复麦克风输入开始后的倒计时文案“准备演奏”中“备”字缺失。保持现有字号、颜色、位置、倒计时逻辑及其他界面不变。

## 现状

- 倒计时标签由 `components/screen_adapter/src/screen_adapter.c` 动态创建，使用 `lv_font_gudianChinese_34`。
- `lv_font_gudianChinese_34` 缺少 U+5907“备”。
- `lv_font_gudianChinese_42` 已包含 U+5907，可作为同系列字形来源。
- `lv_font_gudianChinese_34_extra` 已作为 34px 古典字体补充字库接入两个构建入口，目前包含“其他模式”。

## 方案

1. 从现有 `lv_font_gudianChinese_42` 中提取 U+5907 的 4bpp 字形位图和度量。
2. 按 34/42 比例缩放位图，并按 34px 字体基线换算字形度量，生成与现有补充字库格式一致的 U+5907 数据。
3. 将 U+5907 加入 `lv_font_gudianChinese_34_extra` 的位图、描述符和 Unicode 映射；保留已有“其他模式”字形不变。
4. 为麦克风倒计时创建 `lv_font_gudianChinese_34` 的局部副本，并将 fallback 指向 `lv_font_gudianChinese_34_extra`。倒计时标签继续以 34px 主字体渲染，仅缺失字形走补充字库。

## 变更边界

只允许修改：

- `vendor/smartmusic_screen/SmartMusic/components/ui-guider/generated/guider_fonts/lv_font_gudianChinese_34_extra.c`
- `components/screen_adapter/src/screen_adapter.c`

现有构建入口已经注册补充字体，不重复修改。不得改动评分、识别、通信、倒计时流程、其他 UI 或小程序代码。

## 验证

- 校验补充字体包含 U+5907 且已有四个字形映射仍然有效。
- 校验倒计时标签使用主字体加补充字体的 fallback 链。
- 运行目标文件的 `git diff --check` 和静态映射检查。
- 不执行工程编译或烧录。
