/*
 * serial_cli.c - USB serial command interface.
 *
 * Commands never fabricate data: anything the SDK cannot provide is reported
 * as unavailable. The only destructive command ("reset") needs a one-time code.
 */
#include "serial_cli.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_logic.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wiser_profile.h"
#include "zigbee_router.h"

static const char *TAG = "WB_CLI";

static uint32_t s_reset_code;
static int64_t s_reset_code_expiry_us;

#define REGISTER(name, fn, hint_s, help_s)                                    \
    do {                                                                      \
        const esp_console_cmd_t c = {.command = name, .help = help_s,         \
                                     .hint = hint_s, .func = fn};             \
        ESP_ERROR_CHECK(esp_console_cmd_register(&c));                        \
    } while (0)

static int cmd_status(int argc, char **argv) { wb_diag_cmd_status(); return 0; }
static int cmd_network(int argc, char **argv) { wb_diag_cmd_network(); return 0; }
static int cmd_neighbors(int argc, char **argv) { wb_diag_cmd_neighbors(); return 0; }
static int cmd_routes(int argc, char **argv) { wb_diag_cmd_routes(); return 0; }
static int cmd_children(int argc, char **argv) { wb_diag_cmd_children(); return 0; }
static int cmd_diagnostics(int argc, char **argv) { wb_diag_cmd_diagnostics(); return 0; }
static int cmd_events(int argc, char **argv) { wb_diag_cmd_events(); return 0; }
static int cmd_multihop(int argc, char **argv) { wb_diag_cmd_multihop(); return 0; }

static int cmd_health(int argc, char **argv)
{
    if (!wb_zb_lock(3000)) { printf("ERROR: stack busy\n"); return 1; }
    wb_diag_print_health();
    wb_zb_unlock();
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *a = esp_app_get_description();
    printf("%s %s, ESP-IDF %s, built %s %s\n", a->project_name, a->version, a->idf_ver, a->date, a->time);
    return 0;
}

static int cmd_join(int argc, char **argv)
{
    const char *why;
    esp_err_t e = wb_zb_request_join(&why);
    printf("%s: %s\n", e == ESP_OK ? "OK" : "REFUSED", why);
    if (e == ESP_OK) printf("Make sure the Wiser hub is in pairing mode. Progress is logged; 'status' shows the verdict.\n");
    return e == ESP_OK ? 0 : 1;
}

static int cmd_scan(int argc, char **argv)
{
    bool force = argc > 1 && strcmp(argv[1], "force") == 0;
    const char *why;
    esp_err_t e = wb_diag_cmd_scan(force, &why);
    printf("%s: %s\n", e == ESP_OK ? "OK" : "REFUSED", why);
    return e == ESP_OK ? 0 : 1;
}

