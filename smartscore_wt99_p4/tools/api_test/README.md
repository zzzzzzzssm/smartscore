# Device API test tools

## Full-page DashScope OMR

Post one local JPEG page to the P4's existing recognition endpoint:

```powershell
powershell -ExecutionPolicy Bypass -File tools/api_test/dashscope_omr_test.ps1 `
  -DeviceBaseUrl http://192.168.1.50 `
  -JpegPath D:\scores\page-1.jpg
```

The script prints only task ID, image/request statistics, token usage, event
count, and playback status. It never reads or prints the DashScope API key and
does not print the JPEG, Base64 data, or full compact score.

The firmware must be built with a privately supplied `DASHSCOPE_API_KEY`.
Source code retains only `__DASHSCOPE_API_KEY__`. In an ESP-IDF 5.5.3
PowerShell, configure, build, and flash from the same shell:

```powershell
Set-Location "D:\qianrushi\smartscore_wt99_p4_temp\smartscore_wt99_p4"
$env:DASHSCOPE_API_KEY="<set privately in this shell>"
idf.py reconfigure
idf.py build
idf.py -p COM5 flash monitor
Remove-Item Env:DASHSCOPE_API_KEY
```

The configure step fails closed when the environment variable is absent or
invalid. The value is written only to a generated header under the temporary
build directory; it is not placed in source, CMake cache, `build.ninja`, or
normal build output. Treat the build directory and firmware image as sensitive
artifacts because the device must contain the credential to authenticate.
