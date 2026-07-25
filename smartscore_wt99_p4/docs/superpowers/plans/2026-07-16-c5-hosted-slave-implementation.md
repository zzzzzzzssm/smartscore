# WT99 ESP32-C5 Hosted Slave Implementation Plan

1. Activate the verified ESP-IDF 5.5.3/Python 3.11 environment and verify the
   `esp32c5` target.
2. Generate `c5_hosted_slave/` from the official component-registry example
   `espressif/esp_hosted=2.12.6:slave`.
3. Inspect the generated project and its actual ESP-Hosted 2.12.6 Kconfig
   symbols before changing configuration.
4. Put stable WT99 C5 settings in defaults: ESP32-C5 target, on-board P4-C5
   preset, SDIO transport, Wi-Fi, Bluetooth sharing, and controller-only mode.
5. Run `idf.py set-target esp32c5` and `idf.py reconfigure` with ESP-IDF 5.5.3.
6. Verify the resolved `sdkconfig` contains the required SDIO, Wi-Fi, and
   Bluetooth values and contains no alternate UART/SPI Hosted transport.
7. Run `idf.py build`; fix the first real configuration/compiler/linker error
   without disabling SDIO, Wi-Fi, or Bluetooth.
8. Verify output metadata: target, ESP-Hosted firmware version, partition and
   flash arguments, image sizes, and SHA-256 hashes.
9. Document the exact manual COM8 flash/reset/monitor commands without running
   them.

Safety boundary: do not execute `flash`, `erase-flash`, OTA activation, or any
other command that writes to the C5.