static int addr_arg(int argc, char **argv, uint16_t *out)
{
    if (argc < 2) { printf("usage: %s <0xSHORT|IEEE>\n", argv[0]); return 1; }
    if (!wb_diag_resolve_addr(argv[1], out)) {
        printf("ERROR: '%s' is not a short address (0x1234) or a known IEEE address\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    uint16_t a;
    if (addr_arg(argc, argv, &a)) return 1;
    uint8_t count = argc > 2 ? (uint8_t)atoi(argv[2]) : 3;
    const char *why;
    esp_err_t e = wb_diag_cmd_ping(a, count, &why);
    if (e != ESP_OK) printf("REFUSED: %s\n", why);
    return e == ESP_OK ? 0 : 1;
}

static int cmd_lqi(int argc, char **argv)
{
    uint16_t a;
    if (addr_arg(argc, argv, &a)) return 1;
    const char *why;
    esp_err_t e = wb_diag_cmd_lqi(a, &why);
    printf("%s: %s\n", e == ESP_OK ? "OK" : "REFUSED", why);
    return e == ESP_OK ? 0 : 1;
}

static int cmd_interview(int argc, char **argv)
{
    uint16_t a;
    if (addr_arg(argc, argv, &a)) return 1;
    const char *why;
    esp_err_t e = wb_diag_cmd_interview(a, &why);
    if (e != ESP_OK) printf("REFUSED: %s\n", why);
    return e == ESP_OK ? 0 : 1;
}

static int cmd_profile(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "show";
    if (strcmp(sub, "show") == 0) {
        wb_profile_print(wb_profile_get(), "ACTIVE PROFILE (registered with the stack at boot)");
        if (memcmp(wb_profile_get(), wb_profile_pending(), sizeof(wb_profile_t)) != 0)
            printf("(there are unsaved/unapplied edits: 'profile pending')\n");
        return 0;
    }
    if (strcmp(sub, "pending") == 0) {
        wb_profile_print(wb_profile_pending(), "PENDING PROFILE (applies after 'profile save' + reboot)");
        return 0;
    }
    if (strcmp(sub, "set") == 0) {
        if (argc < 4) { printf("usage: profile set <key> <value>\n"); return 1; }
        /* Re-join remaining args so values may contain spaces. */
        char value[96] = "";
        for (int i = 3; i < argc; ++i) {
            if (i > 3) strlcat(value, " ", sizeof(value));
            strlcat(value, argv[i], sizeof(value));
        }
        const char *why;
        esp_err_t e = wb_profile_set(argv[2], value, &why);
        printf("%s: %s\n", e == ESP_OK ? "OK" : "ERROR", why);
        return e == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "save") == 0) {
        esp_err_t e = wb_profile_save();
        printf("%s\n", e == ESP_OK ? "Saved. Reboot to apply. If already joined, the hub keeps its old view of this "
                                     "device until it is reset and re-joined."
                                   : esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "defaults") == 0) {
        esp_err_t e = wb_profile_reset_to_defaults();
        printf("%s\n", e == ESP_OK ? "NVS override removed. Kconfig defaults apply after reboot." : esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    printf("usage: profile [show|pending|set <key> <value>|save|defaults]\n");
    return 1;
}

static int cmd_epid(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "show") == 0) {
        uint8_t e[8];
        char s[24];
        if (wb_opt_get_epid_filter(e)) { wb_format_ieee(e, s, sizeof(s)); printf("EPID join filter: %s\n", s); }
        else printf("EPID join filter: none (joins any open network on the configured channels)\n");
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        esp_err_t e = wb_opt_set_epid_filter(NULL);
        printf("%s\n", e == ESP_OK ? "Cleared (applies to the next join)" : esp_err_to_name(e));
        return 0;
    }
    uint8_t epid[8];
    if (!wb_parse_ieee(argv[1], epid)) { printf("ERROR: expected 16 hex digits, e.g. aa:bb:cc:dd:ee:ff:00:11\n"); return 1; }
    esp_err_t e = wb_opt_set_epid_filter(epid);
    printf("%s\n", e == ESP_OK ? "Saved. The next 'join' only targets this extended PAN ID." : esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}

static int cmd_channels(int argc, char **argv)
{
    char s[64];
    if (argc < 2) {
        wb_format_channel_mask(wb_zb_effective_channel_mask(), s, sizeof(s));
        printf("Join channels: %s (mask 0x%08" PRIx32 ")%s\n", s, wb_zb_effective_channel_mask(),
               wb_opt_get_channel_mask() ? " [NVS override]" : " [Kconfig]");
        return 0;
    }
    uint32_t mask = 0;
    if (strcmp(argv[1], "default") == 0) {
        mask = 0;
    } else if (strncmp(argv[1], "0x", 2) == 0) {
        mask = strtoul(argv[1], NULL, 16);
        if ((mask & ~0x07FFF800u) || !mask) { printf("ERROR: mask must select channels 11..26 only\n"); return 1; }
    } else {
        char tmp[64];
        strlcpy(tmp, argv[1], sizeof(tmp));
        char *save = NULL;
        for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            int ch = atoi(t);
            if (ch < 11 || ch > 26) { printf("ERROR: channel %s out of range 11..26\n", t); return 1; }
            mask |= 1ul << ch;
        }
    }
    esp_err_t e = wb_opt_set_channel_mask(mask);
    printf("%s\n", e == ESP_OK ? "Saved. Reboot to apply (affects joining only; a joined device follows the network)."
                               : esp_err_to_name(e));
    return 0;
}

static int cmd_tclk(int argc, char **argv)
{
    if (argc < 2) {
        printf("TCLK exchange required: %s%s\n", wb_zb_effective_tclk_required() ? "on" : "off",
               wb_opt_get_tclk_required() < 0 ? " [Kconfig]" : " [NVS override]");
        return 0;
    }
    int v = strcmp(argv[1], "on") == 0 ? 1 : strcmp(argv[1], "off") == 0 ? 0 : strcmp(argv[1], "default") == 0 ? -1 : -2;
    if (v == -2) { printf("usage: tclk [on|off|default]\n"); return 1; }
    esp_err_t e = wb_opt_set_tclk_required(v);
    printf("%s\n", e == ESP_OK ? "Saved. Reboot to apply. Only use 'off' if joins fail with TCLK_EX_FAILURE."
                               : esp_err_to_name(e));
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    int64_t now = esp_timer_get_time();
    if (argc >= 3 && strcmp(argv[1], "confirm") == 0) {
        uint32_t code = (uint32_t)strtoul(argv[2], NULL, 10);
        if (!s_reset_code || now > s_reset_code_expiry_us || code != s_reset_code) {
            printf("REFUSED: wrong or expired code. Run 'reset' again.\n");
            s_reset_code = 0;
            return 1;
        }
        s_reset_code = 0;
        printf("Resetting network membership...\n");
        esp_err_t e = wb_zb_request_network_reset();
        if (e != ESP_OK) printf("Reset request returned %s\n", esp_err_to_name(e));
        return 0;
    }
    s_reset_code = 1000 + (esp_random() % 9000);
    s_reset_code_expiry_us = now + 30LL * 1000 * 1000;
    printf("WARNING: this makes the repeater LEAVE the Wiser network and erases its Zigbee state.\n"
           "Child TRVs will have to find another parent (they normally do so automatically).\n"
           "The compatibility profile and options in NVS are kept.\n"
           "To proceed within 30 s type:  reset confirm %" PRIu32 "\n",
           s_reset_code);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("Rebooting (network membership is kept)...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static esp_log_level_t parse_level(const char *s)
{
    if (!strcmp(s, "none")) return ESP_LOG_NONE;
    if (!strcmp(s, "error")) return ESP_LOG_ERROR;
    if (!strcmp(s, "warn")) return ESP_LOG_WARN;
    if (!strcmp(s, "info")) return ESP_LOG_INFO;
    if (!strcmp(s, "debug")) return ESP_LOG_DEBUG;
    return (esp_log_level_t)-1;
}

static int cmd_log(int argc, char **argv)
{
    if (argc < 3) { printf("usage: log <tag|*> <none|error|warn|info|debug>\n"); return 1; }
    esp_log_level_t l = parse_level(argv[2]);
    if ((int)l < 0) { printf("bad level\n"); return 1; }
    esp_log_level_set(argv[1], l);
    printf("ok\n");
    return 0;
}

esp_err_t wb_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "wb>";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 6144;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl));
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_cfg, &repl));
#else
#error "Unsupported console"
#endif

    esp_console_register_help_command();
    REGISTER("status", cmd_status, NULL, "Overall state, link counts and diagnostic verdict");
    REGISTER("network", cmd_network, NULL, "PAN ID, extended PAN ID, channel, short/IEEE address, join state");
    REGISTER("neighbors", cmd_neighbors, NULL, "Local neighbour table (LQI, RSSI, relationship, cost, age)");
    REGISTER("routes", cmd_routes, NULL, "Local routing table and route records");
    REGISTER("children", cmd_children, NULL, "Child devices using this repeater as parent");
    REGISTER("diagnostics", cmd_diagnostics, NULL, "Join/commissioning counters and failure classification");
    REGISTER("events", cmd_events, NULL, "Recent event log (joins, leaves, children, NWK status)");
    REGISTER("multihop", cmd_multihop, NULL, "Evidence of routing via an intermediate router to the hub");
    REGISTER("health", cmd_health, NULL, "Print one machine-readable HEALTH line");
    REGISTER("join", cmd_join, NULL, "Join an existing network (only when NOT joined; never forms a network)");
    REGISTER("scan", cmd_scan, "[force]", "Active beacon scan (refused while joined unless 'force')");
    REGISTER("ping", cmd_ping, "<0xSHORT|IEEE> [count]", "Send ZDO Node_Desc_req and time the reply (0x0000 = hub)");
    REGISTER("lqi", cmd_lqi, "<0xSHORT|IEEE>", "Read a remote router's neighbour table (ZDO Mgmt_Lqi_req)");
    REGISTER("interview", cmd_interview, "<0xSHORT|IEEE>", "Read descriptors + Basic cluster -> FINGERPRINT-JSON line");
    REGISTER("profile", cmd_profile, "[show|pending|set <k> <v>|save|defaults]", "Device identity/compatibility profile");
    REGISTER("epid", cmd_epid, "[<EPID>|clear|show]", "Restrict joining to one extended PAN ID");
    REGISTER("channels", cmd_channels, "[<list>|0xMASK|default]", "Channels searched when joining, e.g. 'channels 15'");
    REGISTER("tclk", cmd_tclk, "[on|off|default]", "Require Trust Center link-key exchange when joining");
    REGISTER("reset", cmd_reset, "[confirm <code>]", "Leave network + erase Zigbee state (needs confirmation code)");
    REGISTER("reboot", cmd_reboot, NULL, "Restart the device (keeps network membership)");
    REGISTER("version", cmd_version, NULL, "Firmware and toolchain version");
    REGISTER("log", cmd_log, "<tag|*> <level>", "Set log level at runtime");

    ESP_LOGI(TAG, "Serial command interface ready");
    return esp_console_start_repl(repl);
}
