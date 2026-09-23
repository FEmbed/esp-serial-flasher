/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"
#include <unistd.h>

#if CONFIG_SERIAL_FLASHER_PORT_UART
#include "esp32_port.h"
esp_loader_error_t esp32_uart_boot_target(esp32_port_t *p)
{
    if (p->skip_control_pin_init || !GPIO_IS_VALID_OUTPUT_GPIO(p->boot_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(p->reset_pin)) return ESP_LOADER_ERROR_INVALID_PARAM;
    // Release BOOT explicitly; do not inherit an earlier download-mode strap state.
    if (gpio_set_level(p->boot_pin, SERIAL_FLASHER_BOOT_INVERT ? 0 : 1) != ESP_OK ||
        gpio_set_level(p->reset_pin, SERIAL_FLASHER_RESET_INVERT ? 1 : 0) != ESP_OK)
        return ESP_LOADER_ERROR_FAIL;
    usleep(SERIAL_FLASHER_RESET_HOLD_TIME_MS * 1000);
    return gpio_set_level(p->reset_pin, SERIAL_FLASHER_RESET_INVERT ? 0 : 1) == ESP_OK
        ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}
#endif

#if CONFIG_SERIAL_FLASHER_PORT_USB_CDC_ACM
#include "esp32_usb_cdc_acm_port.h"
esp_loader_error_t esp32_usb_cdc_acm_boot_target(esp32_usb_cdc_acm_port_t *p)
{
    if (!esp32_usb_cdc_acm_connected(p)) return ESP_LOADER_ERROR_FAIL;
    // Both native Serial/JTAG and supported bridges use this sequence. A lost release ACK
    // may mean the target has booted; return uncertain and let attestation decide, never retry.
    if (cdc_acm_host_set_control_line_state(p->_acm_device, false, true) != ESP_OK)
        return ESP_LOADER_ERROR_FAIL;
    usleep(SERIAL_FLASHER_RESET_HOLD_TIME_MS * 1000);
    if (!esp32_usb_cdc_acm_connected(p)) return ESP_LOADER_ERROR_FAIL;
    return cdc_acm_host_set_control_line_state(p->_acm_device, false, false) == ESP_OK && esp32_usb_cdc_acm_connected(p)
        ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}
#endif
