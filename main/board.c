/*
 * board.c - Seeed XIAO ESP32-C6 board support.
 *
 * XIAO ESP32-C6 RF switch (per Seeed wiki and the arduino-esp32 XIAO_ESP32C6 variant):
 *   GPIO3  LOW  -> RF switch enabled
 *   GPIO14 LOW  -> on-board ceramic antenna, HIGH -> external U.FL antenna
 * Status LED GPIO15 (active-low), BOOT button GPIO9 (active-low).
 */
#include "board.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "WB_BOARD";

static volatile wb_led_mode_t s_led_mode = WB_LED_OFF;
static volatile int64_t s_identify_until_ms;
static wb_button_cb_t s_on_short;
static wb_button_cb_t s_on_long;

static inline int64_t now_ms(void) { return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

esp_err_t wb_board_rf_init(void)
{
#if CONFIG_WB_RF_SWITCH_EN_GPIO >= 0
    gpio_reset_pin(CONFIG_WB_RF_SWITCH_EN_GPIO);
    gpio_set_direction(CONFIG_WB_RF_SWITCH_EN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_WB_RF_SWITCH_EN_GPIO, 0);
#endif
#if CONFIG_WB_ANT_SELECT_GPIO >= 0
    gpio_reset_pin(CONFIG_WB_ANT_SELECT_GPIO);
    gpio_set_direction(CONFIG_WB_ANT_SELECT_GPIO, GPIO_MODE_OUTPUT);
#if CONFIG_WB_ANTENNA_EXTERNAL
    gpio_set_level(CONFIG_WB_ANT_SELECT_GPIO, 1);
#else
    gpio_set_level(CONFIG_WB_ANT_SELECT_GPIO, 0);
#endif
#endif
    ESP_LOGI(TAG, "RF switch configured, antenna: %s", wb_board_antenna_str());
    return ESP_OK;
}

const char *wb_board_antenna_str(void)
{
#if CONFIG_WB_ANT_SELECT_GPIO < 0
    return "not switched";
#elif CONFIG_WB_ANTENNA_EXTERNAL
    return "external U.FL";
#else
    return "on-board ceramic";
#endif
}

void wb_board_set_led(wb_led_mode_t mode) { s_led_mode = mode; }

void wb_board_identify(uint16_t seconds)
{
    s_identify_until_ms = seconds ? now_ms() + (int64_t)seconds * 1000 : 0;
}

static void led_write(bool on)
{
#if CONFIG_WB_LED_GPIO >= 0
#if CONFIG_WB_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_WB_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_WB_LED_GPIO, on ? 1 : 0);
#endif
#else
    (void)on;
#endif
}

static bool led_pattern(wb_led_mode_t mode, int64_t t)
{
    switch (mode) {
    case WB_LED_IDLE: return (t % 2000) < 80;
    case WB_LED_STEERING: return (t % 200) < 100;
    case WB_LED_JOINED: return true;
    case WB_LED_JOINED_ISOLATED: { int64_t p = t % 2000; return p < 80 || (p >= 240 && p < 320); }
    case WB_LED_ERROR: return (t % 1000) < 500;
    case WB_LED_RESET_ARMED: return (t % 100) < 50;
    case WB_LED_OFF:
    default: return false;
    }
}

static void board_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(20);
    int64_t press_start = -1;
    bool armed = false;

    for (;;) {
        int64_t t = now_ms();

#if CONFIG_WB_BUTTON_GPIO >= 0
        bool pressed = gpio_get_level(CONFIG_WB_BUTTON_GPIO) == 0;
        if (pressed && press_start < 0) {
            press_start = t;
        } else if (pressed && !armed && (t - press_start) >= (int64_t)CONFIG_WB_FACTORY_RESET_HOLD_S * 1000) {
            armed = true;
            ESP_LOGW(TAG, "Button held %ds: release now to RESET network membership", CONFIG_WB_FACTORY_RESET_HOLD_S);
        } else if (!pressed && press_start >= 0) {
            int64_t held = t - press_start;
            press_start = -1;
            if (armed) {
                armed = false;
                if (s_on_long) s_on_long();
            } else if (held >= 50 && held < 2000) {
                if (s_on_short) s_on_short();
            } else if (held >= 2000) {
                ESP_LOGI(TAG, "Button released after %lld ms: ignored (short press <2 s = join, hold %ds = reset)",
                         (long long)held, CONFIG_WB_FACTORY_RESET_HOLD_S);
            }
        }
#endif

        wb_led_mode_t mode = armed ? WB_LED_RESET_ARMED : s_led_mode;
        bool on;
        if (!armed && s_identify_until_ms && t < s_identify_until_ms) on = (t % 500) < 250;
        else on = led_pattern(mode, t);
        led_write(on);

        vTaskDelay(period);
    }
}

esp_err_t wb_board_start(wb_button_cb_t on_short_press, wb_button_cb_t on_long_hold)
{
    s_on_short = on_short_press;
    s_on_long = on_long_hold;

#if CONFIG_WB_LED_GPIO >= 0
    gpio_reset_pin(CONFIG_WB_LED_GPIO);
    gpio_set_direction(CONFIG_WB_LED_GPIO, GPIO_MODE_OUTPUT);
    led_write(false);
#endif
#if CONFIG_WB_BUTTON_GPIO >= 0
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_WB_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
#endif
    BaseType_t ok = xTaskCreate(board_task, "wb_board", 3072, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
