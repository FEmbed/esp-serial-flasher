/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb_port_mocks/mock_sdk.h"
#include "esp32_usb_cdc_acm_port.h"
#include "slip.h"

/* Compile the actual port, including its private callback, against SDK doubles. */
#include "../port/esp32_usb_cdc_acm_port.c"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static cdc_acm_host_device_config_t usb_config;
static cdc_dev_t device;
static int64_t now_us;
static unsigned allocations, live_allocations, fail_allocation, closes, tx_calls, controls, receive_calls;
static unsigned allocation_caps[2];
static size_t allocation_sizes[2];
static bool fail_create, fail_open, tx_fault;
static esp32_usb_cdc_acm_port_t *current_port;
typedef struct { unsigned at_ms, size; uint8_t data[16]; unsigned fault; } arrival_t;
static arrival_t arrivals[4];
static unsigned arrival_count, arrival_next;

int64_t esp_timer_get_time(void) { return now_us; }
int mock_usleep(unsigned us) { now_us += us; return 0; }
void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void *heap_caps_malloc(size_t size, unsigned caps)
{
    if (allocations < 2) { allocation_caps[allocations] = caps; allocation_sizes[allocations] = size; }
    if (++allocations == fail_allocation) return NULL;
    void *p = malloc(size);
    if (p) ++live_allocations;
    return p;
}
void heap_caps_free(void *p) { if (p) { --live_allocations; free(p); } }

/* FreeRTOS static stream contract: length includes one reserved empty/full byte.
 * This deterministic single-thread model is not a scheduler, ISR, or DMA test. */
StreamBufferHandle_t xStreamBufferCreateStatic(size_t length, size_t trigger, uint8_t *storage, StaticStreamBuffer_t *meta)
{
    CHECK(trigger == 1);
    if (fail_create) return NULL;
    *meta = (StaticStreamBuffer_t){.storage = storage, .length = length};
    return meta;
}
static size_t queued(StreamBufferHandle_t s) { return (s->head + s->length - s->tail) % s->length; }
size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t size, TickType_t ticks)
{
    CHECK(ticks == 0); /* A CDC callback must never wait for the consumer. */
    size_t room = s->length - 1 - queued(s), count = size < room ? size : room;
    for (size_t i = 0; i < count; ++i) { s->storage[s->head] = ((const uint8_t *)data)[i]; s->head = (s->head + 1) % s->length; }
    return count;
}
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t size, TickType_t ticks)
{
    ++receive_calls;
    if (!queued(s)) {
        int64_t end = now_us + (int64_t)ticks * 1000;
        if (arrival_next < arrival_count && (int64_t)arrivals[arrival_next].at_ms * 1000 <= end) {
            arrival_t *a = &arrivals[arrival_next++];
            now_us = (int64_t)a->at_ms * 1000;
            if (a->fault == 1) esp32_usb_cdc_acm_disconnected(current_port);
            if (a->fault == 2) __atomic_store_n(&current_port->_rx_fault, 1, __ATOMIC_RELEASE);
            CHECK(usb_config.data_cb(a->data, a->size, usb_config.user_arg));
        } else {
            now_us = end;
        }
    }
    size_t count = queued(s) < size ? queued(s) : size;
    for (size_t i = 0; i < count; ++i) { ((uint8_t *)data)[i] = s->storage[s->tail]; s->tail = (s->tail + 1) % s->length; }
    return count;
}
void vStreamBufferDelete(StreamBufferHandle_t s) { (void)s; }
void xStreamBufferReset(StreamBufferHandle_t s) { s->head = s->tail = 0; }

