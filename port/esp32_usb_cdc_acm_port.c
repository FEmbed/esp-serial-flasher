/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "usb/cdc_acm_host.h"
#include "esp_private/cdc_host_common.h"
#include "usb/vcp_cp210x.h"
#include "usb/vcp_ch34x.h"
#include "esp32_usb_cdc_acm_port.h"

static const char *TAG = "usb_cdc_acm_port";
#define ESP32_USB_CDC_RX_STREAM_BUFFER_SIZE 4096

#if SERIAL_FLASHER_DEBUG_TRACE
static void transfer_debug_print(const uint8_t *data, const uint16_t size, const bool write)
{
    static bool write_prev = false;

    if (write_prev != write) {
        write_prev = write;
        printf("\n--- %s ---\n", write ? "WRITE" : "READ");
    }

    for (uint32_t i = 0; i < size; i++) {
        printf("%02x ", data[i]);
    }
}
#endif

/* ─── internal deinit (shared by ops->deinit and the disconnect handler) ─── */

static void usb_port_deinit_impl(esp32_usb_cdc_acm_port_t *p)
{
    p->acm_host_error_callback        = NULL;
    p->device_disconnected_callback   = NULL;
    p->acm_host_serial_state_callback = NULL;
    p->_is_usb_serial_jtag            = false;

    if (p->_acm_device != NULL) {
        if (cdc_acm_host_close(p->_acm_device) != ESP_OK) {
            ESP_LOGE(TAG, "Could not close device");
        }
        p->_acm_device = NULL;
    }

    if (p->_rx_stream_buffer != NULL) {
        vStreamBufferDelete(p->_rx_stream_buffer);
        p->_rx_stream_buffer = NULL;
    }
    if (p->_rx_stream_storage != NULL) {
        heap_caps_free(p->_rx_stream_storage);
        p->_rx_stream_storage = NULL;
    }
    if (p->_rx_stream_buffer_struct != NULL) {
        heap_caps_free(p->_rx_stream_buffer_struct);
        p->_rx_stream_buffer_struct = NULL;
    }
}

static esp_err_t open_detected_usb_serial_device(uint16_t vid,
                                                 uint16_t pid,
                                                 const cdc_acm_host_device_config_t *dev_config,
                                                 cdc_acm_dev_hdl_t *cdc_hdl_ret,
                                                 bool *is_usb_serial_jtag)
{
    if (vid == ESPRESSIF_VID && pid == ESP_SERIAL_JTAG_PID) {
        esp_err_t err = cdc_acm_host_open(vid, pid, 0, dev_config, cdc_hdl_ret);
        if (err == ESP_OK && is_usb_serial_jtag != NULL) {
            *is_usb_serial_jtag = true;
        }
        return err;
    }
    if (vid == SILICON_LABS_VID) {
        esp_err_t err = cp210x_vcp_open(pid, 0, dev_config, cdc_hdl_ret);
        if (err == ESP_OK && is_usb_serial_jtag != NULL) {
            *is_usb_serial_jtag = false;
        }
        return err;
    }
    if (vid == NANJING_QINHENG_MICROE_VID) {
        esp_err_t err = ch34x_vcp_open(pid, 0, dev_config, cdc_hdl_ret);
        if (err == ESP_OK && is_usb_serial_jtag != NULL) {
            *is_usb_serial_jtag = false;
        }
        return err;
    }

    esp_err_t err = cdc_acm_host_open(vid, pid, 0, dev_config, cdc_hdl_ret);
    if (err == ESP_OK && is_usb_serial_jtag != NULL) {
        *is_usb_serial_jtag = false;
    }
    return err;
}

static bool pid_is_cp210x(uint16_t pid)
{
    return pid == CP210X_PID || pid == CP2105_PID || pid == CP2108_PID;
}

static bool pid_is_ch34x(uint16_t pid)
{
    return pid == CH340_PID || pid == CH340_PID_1 || pid == CH341_PID;
}

static void log_opened_usb_serial_device(cdc_acm_dev_hdl_t cdc_hdl)
{
    const usb_device_desc_t *device_desc = NULL;
    const esp_err_t err =
        usb_host_get_device_descriptor(((cdc_dev_t *)cdc_hdl)->dev_hdl, &device_desc);
    if (err == ESP_OK && device_desc != NULL) {
        ESP_LOGI(TAG,
                 "Opened USB serial candidate vid=0x%04X pid=0x%04X",
                 device_desc->idVendor,
                 device_desc->idProduct);
    }
}

