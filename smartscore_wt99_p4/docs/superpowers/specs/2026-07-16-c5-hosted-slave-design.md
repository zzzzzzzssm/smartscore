# WT99 ESP32-C5 ESP-Hosted Slave Firmware Design

## Objective

Create a standalone ESP32-C5 firmware project at `c5_hosted_slave/` for the
WT99P4C5-S1 board. The firmware provides Wi-Fi and Bluetooth controller
capabilities to the ESP32-P4 host over the board's fixed 4-bit SDIO link.

This step builds and verifies the C5 image only. It does not flash or erase the
C5 and does not modify the P4 application.

## Version Contract

- ESP-IDF: 5.5.3, installed at
  `D:/Espressif/frameworks/esp-idf-v5.5.3`.
- ESP-Hosted slave: exactly 2.12.6.
- P4 host ESP-Hosted: 2.12.6, as locked by the existing P4 project.
- P4 Wi-Fi Remote: 1.6.0; it is not built into the C5 slave image.

Matching the ESP-Hosted major and minor versions prevents the RPC and
Bluetooth feature-control incompatibility present in the vendor's older 2.0.13
prebuilt image.

## Source Approach

Generate `c5_hosted_slave/` from the official Espressif component-registry
example `espressif/esp_hosted=2.12.6:slave`. This is preferred over copying the
P4 project's generated `managed_components/` directory or cloning the full
ESP-Hosted repository.

The generated C5 project is self-contained and has its own build directory,
configuration, dependency lock file, and firmware outputs. It does not depend
on the P4 build directory.

## Functional Configuration

The C5 configuration will explicitly enable:

- target `esp32c5`;
- ESP-Hosted coprocessor application;
- SDIO slave transport;
- Wi-Fi capability;
- Bluetooth sharing through Hosted HCI on the shared SDIO transport;
- Bluetooth controller-only mode;
- the ESP32-P4-C5 on-board C5 board preset when offered by ESP-Hosted 2.12.6.

The dedicated C5 SDIO pins are fixed by the ESP32-C5 peripheral and match the
WT99 schematic. No P4 GPIO numbers are copied into the C5 firmware.

Optional examples, network split, MQTT, iperf traffic tests, console business
commands, and unrelated application features remain disabled unless they are
required by the upstream minimal slave build.

## Generated Configuration Policy

Stable settings belong in `c5_hosted_slave/sdkconfig.defaults` and the
target-specific defaults supplied by the official example. The generated
`sdkconfig` and `build/` output are local build artifacts, not hand-maintained
source files.

No files inside the P4 project's `managed_components/` directory are edited.

## Build and Verification

The build procedure will:

1. activate ESP-IDF 5.5.3 with its Python 3.11 environment;
2. create the official 2.12.6 slave example;
3. set the target to `esp32c5`;
4. apply and inspect the required configuration;
5. run `idf.py reconfigure` and `idf.py build`;
6. stop on the first real configuration or compiler error and fix it without
   disabling Wi-Fi, Bluetooth, or SDIO;
7. verify the ELF/image target, ESP-Hosted version, firmware size, flash
   arguments, and SHA-256 hashes of the produced binaries.

No `idf.py flash`, `erase-flash`, or C5 OTA command is executed.

## Expected Runtime Contract

After the user later flashes the image through COM8 and resets C5 with BOOT
released, the C5 UART log should identify:

- ESP32-C5;
- ESP-Hosted-MCU Slave firmware 2.12.6;
- SDIO transport;
- WLAN over SDIO;
- Bluetooth/Hosted HCI capability.

The P4 host should then receive the Hosted INIT response without a firmware
version mismatch and may proceed to initialize Wi-Fi Remote and NimBLE Host.

## Failure Handling

- Component download failure: preserve the project state and report the exact
  registry/network error.
- Unsupported or renamed Kconfig symbol: inspect the generated 2.12.6 Kconfig
  and use its actual symbol; do not invent settings.
- Build failure: report and address the first compiler/linker/configuration
  error.
- Image verification failure: do not provide a flash command until target,
  version, and image layout are confirmed.
- Flashing remains a manual user action on COM8 after build review.