esp_err_t cdc_acm_host_open(uint16_t vid, uint16_t pid, uint8_t interface,
                          const cdc_acm_host_device_config_t *config, cdc_acm_dev_hdl_t *handle)
{
    (void)vid; (void)pid; (void)interface;
    usb_config = *config;
    CHECK(config->in_buffer_size == 512); /* DMA staging does not grow. */
    if (fail_open) return ESP_FAIL;
    *handle = &device;
    return ESP_OK;
}
esp_err_t cp210x_vcp_open(uint16_t pid, uint8_t i, const cdc_acm_host_device_config_t *c, cdc_acm_dev_hdl_t *h) { return cdc_acm_host_open(0, pid, i, c, h); }
esp_err_t ch34x_vcp_open(uint16_t pid, uint8_t i, const cdc_acm_host_device_config_t *c, cdc_acm_dev_hdl_t *h) { return cdc_acm_host_open(0, pid, i, c, h); }
esp_err_t ftdi_vcp_open(uint16_t pid, uint8_t i, const cdc_acm_host_device_config_t *c, cdc_acm_dev_hdl_t *h) { return cdc_acm_host_open(0, pid, i, c, h); }
esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t handle)
{
    CHECK(handle == &device && live_allocations == 2);
    CHECK(current_port->_rx_stream_buffer && current_port->_rx_stream_storage);
    cdc_acm_host_dev_event_data_t event = {.type = CDC_ACM_HOST_DEVICE_DISCONNECTED};
    usb_config.event_cb(&event, usb_config.user_arg); /* Callback before owner frees storage. */
    ++closes;
    return ESP_OK;
}
esp_err_t usb_host_get_device_descriptor(void *handle, const usb_device_desc_t **desc)
{
    (void)handle;
    static const usb_device_desc_t d = {.idVendor = SILICON_LABS_VID, .idProduct = CP210X_PID};
    *desc = &d;
    return ESP_OK;
}
esp_err_t cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t handle, uint8_t *data, size_t size, uint32_t timeout)
{
    (void)handle; (void)data; (void)size; (void)timeout;
    ++tx_calls;
    if (tx_fault) __atomic_store_n(&current_port->_rx_fault, 1, __ATOMIC_RELEASE);
    return ESP_OK;
}
esp_err_t cdc_acm_host_set_control_line_state(cdc_acm_dev_hdl_t h, bool dtr, bool rts) { (void)h; (void)dtr; (void)rts; ++controls; return ESP_OK; }
esp_err_t cdc_acm_host_line_coding_get(cdc_acm_dev_hdl_t h, cdc_acm_line_coding_t *l) { (void)h; l->dwDTERate = 115200; return ESP_OK; }
esp_err_t cdc_acm_host_line_coding_set(cdc_acm_dev_hdl_t h, const cdc_acm_line_coding_t *l) { (void)h; (void)l; return ESP_OK; }

