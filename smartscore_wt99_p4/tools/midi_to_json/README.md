# MIDI 转 JSON 工具

将标准 MIDI 转成设备 `/sdcard/scores` 使用的乐谱 JSON：

```powershell
python tools/midi_to_json/convert.py input.mid output.json --title "曲名"
```

转换器保留 MIDI 力度和 tick，默认删除第一个音符前的空白，并输出高音谱表单声部；使用 `--keep-leading-rest` 可以保留开头空白。