static esp_err_t open_auto_detected_usb_serial_device(const esp32_usb_cdc_acm_port_t *p,
                                                      const cdc_acm_host_device_config_t *dev_config,
                                                      cdc_acm_dev_hdl_t *cdc_hdl_ret,
                                                      bool *is_usb_serial_jtag)
{
    const bool vid_auto = p->device_vid == USB_VID_PID_AUTO_DETECT;
    const bool pid_auto = p->device_pid == USB_VID_PID_AUTO_DETECT;
    const bool fully_auto = vid_auto && pid_auto;
    const uint32_t configured_timeout = dev_config->connection_timeout_ms;
    const uint32_t quick_timeout =
        configured_timeout == 0 ? 500 : (configured_timeout < 500 ? configured_timeout : 500);

    cdc_acm_host_device_config_t probe_config = *dev_config;
    if (fully_auto) {
        probe_config.connection_timeout_ms = quick_timeout;
    }

    if ((vid_auto || p->device_vid == SILICON_LABS_VID) &&
        (pid_auto || pid_is_cp210x(p->device_pid))) {
        const uint16_t pid = pid_auto ? CP210X_PID_AUTO : p->device_pid;
        esp_err_t err = cp210x_vcp_open(pid, 0, &probe_config, cdc_hdl_ret);
        if (err == ESP_OK) {
            if (is_usb_serial_jtag != NULL) {
                *is_usb_serial_jtag = false;
            }
            log_opened_usb_serial_device(*cdc_hdl_ret);
            return ESP_OK;
        }
    }

    if ((vid_auto || p->device_vid == NANJING_QINHENG_MICROE_VID) &&
        (pid_auto || pid_is_ch34x(p->device_pid))) {
        const uint16_t pid = pid_auto ? CH34X_PID_AUTO : p->device_pid;
        esp_err_t err = ch34x_vcp_open(pid, 0, &probe_config, cdc_hdl_ret);
        if (err == ESP_OK) {
            if (is_usb_serial_jtag != NULL) {
                *is_usb_serial_jtag = false;
            }
            log_opened_usb_serial_device(*cdc_hdl_ret);
            return ESP_OK;
        }
    }

    if ((vid_auto || p->device_vid == ESPRESSIF_VID) &&
        (pid_auto || p->device_pid == ESP_SERIAL_JTAG_PID)) {
        const uint16_t pid = pid_auto ? ESP_SERIAL_JTAG_PID : p->device_pid;
        esp_err_t err = cdc_acm_host_open(ESPRESSIF_VID,
                                          pid,
                                          0,
                                          &probe_config,
                                          cdc_hdl_ret);
        if (err == ESP_OK) {
            if (is_usb_serial_jtag != NULL) {
                *is_usb_serial_jtag = true;
            }
            log_opened_usb_serial_device(*cdc_hdl_ret);
            return ESP_OK;
        }
    }

    const uint16_t generic_vid = vid_auto ? CDC_HOST_ANY_VID : p->device_vid;
    const uint16_t generic_pid = pid_auto ? CDC_HOST_ANY_PID : p->device_pid;
    esp_err_t err = cdc_acm_host_open(generic_vid,
                                      generic_pid,
                                      0,
                                      dev_config,
                                      cdc_hdl_ret);
    if (err == ESP_OK) {
        if (is_usb_serial_jtag != NULL) {
            *is_usb_serial_jtag = false;
        }
        log_opened_usb_serial_device(*cdc_hdl_ret);
        return ESP_OK;
    }

    return err;
}
/* ─── USB event / data callbacks ─────────────────────────────────────────── */

static bool handle_usb_data(const uint8_t *data, size_t data_len, void *arg)
{
    esp32_usb_cdc_acm_port_t *p = (esp32_usb_cdc_acm_port_t *)arg;
    size_t sent = xStreamBufferSend(p->_rx_stream_buffer, data, data_len, 0);
    if (sent != data_len) {
        ESP_LOGW(TAG, "RX stream buffer full: dropped %u bytes", (unsigned)(data_len - sent));
        return false;
    }
    return true;
}

