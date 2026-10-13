# Serial Flasher Tests

## Overview

The following kinds of tests are written for serial flasher:

- Deterministic host port tests
- Qemu tests
- Target tests

## USB RX Host Regression

```bash
python test/run_usb_cdc_rx_regression.py --output-dir build-usb-rx-test
```

Requires Python 3 and `clang` (or `--compiler` selecting a C11 compiler). The
runner compiles the actual `esp32_usb_cdc_acm_port.c` and `slip.c`, with SDK
doubles limited to allocation, static stream capacity, USB callbacks, and a
deterministic arrival clock. It records the compiler command and exit/output
in `result.json`. It never opens a serial port or contacts a target.

The regression covers the starved-consumer 8218-byte SLIP window and exact
decoded bytes, 32 ACK-separated windows through ring wraparound, a deterministic
reproduction of the old 4095-byte usable capacity failure even without SLIP
escaping, sticky RX loss with reset/control recovery, new-session cleanup,
PSRAM allocation failure without internal fallback, and short receives sharing
one deadline (including disconnect/loss during I/O).

The stream double follows FreeRTOS static `length - 1` capacity. It does not
prove actual FreeRTOS concurrency, USB DMA/cache behavior, stub pacing, memory
peaks, or hardware stability. The window proof relies on the bundled
`esp-flasher-stub` v0.8.0 enforcing one unacknowledged packet and sending MD5
after the final ACK; a stub or protocol-window change requires revisiting that
bound. Hardware release acceptance must rerun complete flash readback, USB
unplug/error recovery, serial bridging, and host RAM/PSRAM gates.

## Qemu Tests

Qemu tests use emulated esp32 to test the correctness of the library.

### Installation

Install QEMU for ESP-IDF by following the [official ESP-IDF QEMU guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/qemu.html).

`qemu-system-xtensa` must be available on `PATH`. This is the case by default when using the ESP-IDF installer or the `espressif/idf` Docker image.

Optionally, if your `qemu-system-xtensa` binary is not on `PATH`, set `QEMU_PATH` to point to it:

```bash
export QEMU_PATH=path_to_qemu-system-xtensa
```

### Running

```bash
./run_qemu_test.sh
```

## Target Tests

To install all the necessary tools for running the Build and Target tests just run the following command:

`pip install -r test/requirements_test.txt`

### Build

The build process consists of two steps:

1. **Build target firmware binaries** - These are the firmware images that will be flashed onto the target ESP devices
2. **Build host examples** - These are the examples that run on the host device and perform the flashing

#### Step 1: Build Target Firmware Binaries

Target firmware binaries must be built separately before building the host examples. These binaries are built from the source in `test/target-example-src/` and then copied to the appropriate `examples/*/target-firmware/` directories.

For detailed instructions on building target firmware, see [test/target-example-src/README.md](target-example-src/README.md).

After building the target firmware, copy the binaries to the example directories using the provided script:

```bash
python3 test/copy_target_binaries.py
```

This script will automatically copy the appropriate binaries from `test/target-example-src/hello-world-ESP32-src/build-*/` to the corresponding `examples/*/target-firmware/` directories based on the chip type and build configuration.

#### Step 2: Build Host Examples

Each example can be built according its README. To make things simpler, there is a tool to build all Espressif SoC examples with one command called [idf-build-apps](https://docs.espressif.com/projects/idf-build-apps/en/latest/). Before executing the [idf-build-apps](https://docs.espressif.com/projects/idf-build-apps/en/latest/), you need to run export script of [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html). After this, you are able to run the following command to build the examples:

```bash
python -m idf_build_apps build -v -p .
      --recursive
      --exclude ./test/target-example-src
      --config "sdkconfig.defaults*"
      --build-dir "build_"@w
      --check-warnings
```

To build examples for other SoCs, please refer to the README of each example.

### Test

Pytest is used to test the built examples. There is a test written for each example. To execute it, just run the following command with replacing target_name and port with your settings:

```bash
pytest --target=<target_name> --port=<port>
```

The examples for Espressif SoC are built for ESP32 and some of them for ESP32-S3 (`esp32_spi_load_ram_example` and `esp32_usb_cdc_acm_example`). Please be aware that tests for ESP32-S3 need to be run separately as `esp32_usb_cdc_acm_example` fails when running after `esp32_spi_load_ram_example`.