static void prepare(esp32_usb_cdc_acm_port_t *p, bool psram)
{
    CHECK(live_allocations == 0);
    allocations = fail_allocation = closes = tx_calls = controls = receive_calls = 0;
    fail_create = fail_open = tx_fault = false;
    now_us = 0; arrival_count = arrival_next = 0;
    *p = (esp32_usb_cdc_acm_port_t){.port.ops = &esp32_usb_cdc_acm_ops, .device_vid = SILICON_LABS_VID,
                                  .device_pid = CP210X_PID, .out_buffer_size = 4096, .rx_buffer_in_psram = psram};
    current_port = p;
}
static void init(esp32_usb_cdc_acm_port_t *p, bool psram)
{
    prepare(p, psram);
    CHECK(p->port.ops->init(&p->port) == ESP_LOADER_SUCCESS);
    CHECK(esp32_usb_cdc_acm_io_ready(p));
}
static void close_port(esp32_usb_cdc_acm_port_t *p)
{
    p->port.ops->deinit(&p->port);
    CHECK(!p->_acm_device && !p->_rx_stream_buffer && !p->_rx_stream_storage && !p->_rx_stream_buffer_struct);
    CHECK(live_allocations == 0 && !esp32_usb_cdc_acm_connected(p));
}
static void allocation_test(void)
{
    esp32_usb_cdc_acm_port_t p;
    for (unsigned psram = 0; psram < 2; ++psram) {
        init(&p, psram);
        CHECK(allocation_sizes[0] == 8219 && allocation_sizes[1] == sizeof(StaticStreamBuffer_t));
        CHECK(allocation_caps[0] == ((psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT));
        CHECK(allocation_caps[1] == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        CHECK(p.port.ops->init(&p.port) == ESP_LOADER_ERROR_INVALID_PARAM);
        close_port(&p);
        for (unsigned failure = 1; failure <= 4; ++failure) {
            prepare(&p, psram);
            fail_allocation = failure <= 2 ? failure : 0;
            fail_create = failure == 3; fail_open = failure == 4;
            CHECK(p.port.ops->init(&p.port) == ESP_LOADER_ERROR_FAIL);
            CHECK(allocations == 2 && live_allocations == 0); /* No retry with internal RX storage. */
            CHECK(!p._rx_stream_buffer && !p._rx_stream_storage && !p._rx_stream_buffer_struct && !p._acm_device);
        }
    }
}

static uint8_t wire[9000];
static size_t wire_size;
static esp_loader_error_t encode_write(esp_loader_port_t *p, const uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)p; (void)timeout; CHECK(wire_size + size <= sizeof(wire));
    memcpy(wire + wire_size, data, size); wire_size += size; return ESP_LOADER_SUCCESS;
}
static uint32_t encode_time(esp_loader_port_t *p) { (void)p; return 1000; }
static void encode_packet(const uint8_t *data, size_t size)
{
    static const esp_loader_port_ops_t ops = {.write = encode_write, .remaining_time = encode_time};
    esp_loader_port_t port = {.ops = &ops}; esp_loader_t loader = {._port = &port};
    CHECK(SLIP_send_delimiter(&loader) == ESP_LOADER_SUCCESS);
    CHECK(SLIP_send(&loader, data, size) == ESP_LOADER_SUCCESS);
    CHECK(SLIP_send_delimiter(&loader) == ESP_LOADER_SUCCESS);
}
static void deliver_burst(void)
{
    for (size_t offset = 0; offset < wire_size; offset += 512) {
        size_t chunk = wire_size - offset < 512 ? wire_size - offset : 512;
        CHECK(usb_config.data_cb(wire + offset, chunk, usb_config.user_arg));
    }
}
static void full_window_test(void)
{
    esp32_usb_cdc_acm_port_t p; init(&p, true);
    uint8_t response[10], payload[4096], decoded[4096];
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = i % 2 ? 0xDB : 0xC0;
    memset(response, 0xC0, sizeof(response));
    wire_size = 0; encode_packet(response, sizeof(response)); wire[wire_size++] = 0xC0; wire[wire_size++] = 0xC0;
    encode_packet(payload, sizeof(payload));
    CHECK(wire_size == 8218);
    deliver_burst(); /* No consumer scheduling until the entire worst-case wire window is queued. */
    CHECK(!p._rx_fault && queued(p._rx_stream_buffer) == 8218);
    esp_loader_t loader = {._port = &p.port}; p.port.ops->start_timer(&p.port, 1000);
    size_t decoded_size;
    CHECK(SLIP_receive_packet(&loader, decoded, sizeof(response), &decoded_size) == ESP_LOADER_SUCCESS);
    CHECK(decoded_size == sizeof(response) && !memcmp(decoded, response, sizeof(response)));
    CHECK(SLIP_receive_packet(&loader, decoded, sizeof(decoded), &decoded_size) == ESP_LOADER_SUCCESS);
    CHECK(decoded_size == sizeof(payload) && !memcmp(decoded, payload, sizeof(payload)) && queued(p._rx_stream_buffer) == 0);
    /* Repeated ACK-separated data windows exercise ring wraparound without growth. */
    for (unsigned packet = 0; packet < 32; ++packet) {
        wire_size = 0; encode_packet(payload, sizeof(payload)); deliver_burst();
        CHECK(SLIP_receive_packet(&loader, decoded, sizeof(decoded), &decoded_size) == ESP_LOADER_SUCCESS);
        CHECK(decoded_size == sizeof(payload) && !memcmp(decoded, payload, sizeof(payload)));
        uint32_t ack = (packet + 1) * sizeof(payload);
        CHECK(SLIP_send_delimiter(&loader) == ESP_LOADER_SUCCESS && SLIP_send(&loader, (uint8_t *)&ack, sizeof(ack)) == ESP_LOADER_SUCCESS && SLIP_send_delimiter(&loader) == ESP_LOADER_SUCCESS);
    }
    uint8_t md5[16]; memset(md5, 0xDB, sizeof(md5)); wire_size = 0; encode_packet(md5, sizeof(md5)); deliver_burst();
    CHECK(SLIP_receive_packet(&loader, decoded, sizeof(md5), &decoded_size) == ESP_LOADER_SUCCESS);
    CHECK(decoded_size == sizeof(md5) && !memcmp(decoded, md5, sizeof(md5)) && !p._rx_fault);
    close_port(&p);
}
static void fault_test(void)
{
    esp32_usb_cdc_acm_port_t p; init(&p, true);
    /* Same real callback, old FreeRTOS length: an unescaped 4KiB packet already cannot fit. */
    p._rx_stream_buffer = xStreamBufferCreateStatic(4096, 1, p._rx_stream_storage, p._rx_stream_buffer_struct);
    uint8_t payload[4096]; memset(payload, 0x55, sizeof(payload));
    wire_size = 0; encode_packet(payload, sizeof(payload)); CHECK(wire_size == 4098); deliver_burst();
    CHECK(p._rx_fault && !esp32_usb_cdc_acm_io_ready(&p) && esp32_usb_cdc_acm_connected(&p));
    size_t count = queued(p._rx_stream_buffer);
    CHECK(usb_config.data_cb(wire, 512, usb_config.user_arg) && queued(p._rx_stream_buffer) == count);
    uint8_t byte = 0;
    CHECK(p.port.ops->read(&p.port, &byte, 1, 1000) == ESP_LOADER_ERROR_FAIL && receive_calls == 0);
    CHECK(p.port.ops->write(&p.port, &byte, 1, 1000) == ESP_LOADER_ERROR_FAIL && tx_calls == 0);
    p.port.ops->reset_target(&p.port);
    CHECK(controls == 2 && p._rx_fault && !esp32_usb_cdc_acm_io_ready(&p));
    close_port(&p); CHECK(p._rx_fault);
    CHECK(p.port.ops->init(&p.port) == ESP_LOADER_SUCCESS && !p._rx_fault); close_port(&p);
}
static void exact_read_test(void)
{
    esp32_usb_cdc_acm_port_t p; init(&p, true);
    arrivals[0] = (arrival_t){.at_ms = 2, .size = 2, .data = {1, 2}};
    arrivals[1] = (arrival_t){.at_ms = 7, .size = 1, .data = {3}};
    arrivals[2] = (arrival_t){.at_ms = 9, .size = 3, .data = {4, 5, 6}}; arrival_count = 3;
    uint8_t bytes[6]; CHECK(p.port.ops->read(&p.port, bytes, 6, 10) == ESP_LOADER_SUCCESS);
    CHECK(!memcmp(bytes, "\1\2\3\4\5\6", 6) && now_us == 9000 && receive_calls == 3);
    close_port(&p);
    init(&p, true);
    arrivals[0] = (arrival_t){.at_ms = 8, .size = 1, .data = {1}};
    arrivals[1] = (arrival_t){.at_ms = 16, .size = 1, .data = {2}}; arrival_count = 2;
    CHECK(p.port.ops->read(&p.port, bytes, 2, 10) == ESP_LOADER_ERROR_TIMEOUT);
    CHECK(now_us == 10000 && arrival_next == 1 && bytes[0] == 1); /* Partial arrival did not extend deadline. */
    close_port(&p);
    for (unsigned fault = 1; fault <= 2; ++fault) {
        init(&p, true); arrivals[0] = (arrival_t){.at_ms = 2, .size = 1, .data = {1}, .fault = fault}; arrival_count = 1;
        CHECK(p.port.ops->read(&p.port, bytes, 1, 10) == ESP_LOADER_ERROR_FAIL && now_us == 2000); close_port(&p);
    }
    init(&p, true);
    CHECK(p.port.ops->read(&p.port, bytes, 0, 0) == ESP_LOADER_SUCCESS);
    CHECK(usb_config.data_cb((uint8_t *)"a", 1, usb_config.user_arg));
    CHECK(p.port.ops->read(&p.port, bytes, 1, 0) == ESP_LOADER_SUCCESS && bytes[0] == 'a');
    CHECK(p.port.ops->read(&p.port, bytes, 1, 0) == ESP_LOADER_ERROR_TIMEOUT);
    tx_fault = true; CHECK(p.port.ops->write(&p.port, bytes, 1, 10) == ESP_LOADER_ERROR_FAIL && tx_calls == 1); close_port(&p);
}
int main(void)
{
    allocation_test(); full_window_test(); fault_test(); exact_read_test();
    puts("{\"result\":\"PASS\",\"groups\":[\"allocation-policy-and-cleanup\",\"SLIP-starved-consumer-full-window-and-wrap\",\"old-window-loss-fault-recovery-lifecycle\",\"exact-read-single-deadline-and-inflight-fault\"]}");
    return 0;
}
