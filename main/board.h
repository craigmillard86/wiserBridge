/*
 * board.h - Seeed XIAO ESP32-C6 board support: RF switch, status LED, BOOT button.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_LED_OFF = 0,
    WB_LED_IDLE,            /* not commissioned: short blink every 2 s */
    WB_LED_STEERING,        /* joining: 5 Hz blink */
    WB_LED_JOINED,          /* joined with router neighbours: solid */
    WB_LED_JOINED_ISOLATED, /* joined, no router neighbour: double blink */
    WB_LED_ERROR,           /* stack error: 1 Hz blink */
    WB_LED_RESET_ARMED,     /* button held long enough to reset: 10 Hz */
} wb_led_mode_t;

typedef void (*wb_button_cb_t)(void);

/* Configure RF switch / antenna select. Must run before the radio starts. */
esp_err_t wb_board_rf_init(void);

/* Start LED + button handling task. */
esp_err_t wb_board_start(wb_button_cb_t on_short_press, wb_button_cb_t on_long_hold);

void wb_board_set_led(wb_led_mode_t mode);

/* Blink for identify for the given number of seconds (0 cancels). */
void wb_board_identify(uint16_t seconds);

const char *wb_board_antenna_str(void);

#ifdef __cplusplus
}
#endif
