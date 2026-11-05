/*
 * WiserBridge sniffer - passive IEEE 802.15.4 capture for the ESP32-C6.
 *
 * This is a SEPARATE firmware from the router. It never transmits (no ACKs, no
 * beacons, no joins): the radio is put in promiscuous receive-only mode.
 *
 * Output (USB Serial/JTAG, text, one frame per line):
 *   #WBSNIFF v1 ch=<channel>                         header (on start / channel change)
 *   F <ch> <rssi_dBm> <lqi> <timestamp_us> <hex>     frame (MAC header + payload, NO FCS)
 *   #DROP <n>                                        frames lost because the queue overflowed
 * Input (commands, newline terminated):
 *   c <11..26>   set channel
 *   s            print status
 *
 * The ESP32-C6 radio validates the FCS in hardware and replaces the two FCS
 * bytes with RSSI/LQI (see esp_ieee802154.h), so only CRC-valid frames are
 * reported and the FCS is not forwarded. tools/sniff_to_pcap.py converts the
 * stream to a pcap for Wireshark.
 *
 * Encrypted Zigbee NWK/APS payloads remain encrypted. Nothing here bypasses
 * Zigbee security.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define QUEUE_DEPTH 64
#define DEFAULT_CHANNEL 11

/* XIAO ESP32-C6 RF switch: GPIO3 LOW enables, GPIO14 LOW selects on-board antenna. */
#define RF_SWITCH_EN_GPIO 3
#define ANT_SELECT_GPIO 14

typedef struct {
    uint8_t len;
    uint8_t ch;
    int8_t rssi;
    uint8_t lqi;
    uint64_t ts;
    uint8_t data[127];
} frame_t;

static QueueHandle_t s_q;
static volatile uint32_t s_drops;
static volatile uint32_t s_rx;
static volatile uint8_t s_channel = DEFAULT_CHANNEL;

static void IRAM_ATTR rx_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    frame_t f;
    uint8_t phr = frame[0] & 0x7F;
    /* PHR length includes the 2 FCS bytes (replaced by RSSI/LQI); drop them. */
    f.len = phr >= 2 ? (uint8_t)(phr - 2) : 0;
    if (f.len > sizeof(f.data)) f.len = sizeof(f.data);
    memcpy(f.data, frame + 1, f.len);
    f.ch = info->channel;
    f.rssi = info->rssi;
    f.lqi = info->lqi;
    f.ts = info->timestamp;
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_q, &f, &woken) != pdTRUE) s_drops++;
    else s_rx++;
    esp_ieee802154_receive_handle_done(frame);
    if (woken) portYIELD_FROM_ISR();
}

static void set_channel(uint8_t ch)
{
    s_channel = ch;
    esp_ieee802154_set_channel(ch);
    esp_ieee802154_receive();
    printf("#WBSNIFF v1 ch=%u\n", ch);
    fflush(stdout);
}

static void output_task(void *arg)
{
    (void)arg;
    frame_t f;
    char line[16 + 4 + 6 + 4 + 22 + 2 * 127 + 4];
    uint32_t last_drops = 0;
    for (;;) {
        if (xQueueReceive(s_q, &f, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int n = snprintf(line, sizeof(line), "F %u %d %u %" PRIu64 " ", f.ch, f.rssi, f.lqi, f.ts);
            for (int i = 0; i < f.len && n < (int)sizeof(line) - 3; ++i) {
                static const char hex[] = "0123456789abcdef";
                line[n++] = hex[f.data[i] >> 4];
                line[n++] = hex[f.data[i] & 0xF];
            }
            line[n++] = '\n';
            line[n] = '\0';
            fputs(line, stdout);
        }
        if (s_drops != last_drops) {
            last_drops = s_drops;
            printf("#DROP %" PRIu32 "\n", last_drops);
        }
        fflush(stdout);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    char buf[32];
    for (;;) {
        if (!fgets(buf, sizeof(buf), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (buf[0] == 'c') {
            int ch = atoi(buf + 1);
            if (ch >= 11 && ch <= 26) set_channel((uint8_t)ch);
            else printf("#ERR channel must be 11..26\n");
        } else if (buf[0] == 's') {
            printf("#STATUS ch=%u rx=%" PRIu32 " drops=%" PRIu32 " uptime_ms=%" PRId64 "\n", s_channel, s_rx, s_drops,
                   esp_timer_get_time() / 1000);
        }
        fflush(stdout);
    }
}

void app_main(void)
{
    gpio_reset_pin(RF_SWITCH_EN_GPIO);
    gpio_set_direction(RF_SWITCH_EN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RF_SWITCH_EN_GPIO, 0);
    gpio_reset_pin(ANT_SELECT_GPIO);
    gpio_set_direction(ANT_SELECT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ANT_SELECT_GPIO, 0);

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    cfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);

    s_q = xQueueCreate(QUEUE_DEPTH, sizeof(frame_t));

    esp_ieee802154_event_cb_list_t cbs = {.rx_done_cb = rx_done};
    ESP_ERROR_CHECK(esp_ieee802154_event_callback_list_register(cbs));
    ESP_ERROR_CHECK(esp_ieee802154_enable());
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_coordinator(false);
    esp_ieee802154_set_rx_when_idle(true);

    xTaskCreate(output_task, "sniff_out", 4096, NULL, 5, NULL);
    xTaskCreate(input_task, "sniff_in", 3072, NULL, 4, NULL);

    printf("#WBSNIFF passive 802.15.4 sniffer (receive-only, promiscuous). Commands: 'c <ch>', 's'\n");
    set_channel(DEFAULT_CHANNEL);
}
