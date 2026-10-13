/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host doubles model the documented SDK contracts, not USB scheduling or DMA. */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1U
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {
    uint8_t *storage;
    size_t length, head, tail;
} StaticStreamBuffer_t;
typedef StaticStreamBuffer_t *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreateStatic(size_t, size_t, uint8_t *, StaticStreamBuffer_t *);
size_t xStreamBufferSend(StreamBufferHandle_t, const void *, size_t, TickType_t);
size_t xStreamBufferReceive(StreamBufferHandle_t, void *, size_t, TickType_t);
void vStreamBufferDelete(StreamBufferHandle_t);
void xStreamBufferReset(StreamBufferHandle_t);
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
int64_t esp_timer_get_time(void);
int mock_usleep(unsigned);
#define usleep mock_usleep
void mock_log(const char *, const char *, ...);
#define ESP_LOGE(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) mock_log(tag, __VA_ARGS__)

typedef struct { uint16_t idVendor, idProduct; } usb_device_desc_t;
typedef struct { void *dev_hdl; } cdc_dev_t;
typedef cdc_dev_t *cdc_acm_dev_hdl_t;
typedef struct { uint32_t dwDTERate; } cdc_acm_line_coding_t;
enum { CDC_ACM_HOST_ERROR, CDC_ACM_HOST_DEVICE_DISCONNECTED, CDC_ACM_HOST_SERIAL_STATE };
typedef struct {
    unsigned type;
    union { int error; struct { uint16_t val; } serial_state; } data;
} cdc_acm_host_dev_event_data_t;
typedef struct {
    uint32_t connection_timeout_ms, out_buffer_size, in_buffer_size;
    void (*event_cb)(const cdc_acm_host_dev_event_data_t *, void *);
    bool (*data_cb)(const uint8_t *, size_t, void *);
    void *user_arg;
} cdc_acm_host_device_config_t;
#define CDC_HOST_ANY_VID 0
#define CDC_HOST_ANY_PID 0
#define CP210X_PID_AUTO 0
#define CH34X_PID_AUTO 0
#define FTDI_PID_AUTO 0
#define FTDI_VID 0x0403
#define FT232_PID 0x6001
#define FT231_PID 0x6015
esp_err_t cdc_acm_host_open(uint16_t, uint16_t, uint8_t, const cdc_acm_host_device_config_t *, cdc_acm_dev_hdl_t *);
esp_err_t cp210x_vcp_open(uint16_t, uint8_t, const cdc_acm_host_device_config_t *, cdc_acm_dev_hdl_t *);
esp_err_t ch34x_vcp_open(uint16_t, uint8_t, const cdc_acm_host_device_config_t *, cdc_acm_dev_hdl_t *);
esp_err_t ftdi_vcp_open(uint16_t, uint8_t, const cdc_acm_host_device_config_t *, cdc_acm_dev_hdl_t *);
esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t);
esp_err_t usb_host_get_device_descriptor(void *, const usb_device_desc_t **);
esp_err_t cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t, uint8_t *, size_t, uint32_t);
esp_err_t cdc_acm_host_set_control_line_state(cdc_acm_dev_hdl_t, bool, bool);
esp_err_t cdc_acm_host_line_coding_get(cdc_acm_dev_hdl_t, cdc_acm_line_coding_t *);
esp_err_t cdc_acm_host_line_coding_set(cdc_acm_dev_hdl_t, const cdc_acm_line_coding_t *);