static void handle_usb_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    esp32_usb_cdc_acm_port_t *p = (esp32_usb_cdc_acm_port_t *)user_ctx;

    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error has occurred, err_no = %i", event->data.error);
        if (p->acm_host_error_callback != NULL) {
            p->acm_host_error_callback();
        }
        break;

    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device disconnected");
        if (p->device_disconnected_callback != NULL) {
            p->device_disconnected_callback();
        }
        usb_port_deinit_impl(p);
        break;

    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
        if (p->acm_host_serial_state_callback != NULL) {
            p->acm_host_serial_state_callback();
        }
        break;

    default:
        ESP_LOGW(TAG, "Unsupported CDC event: %i", event->type);
        break;
    }
}

/* ─── port ops ───────────────────────────────────────────────────────────── */

static esp_loader_error_t esp32_usb_port_init(esp_loader_port_t *port)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);

    p->_rx_stream_storage = heap_caps_malloc(ESP32_USB_CDC_RX_STREAM_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    p->_rx_stream_buffer_struct = heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p->_rx_stream_storage == NULL || p->_rx_stream_buffer_struct == NULL) {
        ESP_LOGE(TAG, "Could not allocate internal RX stream buffer for USB data reception");
        usb_port_deinit_impl(p);
        return ESP_LOADER_ERROR_FAIL;
    }
    p->_rx_stream_buffer = xStreamBufferCreateStatic(ESP32_USB_CDC_RX_STREAM_BUFFER_SIZE,
                                                     1,
                                                     p->_rx_stream_storage,
                                                     p->_rx_stream_buffer_struct);
    if (p->_rx_stream_buffer == NULL) {
        ESP_LOGE(TAG, "Could not create the internal stream buffer for USB data reception");
        usb_port_deinit_impl(p);
        return ESP_LOADER_ERROR_FAIL;
    }

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = p->connection_timeout_ms,
        .out_buffer_size       = p->out_buffer_size,
        .in_buffer_size        = 512,
        .event_cb              = handle_usb_event,
        .data_cb               = handle_usb_data,
        .user_arg             = p,
    };

    const bool auto_detect = (p->device_vid == USB_VID_PID_AUTO_DETECT ||
                              p->device_pid == USB_VID_PID_AUTO_DETECT);

    if (auto_detect) {
        if (open_auto_detected_usb_serial_device(p,
                                                 &dev_config,
                                                 &p->_acm_device,
                                                 &p->_is_usb_serial_jtag) == ESP_OK) {
            return ESP_LOADER_SUCCESS;
        }
    } else {
        esp_err_t err = open_detected_usb_serial_device(p->device_vid,
                                                        p->device_pid,
                                                        &dev_config,
                                                        &p->_acm_device,
                                                        &p->_is_usb_serial_jtag);
        if (err == ESP_OK) {
            return ESP_LOADER_SUCCESS;
        }
    }

    ESP_LOGE(TAG, "Failed to open any USB device");
    usb_port_deinit_impl(p);
    return ESP_LOADER_ERROR_FAIL;
}

static void esp32_usb_port_deinit(esp_loader_port_t *port)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    usb_port_deinit_impl(p);
}

static void usb_delay_ms_raw(uint32_t ms)
{
    usleep(ms * 1000);
}

static void esp32_usb_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    usb_delay_ms_raw(ms);
}

static void usb_serial_jtag_reset_target(esp32_usb_cdc_acm_port_t *p)
{
    xStreamBufferReset(p->_rx_stream_buffer);
    cdc_acm_host_set_control_line_state(p->_acm_device, false, true);
    usb_delay_ms_raw(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
    cdc_acm_host_set_control_line_state(p->_acm_device, false, false);
}

static void usb_serial_jtag_enter_bootloader(esp32_usb_cdc_acm_port_t *p)
{
    cdc_acm_host_set_control_line_state(p->_acm_device, true, false);
    usb_delay_ms_raw(SERIAL_FLASHER_BOOT_HOLD_TIME_MS);
    cdc_acm_host_set_control_line_state(p->_acm_device, true, true);
    usb_serial_jtag_reset_target(p);
}

static void usb_serial_converter_reset_target(esp32_usb_cdc_acm_port_t *p)
{
    xStreamBufferReset(p->_rx_stream_buffer);
    cdc_acm_host_set_control_line_state(p->_acm_device, false, true);
    usb_delay_ms_raw(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
    cdc_acm_host_set_control_line_state(p->_acm_device, false, false);
}

static void usb_serial_converter_enter_bootloader(esp32_usb_cdc_acm_port_t *p)
{
    xStreamBufferReset(p->_rx_stream_buffer);
    cdc_acm_host_set_control_line_state(p->_acm_device, false, true);
    usb_delay_ms_raw(SERIAL_FLASHER_BOOT_HOLD_TIME_MS);
    cdc_acm_host_set_control_line_state(p->_acm_device, true, false);
    usb_delay_ms_raw(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
    cdc_acm_host_set_control_line_state(p->_acm_device, false, false);
}

static void esp32_usb_enter_bootloader(esp_loader_port_t *port)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    assert(p->_acm_device != NULL && p->_rx_stream_buffer != NULL);

    if (p->_is_usb_serial_jtag) {
        usb_serial_jtag_enter_bootloader(p);
    } else {
        usb_serial_converter_enter_bootloader(p);
    }
}

static void esp32_usb_reset_target(esp_loader_port_t *port)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    assert(p->_acm_device != NULL && p->_rx_stream_buffer != NULL);

    if (p->_is_usb_serial_jtag) {
        usb_serial_jtag_reset_target(p);
    } else {
        usb_serial_converter_reset_target(p);
    }
}

static void esp32_usb_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    p->_time_end = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t esp32_usb_remaining_time(esp_loader_port_t *port)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    int64_t remaining = (p->_time_end - esp_timer_get_time()) / 1000;
    return (remaining > 0) ? (uint32_t)remaining : 0;
}

