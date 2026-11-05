/*
 * WiserBridge - ESP32-C6 Zigbee router (range extender) for extending an existing
 * Zigbee network such as a Drayton Wiser system.
 *
 * This is a generic Zigbee 3.0 router. It is not a Drayton product and does not
 * claim to be one. Compatibility with the Wiser Heat Hub is UNVERIFIED and is
 * what this firmware is designed to test - see docs/commissioning.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "board.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "serial_cli.h"
#include "wiser_profile.h"
#include "zigbee_router.h"

static const char *TAG = "WB_MAIN";

static void on_button_short(void)
{
    const char *why;
    esp_err_t e = wb_zb_request_join(&why);
    ESP_LOGI(TAG, "BOOT short press -> join: %s (%s)", e == ESP_OK ? "started" : "refused", why);
}

static void on_button_long(void)
{
    ESP_LOGW(TAG, "BOOT long hold -> network reset");
    wb_zb_request_network_reset();
}

void app_main(void)
{
    /* Application NVS only. The Zigbee NVRAM lives in the separate zb_storage partition. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s); erasing application NVS", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(wb_board_rf_init());
    wb_diag_init();
    ESP_ERROR_CHECK(wb_profile_init());
    wb_diag_print_banner();

    ESP_ERROR_CHECK(wb_board_start(on_button_short, on_button_long));
    ESP_ERROR_CHECK(wb_zb_start());
    ESP_ERROR_CHECK(wb_cli_start());
}
