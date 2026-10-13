# Configuration Reference

This document provides a comprehensive reference for all configuration options available in ESP Serial Flasher.

## Configuration Types

ESP Serial Flasher supports various configuration options that can be set as **CMake cache variables**. These options control the behavior and features of the library.

## Configuration Options

### Debug and Diagnostics

#### `SERIAL_FLASHER_DEBUG_TRACE`

- **Type**: CMake cache variable / Kconfig (`CONFIG_SERIAL_FLASHER_DEBUG_TRACE`)
- **Default**: Disabled
- **Description**: Enables debug tracing output (transfer data tracing).

### Protocol / Interface Selection

Starting from v2, the communication protocol is **selected at runtime** by calling the appropriate per-protocol init function. There are no compile-time interface flags.

```c
esp_loader_t loader;
esp32_port_t port = { .port.ops = &esp32_uart_ops, /* ... */ };
esp_loader_init_serial(&loader, &port.port);   // UART, USB CDC-ACM, …
```

All init functions accept an `esp_loader_port_t *` (the embedded base of a caller-owned port struct):

| Init function              | Interface   | Notes                                          |
| -------------------------- | ----------- | ---------------------------------------------- |
| `esp_loader_init_serial()` | Serial SLIP | UART, USB CDC-ACM, Linux tty; full feature set |
| `esp_loader_init_spi()`    | SPI         | RAM download only                              |
| `esp_loader_init_sdio()`   | SDIO        | Experimental; limited platform support         |

Functions not supported by a given protocol return `ESP_LOADER_ERROR_UNSUPPORTED_FUNC`.

### Port Compilation (ESP-IDF / Kconfig only)

For ESP-IDF builds, you choose which port implementations to compile into the library. Multiple ports can be enabled simultaneously, enabling runtime switching between interfaces in the same firmware image.

#### `CONFIG_SERIAL_FLASHER_PORT_UART`

- **Type**: Kconfig (`bool`)
- **Default**: Enabled (`y`)
- **Description**: Compile the ESP32 UART port (`esp32_port.c`). Exposes `esp32_uart_ops`.

#### `CONFIG_SERIAL_FLASHER_PORT_SPI`

- **Type**: Kconfig (`bool`)
- **Default**: Disabled
- **Description**: Compile the ESP32 SPI port (`esp32_spi_port.c`). Exposes `esp32_spi_ops`. Only supports RAM download operations.

#### `CONFIG_SERIAL_FLASHER_PORT_SDIO`

- **Type**: Kconfig (`bool`)
- **Default**: Disabled
- **Description**: Compile the ESP32 SDIO port (`esp32_sdio_port.c`). Exposes `esp32_sdio_ops`. Experimental; only available on targets with SDIO host support. SDIO uploads `esp-flasher-stub` during connection and supports the full stub command protocol.

#### `CONFIG_SERIAL_FLASHER_PORT_USB_CDC_ACM`

- **Type**: Kconfig (`bool`)
- **Default**: Disabled
- **Depends on**: `SOC_USB_OTG_SUPPORTED`
- **Description**: Compile the ESP32 USB CDC-ACM port (`esp32_usb_cdc_acm_port.c`). Exposes `esp32_usb_cdc_acm_ops`. Requires the `espressif/usb_host_cdc_acm` managed component.

The port's C configuration `rx_buffer_in_psram` selects RX stream storage in
PSRAM when true, or internal RAM when false (the default for hosts without
PSRAM). Allocation never falls back to a different memory capability. The
stream control block and the CDC driver's 512-byte USB IN staging remain
internal; receive storage lives from successful port initialization until the
owner closes the device and deinitializes the port.

The 4 KiB stub read packet can expand to 8194 bytes in SLIP. With one
unacknowledged packet, the initial command response (at most 22 bytes) and two
conservative baud-change delimiters require 8218 usable bytes. Static FreeRTOS
streams reserve one byte, so storage is 8219 bytes. The stub sends the final MD5
only after the last data ACK; it does not add another simultaneous data window.
This replaces the previous 4096-byte storage: generic internal mode uses 4123
additional internal bytes; PSRAM mode releases those previous 4096 internal
storage bytes and allocates 8219 external bytes. Runtime state and allocator
overhead must be measured separately on the host firmware.

