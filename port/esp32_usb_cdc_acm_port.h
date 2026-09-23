/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_loader_io.h"
#include "esp_loader.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_VID_PID_AUTO_DETECT     (0)      // Use for VID/PID in config to enable auto-detection

#define ESPRESSIF_VID               (0x303a)
#define ESP_SERIAL_JTAG_PID         (0x1001)

// VID and PID definitions from esp-usb library, only these are supported
#define SILICON_LABS_VID            (0x10C4) // Silicon Labs
#define CP210X_PID                  (0xEA60) // Single i.e. CP2101 - CP2104
#define CP2105_PID                  (0xEA70) // Dual
#define CP2108_PID                  (0xEA71) // Quad

#define NANJING_QINHENG_MICROE_VID  (0x1A86) // Nanjing Qinheng Microelectronics
#define CH340_PID                   (0x7522)
#define CH340_PID_1                 (0x7523)
#define CH341_PID                   (0x5523)

typedef void (*loader_port_esp32_usb_cdc_acm_callback_t)(void);

typedef enum {
    ESP32_USB_CDC_DRIVER_NONE = 0,
    ESP32_USB_CDC_DRIVER_ESP_SERIAL_JTAG,
    ESP32_USB_CDC_DRIVER_CP210X,
    ESP32_USB_CDC_DRIVER_CH34X,
    ESP32_USB_CDC_DRIVER_FTDI,
    ESP32_USB_CDC_DRIVER_GENERIC_ACM,
} esp32_usb_cdc_driver_kind_t;

/**
 * @brief Concrete ESP32 USB CDC-ACM port instance.
 *
 * Declare one of these (with static or longer lifetime than the loader),
 * fill the config fields, then pass &port.port to esp_loader_init_serial().
 * USB device open is called automatically inside esp_loader_init_serial() —
 * no separate init step is needed.
 *
 * One owner task serializes init/I/O/deinit. Callbacks only notify that owner;
 * they must not close/reopen the port or release its storage. After disconnect,
 * the owner must call esp_loader_deinit() before reinitializing the port.
 *
 * @code
 *   esp32_usb_cdc_acm_port_t port = {
 *       .port.ops                    = &esp32_usb_cdc_acm_ops,
 *       .device_vid                  = USB_VID_PID_AUTO_DETECT,
 *       .device_pid                  = USB_VID_PID_AUTO_DETECT,
 *       .connection_timeout_ms       = 1000,
 *       .out_buffer_size             = 4096,
 *       .device_disconnected_callback = my_disconnect_cb,
 *   };
 *   esp_loader_t loader;
 *   esp_loader_init_serial(&loader, &port.port);
 * @endcode
 */
typedef struct {
    esp_loader_port_t port;          /*!< Embedded port base */

    /* Configuration — fill before calling esp_loader_init_serial() */
    uint16_t device_vid;
    uint16_t device_pid;
    uint32_t connection_timeout_ms;
    uint32_t out_buffer_size;        /*!< Must be larger than max USB packet size */
    loader_port_esp32_usb_cdc_acm_callback_t acm_host_error_callback;
    loader_port_esp32_usb_cdc_acm_callback_t device_disconnected_callback;
    loader_port_esp32_usb_cdc_acm_callback_t acm_host_serial_state_callback;

    /* Private runtime state — do not access directly */
    cdc_acm_dev_hdl_t    _acm_device;
    StreamBufferHandle_t _rx_stream_buffer;
    StaticStreamBuffer_t *_rx_stream_buffer_struct;
    uint8_t *_rx_stream_storage;
    bool                 _is_usb_serial_jtag;
    esp32_usb_cdc_driver_kind_t _driver_kind;
    uint16_t             _opened_vid;
    uint16_t             _opened_pid;
    int64_t              _time_end;
    uint32_t            _disconnected;  /*!< Atomic latch, cleared only before opening a new session */
} esp32_usb_cdc_acm_port_t;

/* Latch is shared with the CDC callback; handles/storage belong only to the owner.
 * Kept as a C/C++ compatible integer, accessed exclusively through atomic builtins. */
static inline bool esp32_usb_cdc_acm_connected(const esp32_usb_cdc_acm_port_t *p)
{
    return p && !__atomic_load_n(&p->_disconnected, __ATOMIC_ACQUIRE) &&
           p->_acm_device != NULL && p->_rx_stream_buffer != NULL;
}

static inline void esp32_usb_cdc_acm_disconnected(esp32_usb_cdc_acm_port_t *p)
{
    __atomic_store_n(&p->_disconnected, 1, __ATOMIC_RELEASE);
}

/** Port operations vtable for the ESP32 USB CDC-ACM port. */
extern const esp_loader_port_ops_t esp32_usb_cdc_acm_ops;

/** One checked DTR/RTS normal-boot sequence; no reopen/retry and no claim of target boot health. */
esp_loader_error_t esp32_usb_cdc_acm_boot_target(esp32_usb_cdc_acm_port_t *port);

#ifdef __cplusplus
}
#endif