static void esp32_usb_debug_print(esp_loader_port_t *port, const char *str)
{
    (void)port;
    printf("DEBUG: %s\n", str);
}

static esp_loader_error_t esp32_usb_write(esp_loader_port_t *port, const uint8_t *data, const uint16_t size,
        const uint32_t timeout)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    assert(data != NULL);
    assert(p->_acm_device != NULL && p->_rx_stream_buffer != NULL);

    esp_err_t err = cdc_acm_host_data_tx_blocking(p->_acm_device,
                    (uint8_t *)data, size, timeout);

    if (err == ESP_OK) {
#if SERIAL_FLASHER_DEBUG_TRACE
        transfer_debug_print(data, size, true);
#endif
        return ESP_LOADER_SUCCESS;
    } else if (err == ESP_ERR_TIMEOUT) {
        return ESP_LOADER_ERROR_TIMEOUT;
    } else {
        return ESP_LOADER_ERROR_FAIL;
    }
}

static esp_loader_error_t esp32_usb_read(esp_loader_port_t *port, uint8_t *data, const uint16_t size, const uint32_t timeout)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    assert(data != NULL);
    assert(p->_acm_device != NULL && p->_rx_stream_buffer != NULL);

    size_t received = xStreamBufferReceive(p->_rx_stream_buffer, data, size, pdMS_TO_TICKS(timeout));

    if (received == size) {
#if SERIAL_FLASHER_DEBUG_TRACE
        transfer_debug_print(data, size, false);
#endif
        return ESP_LOADER_SUCCESS;
    } else {
        return ESP_LOADER_ERROR_TIMEOUT;
    }
}

static esp_loader_error_t esp32_usb_change_rate(esp_loader_port_t *port, uint32_t baudrate)
{
    esp32_usb_cdc_acm_port_t *p = container_of(port, esp32_usb_cdc_acm_port_t, port);
    assert(p->_acm_device != NULL && p->_rx_stream_buffer != NULL);

    cdc_acm_line_coding_t line_coding;
    if (cdc_acm_host_line_coding_get(p->_acm_device, &line_coding) != ESP_OK) {
        return ESP_LOADER_ERROR_FAIL;
    }

    line_coding.dwDTERate = baudrate;

    if (cdc_acm_host_line_coding_set(p->_acm_device, &line_coding) != ESP_OK) {
        return ESP_LOADER_ERROR_FAIL;
    }

    return ESP_LOADER_SUCCESS;
}

const esp_loader_port_ops_t esp32_usb_cdc_acm_ops = {
    .init                     = esp32_usb_port_init,
    .deinit                   = esp32_usb_port_deinit,
    .enter_bootloader         = esp32_usb_enter_bootloader,
    .reset_target             = esp32_usb_reset_target,
    .start_timer              = esp32_usb_start_timer,
    .remaining_time           = esp32_usb_remaining_time,
    .delay_ms                 = esp32_usb_delay_ms,
    .debug_print              = esp32_usb_debug_print,
    .change_transmission_rate = esp32_usb_change_rate,
    .write                    = esp32_usb_write,
    .read                     = esp32_usb_read,
};