An RX overflow invalidates the I/O session. The callback consumes/discards the
USB transfer and latches the fault; it neither blocks the CDC client task nor
asks the driver to append/retry partially consumed bytes. Read/write adapters
must use `esp32_usb_cdc_acm_io_ready()` before and after I/O. Reset and line
control may still use `esp32_usb_cdc_acm_connected()` for recovery. Flushing or
resetting the stream does not clear the fault; only owner close/reinitialization
starts a new I/O session. Loader reads accumulate short receives using one
deadline, with normal FreeRTOS tick rounding.

### Retry and Timing Configuration

#### `SERIAL_FLASHER_WRITE_BLOCK_RETRIES`

- **Type**: CMake cache variable / Kconfig (`CONFIG_SERIAL_FLASHER_WRITE_BLOCK_RETRIES`)
- **Default**: 3 (used when the macro is not defined at compile time; the library falls back to 3 in `esp_loader.c`)
- **Description**: Number of retry attempts for writing blocks to target flash or RAM.

#### `SERIAL_FLASHER_RESET_HOLD_TIME_MS`

- **Type**: CMake cache variable
- **Default**: 100 (milliseconds)
- **Description**: Duration for which the reset pin is asserted during a hard reset.

#### `SERIAL_FLASHER_BOOT_HOLD_TIME_MS`

- **Type**: CMake cache variable
- **Default**: 50 (milliseconds)
- **Description**: Duration for which the boot pin is asserted during a hard reset.

### GPIO Pin Inversion

#### `SERIAL_FLASHER_RESET_INVERT`

- **Type**: CMake cache variable
- **Default**: Disabled
- **Description**: Invert the output of the reset GPIO pin.
- **Use case**: Useful when hardware has an inverting connection between host and target reset pin.
- **Availability**: UART interface only.

#### `SERIAL_FLASHER_BOOT_INVERT`

- **Type**: CMake cache variable
- **Default**: Disabled
- **Description**: Invert the output of the boot GPIO pin.
- **Use case**: Useful when hardware has an inverting connection between host and target boot pin.
- **Availability**: UART interface only.

## Setting Configuration Options

Configuration options can be set when running CMake using the `-D` flag:

```bash
cmake -DSERIAL_FLASHER_WRITE_BLOCK_RETRIES=5 .. && cmake --build .
```

### Multiple Options Example

```bash
cmake \
  -DSERIAL_FLASHER_WRITE_BLOCK_RETRIES=5 \
  -DSERIAL_FLASHER_RESET_HOLD_TIME_MS=200 \
  .. && cmake --build .
```

### ESP-IDF and Zephyr Configuration

When using ESP-IDF or Zephyr, configuration options can be set using **Kconfig** instead of CMake flags. Many options have corresponding Kconfig names with the `CONFIG_` prefix:

- `SERIAL_FLASHER_DEBUG_TRACE` → `CONFIG_SERIAL_FLASHER_DEBUG_TRACE`
- Port compilation → `CONFIG_SERIAL_FLASHER_PORT_UART`, `CONFIG_SERIAL_FLASHER_PORT_SPI`, etc.

You can configure these options using:

**ESP-IDF:**

```bash
idf.py menuconfig
```

**Zephyr:**

```bash
west build -t menuconfig
```

Alternatively, you can set them directly in your `sdkconfig` (ESP-IDF) or `prj.conf` (Zephyr) files:

```ini
# ESP-IDF sdkconfig or Zephyr prj.conf
CONFIG_SERIAL_FLASHER_DEBUG_TRACE=y
CONFIG_SERIAL_FLASHER_PORT_UART=y
CONFIG_SERIAL_FLASHER_PORT_SPI=y
```

## Platform-Specific Configuration

Some platforms may have additional configuration requirements. See the platform-specific setup guides:

- [STM32 Setup](platform-setup.md#stm32-setup)
- [Zephyr Setup](platform-setup.md#zephyr-setup)
- [Raspberry Pi Pico Setup](platform-setup.md#raspberry-pi-pico-setup)
