/*
 * zigbee_router.c - Zigbee Router lifecycle on esp-zigbee-sdk v1.6 (ZBOSS).
 *
 * Design rules (heating-safety):
 *  - Role is always ESP_ZB_DEVICE_TYPE_ROUTER. Network formation is never
 *    requested, so this device cannot create its own network.
 *  - Steering is only started on explicit request (CLI 'join' / BOOT short press,
 *    or Kconfig auto-join) and only while NOT joined. On a joined router BDB
 *    steering would broadcast permit-join and open the Wiser network - refused.
 *  - If the network removes us, we do NOT automatically rejoin.
 *  - Network reset uses the BDB local-reset (sends Leave, keeps the NWK frame
 *    counter) when joined; full NVRAM erase only when not joined.
 */
#include "zigbee_router.h"

#include <string.h>

#include "aps_inspect.h"
#include "board.h"
#include "diag_logic.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wiser_profile.h"

static const char *TAG = "WB_ZB";

/* Keep diag_logic's mirrored constants honest. */
_Static_assert(WB_BDB_STATUS_NO_NETWORK == ESP_ZB_BDB_STATUS_NO_NETWORK, "bdb enum drift");
_Static_assert(WB_BDB_STATUS_TCLK_EX_FAILURE == ESP_ZB_BDB_STATUS_TCLK_EX_FAILURE, "bdb enum drift");
_Static_assert(WB_BDB_STATUS_NOT_PERMITTED == ESP_ZB_BDB_STATUS_NOT_PERMITTED, "bdb enum drift");
_Static_assert(WB_REL_CHILD == ESP_ZB_NWK_RELATIONSHIP_CHILD, "relationship enum drift");
_Static_assert(WB_REL_UNAUTH_CHILD == ESP_ZB_NWK_RELATIONSHIP_UNAUTHENTICATED_CHILD, "relationship enum drift");
_Static_assert(WB_DEV_ROUTER == ESP_ZB_DEVICE_TYPE_ROUTER, "device type enum drift");
_Static_assert(WB_DEV_END_DEVICE == ESP_ZB_DEVICE_TYPE_ED, "device type enum drift");

#define REMOVED_AFTER_JOIN_WINDOW_MS (10 * 60 * 1000)
#define HOUSEKEEPING_PERIOD_MS       (10 * 1000)
#define INIT_RETRY_MS                (3 * 1000)

static wb_zb_status_t s_st = {
    .state = WB_NS_INIT,
    .last_bdb_status = WB_BDB_STATUS_UNKNOWN,
};
static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_local_reset_requested;
static int64_t s_last_health_ms;

/* ZCL char strings: first byte is length. Must outlive the stack. */
static char s_zcl_manufacturer[WB_PROFILE_STR_MAX + 2];
static char s_zcl_model[WB_PROFILE_STR_MAX + 2];
static char s_zcl_sw_build[32 + 2];
static char s_zcl_date_code[16 + 2];

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#define ST_LOCK()   portENTER_CRITICAL(&s_st_mux)
#define ST_UNLOCK() portEXIT_CRITICAL(&s_st_mux)

static void set_state(wb_net_state_t s)
{
    ST_LOCK();
    s_st.state = s;
    ST_UNLOCK();
    switch (s) {
    case WB_NS_IDLE: wb_board_set_led(WB_LED_IDLE); break;
    case WB_NS_STEERING: wb_board_set_led(WB_LED_STEERING); break;
    case WB_NS_JOINED: wb_board_set_led(WB_LED_JOINED_ISOLATED); break; /* upgraded by housekeeping */
    case WB_NS_ERROR: wb_board_set_led(WB_LED_ERROR); break;
    default: wb_board_set_led(WB_LED_OFF); break;
    }
}

const char *wb_net_state_str(wb_net_state_t s)
{
    switch (s) {
    case WB_NS_INIT: return "INIT";
    case WB_NS_IDLE: return "NOT_COMMISSIONED";
    case WB_NS_STEERING: return "JOINING";
    case WB_NS_JOINED: return "JOINED";
    case WB_NS_LEAVING: return "LEAVING";
    case WB_NS_ERROR: return "STACK_ERROR";
    default: return "?";
    }
}

void wb_zb_get_status(wb_zb_status_t *out)
{
    ST_LOCK();
    *out = s_st;
    ST_UNLOCK();
}

bool wb_zb_lock(uint32_t timeout_ms)
{
    return esp_zb_lock_acquire(pdMS_TO_TICKS(timeout_ms));
}

void wb_zb_unlock(void)
{
    esp_zb_lock_release();
}

uint32_t wb_zb_effective_channel_mask(void)
{
    uint32_t m = wb_opt_get_channel_mask();
    return m ? m : (uint32_t)CONFIG_WB_CHANNEL_MASK;
}

bool wb_zb_effective_tclk_required(void)
{
    int v = wb_opt_get_tclk_required();
#if CONFIG_WB_TCLK_EXCHANGE_REQUIRED
    if (v < 0) return true;
#else
    if (v < 0) return false;
#endif
    return v != 0;
}

/* ------------------------------------------------------------------------- */
/* Steering                                                                  */
/* ------------------------------------------------------------------------- */

static void steering_attempt_cb(uint8_t param);

static void start_steering_attempt(void)
{
    ST_LOCK();
    s_st.steering_attempts++;
    uint32_t n = s_st.steering_attempts;
    ST_UNLOCK();
    wb_diag_count_steering_attempt();
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    ESP_LOGI(TAG, "Network steering attempt #%lu started (%s)", (unsigned long)n, esp_err_to_name(err));
    if (err != ESP_OK) {
        /* Stack busy - try again shortly within the window. */
        esp_zb_scheduler_alarm(steering_attempt_cb, 0, 2000);
    }
}

static void steering_attempt_cb(uint8_t param)
{
    (void)param;
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    if (st.state != WB_NS_STEERING) return;
    if (esp_zb_bdb_dev_joined()) return;
    start_steering_attempt();
}

static void steering_window_closed(void)
{
    set_state(WB_NS_IDLE);
    wb_diag_event("JOIN window closed without joining (last BDB status %s)",
                  wb_bdb_status_str(s_st.last_bdb_status));
    ESP_LOGW(TAG, "Join window closed. Running an active scan to classify the failure...");
    wb_diag_start_scan_internal(true);
}

esp_err_t wb_zb_request_join(const char **why)
{
    static const char *ok = "ok";
    *why = ok;
    if (!wb_zb_lock(2000)) { *why = "Zigbee stack busy"; return ESP_ERR_TIMEOUT; }

    esp_err_t ret = ESP_OK;
    wb_zb_status_t st;
    wb_zb_get_status(&st);

    if (!st.stack_started) {
        *why = "stack not started yet";
        ret = ESP_ERR_INVALID_STATE;
    } else if (esp_zb_bdb_dev_joined() || st.state == WB_NS_JOINED) {
        *why = "already joined. Steering on a joined router would open the Wiser network for joining, so it is refused. "
               "Use 'reset' first if you really want to re-commission";
        ret = ESP_ERR_INVALID_STATE;
    } else if (st.state == WB_NS_STEERING) {
        *why = "join already in progress";
    } else if (st.state == WB_NS_LEAVING || st.state == WB_NS_INIT) {
        *why = "busy (leaving/initialising)";
        ret = ESP_ERR_INVALID_STATE;
    } else {
        uint8_t epid[8];
        if (wb_opt_get_epid_filter(epid)) {
            esp_zb_set_extended_pan_id(epid);
            char buf[24];
            wb_format_ieee(epid, buf, sizeof(buf));
            ESP_LOGI(TAG, "Join restricted to extended PAN ID %s", buf);
        } else {
            /* All-zero EPID = no preference (join any open network on the channel mask). Set
             * explicitly so a filter cleared at runtime does not linger in the NIB. */
            memset(epid, 0, sizeof(epid));
            esp_zb_set_extended_pan_id(epid);
            ESP_LOGW(TAG, "No EPID filter: will join ANY network with permit-join open. "
                          "Use 'scan' + 'epid <EPID>' to restrict to the Wiser network.");
        }
        ST_LOCK();
        s_st.join_attempted = true;
        s_st.removed_after_join = false;
        s_st.last_bdb_status = WB_BDB_STATUS_UNKNOWN;
        s_st.steering_deadline_ms = now_ms() + (int64_t)CONFIG_WB_STEERING_WINDOW_S * 1000;
        ST_UNLOCK();
        wb_diag_reset_scan_summary();
        set_state(WB_NS_STEERING);
        wb_diag_event("JOIN requested: window %ds, channels mask 0x%08lx", CONFIG_WB_STEERING_WINDOW_S,
                      (unsigned long)wb_zb_effective_channel_mask());
        start_steering_attempt();
    }
    wb_zb_unlock();
    return ret;
}

esp_err_t wb_zb_request_network_reset(void)
{
    if (!wb_zb_lock(2000)) return ESP_ERR_TIMEOUT;
    bool joined = esp_zb_bdb_dev_joined();
    if (joined) {
        s_local_reset_requested = true;
        set_state(WB_NS_LEAVING);
        wb_diag_event("RESET: local reset (Leave broadcast, NVRAM cleared, frame counter kept)");
        ESP_LOGW(TAG, "Leaving network via BDB local reset...");
        esp_zb_bdb_reset_via_local_action();
        wb_zb_unlock();
        return ESP_OK;
    }
    wb_zb_unlock();
    wb_diag_event("RESET: not joined - erasing Zigbee NVRAM and restarting");
    ESP_LOGW(TAG, "Not joined: erasing zb_storage and restarting");
    vTaskDelay(pdMS_TO_TICKS(200));
    if (wb_zb_lock(2000)) {
        esp_zb_factory_reset(); /* erases zb_storage and restarts */
        wb_zb_unlock();
    }
    return ESP_FAIL; /* not reached on success */
}

static void restart_cb(uint8_t param)
{
    (void)param;
    esp_restart();
}

/* ------------------------------------------------------------------------- */
/* Housekeeping: LED, isolation watchdog, periodic HEALTH                     */
/* ------------------------------------------------------------------------- */

/* Resolve the On/Off output pin: NVS override if set, else the Kconfig default. */
void wb_zb_apply_relay_config(void)
{
    int pin = wb_opt_get_relay_gpio();
    if (pin == WB_RELAY_GPIO_UNSET) pin = CONFIG_WB_RELAY_GPIO;
    int inv = wb_opt_get_relay_active_low();
    bool active_low = (inv < 0) ? (bool)WB_RELAY_ACTIVE_LOW_DEFAULT : (inv != 0);
    wb_board_relay_configure(pin, active_low);
}

/* Keep the fake plug looking alive: refresh AC readings and re-report On/Off + current to the hub so
 * it is not marked unresponsive. Runs in the stack thread (from housekeeping), so no lock needed. */
static void plug_keepalive(void)
{
    uint8_t ep = wb_profile_get()->endpoint;
    bool on = wb_board_relay_get();
    uint16_t current = on ? 41 : 0; /* 0.041 A placeholder when "on" (RMSCurrent, ÷1000) */
    uint16_t voltage = 2295 + (uint16_t)(esp_random() % 20); /* ~229.5-231.4 V */
    esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSVOLTAGE_ID, &voltage, false);
    esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSCURRENT_ID, &current, false);
    esp_zb_zcl_report_attr_cmd_t rep = {
        .zcl_basic_cmd = {.dst_addr_u.addr_short = 0x0000, .dst_endpoint = ep, .src_endpoint = ep},
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        .attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
    };
    esp_zb_zcl_report_attr_cmd_req(&rep);
    rep.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT;
    rep.attributeID = ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSCURRENT_ID;
    esp_zb_zcl_report_attr_cmd_req(&rep);
}

static void housekeeping_cb(uint8_t param)
{
    (void)param;
    wb_zb_status_t st;
    wb_zb_get_status(&st);

#if CONFIG_WB_PLUG_REPORT_S > 0
    static int64_t s_last_plug_ms = 0;
    if (st.state == WB_NS_JOINED && now_ms() - s_last_plug_ms >= (int64_t)CONFIG_WB_PLUG_REPORT_S * 1000) {
        s_last_plug_ms = now_ms();
        plug_keepalive();
    }
#endif

    if (st.state == WB_NS_JOINED) {
        wb_link_summary_t ls;
        wb_diag_link_summary(&ls);
        int64_t t = now_ms();
        if (ls.router_neighbors == 0) {
            if (st.isolated_since_ms == 0) {
                ST_LOCK(); s_st.isolated_since_ms = t; ST_UNLOCK();
            } else if (t - st.isolated_since_ms >= (int64_t)CONFIG_WB_ISOLATION_ALERT_S * 1000 &&
                       ((t - st.isolated_since_ms) / HOUSEKEEPING_PERIOD_MS) % 30 == 0) {
                ESP_LOGW(TAG, "ISOLATED: joined but no router/coordinator neighbour for %llds. "
                              "Children cannot reach the hub. Check WA104/hub power and placement.",
                         (long long)((t - st.isolated_since_ms) / 1000));
                wb_diag_event("ISOLATED for %llds", (long long)((t - st.isolated_since_ms) / 1000));
            }
            wb_board_set_led(WB_LED_JOINED_ISOLATED);
        } else {
            if (st.isolated_since_ms != 0) {
                ESP_LOGI(TAG, "Router neighbour link(s) present again (%u)", ls.router_neighbors);
                ST_LOCK(); s_st.isolated_since_ms = 0; ST_UNLOCK();
            }
            wb_board_set_led(WB_LED_JOINED);
        }
    }

#if CONFIG_WB_HEALTH_INTERVAL_S > 0
    if (now_ms() - s_last_health_ms >= (int64_t)CONFIG_WB_HEALTH_INTERVAL_S * 1000) {
        s_last_health_ms = now_ms();
        wb_diag_print_health();
    }
#endif
    esp_zb_scheduler_alarm(housekeeping_cb, 0, HOUSEKEEPING_PERIOD_MS);
}

/* ------------------------------------------------------------------------- */
/* Signal handler                                                            */
/* ------------------------------------------------------------------------- */

static void bdb_init_cb(uint8_t mode_mask)
{
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode_mask);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BDB initialisation start failed: %s", esp_err_to_name(err));
    }
}

static void on_joined(const char *how)
{
    ST_LOCK();
    s_st.joined_at_ms = now_ms();
    s_st.stack_error = false;
    s_st.isolated_since_ms = 0;
    ST_UNLOCK();
    set_state(WB_NS_JOINED);
    wb_diag_log_network(how);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialised (%s); starting BDB initialisation", esp_zb_get_version_string());
        ST_LOCK(); s_st.stack_started = true; ST_UNLOCK();
        esp_zb_set_tx_power(CONFIG_WB_TX_POWER_DBM);
        wb_aps_inspect_install();
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        esp_zb_scheduler_alarm(housekeeping_cb, 0, HOUSEKEEPING_PERIOD_MS);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            ST_LOCK(); s_st.stack_error = false; ST_UNLOCK();
            if (esp_zb_bdb_is_factory_new()) {
                set_state(WB_NS_IDLE);
                wb_diag_event("START: factory-new, not commissioned");
                ESP_LOGW(TAG, "Not commissioned. Put the Wiser hub into pairing mode, then type 'join' "
                              "or short-press BOOT. (No network will be formed by this device.)");
#if CONFIG_WB_AUTO_JOIN_ON_FIRST_BOOT
                ESP_LOGI(TAG, "Auto-join enabled by Kconfig");
                /* Already in Zigbee context: start steering directly (no lock needed). */
                ST_LOCK();
                s_st.join_attempted = true;
                s_st.steering_deadline_ms = now_ms() + (int64_t)CONFIG_WB_STEERING_WINDOW_S * 1000;
                ST_UNLOCK();
                set_state(WB_NS_STEERING);
                start_steering_attempt();
#endif
            } else {
                ST_LOCK(); s_st.restored_from_nvram = true; ST_UNLOCK();
                wb_diag_count_rejoin();
                wb_diag_event("START: network membership restored from NVRAM");
                on_joined("Network membership restored after restart (from NVRAM)");
            }
        } else {
            ST_LOCK(); s_st.stack_error = true; ST_UNLOCK();
            set_state(WB_NS_ERROR);
            wb_diag_event("START FAILED: %s (%s) - retrying", esp_zb_zdo_signal_to_string(sig), esp_err_to_name(err));
            ESP_LOGE(TAG, "%s failed (%s); retrying BDB initialisation in %d ms",
                     esp_zb_zdo_signal_to_string(sig), esp_err_to_name(err), INIT_RETRY_MS);
            esp_zb_scheduler_alarm(bdb_init_cb, ESP_ZB_BDB_MODE_INITIALIZATION, INIT_RETRY_MS);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            ST_LOCK(); s_st.last_bdb_status = WB_BDB_STATUS_SUCCESS; s_st.last_steering_err = 0; ST_UNLOCK();
            wb_diag_count_steering_result(true, WB_BDB_STATUS_SUCCESS);
            wb_diag_event("JOIN SUCCESS (network steering)");
            on_joined("JOINED network via network steering");
        } else {
            uint8_t bdb = (uint8_t)esp_zb_get_bdb_commissioning_status();
            ST_LOCK(); s_st.last_bdb_status = bdb; s_st.last_steering_err = err; ST_UNLOCK();
            wb_diag_count_steering_result(false, bdb);
            wb_diag_event("JOIN attempt failed: %s, BDB status %s", esp_err_to_name(err), wb_bdb_status_str(bdb));
            ESP_LOGW(TAG, "Network steering failed (%s, BDB status %s)", esp_err_to_name(err), wb_bdb_status_str(bdb));
            wb_zb_status_t st;
            wb_zb_get_status(&st);
            if (st.state == WB_NS_STEERING) {
                if (now_ms() + (int64_t)CONFIG_WB_STEERING_RETRY_S * 1000 < st.steering_deadline_ms) {
                    ESP_LOGI(TAG, "Retrying in %ds (window ends in %llds)", CONFIG_WB_STEERING_RETRY_S,
                             (long long)((st.steering_deadline_ms - now_ms()) / 1000));
                    esp_zb_scheduler_alarm(steering_attempt_cb, 0, CONFIG_WB_STEERING_RETRY_S * 1000);
                } else {
                    steering_window_closed();
                }
            }
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING_CANCELLED:
        wb_diag_event("Steering cancelled (%s)", esp_err_to_name(err));
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        /* Never requested by this firmware. Logged loudly if it ever appears. */
        ESP_LOGE(TAG, "Unexpected FORMATION signal (%s) - this firmware never forms networks", esp_err_to_name(err));
        wb_diag_event("UNEXPECTED formation signal");
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE: {
        esp_zb_zdo_signal_leave_params_t *p = (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        uint8_t type = p ? p->leave_type : 0xFF;
        wb_diag_count_leave();
        if (type == ESP_ZB_NWK_LEAVE_TYPE_REJOIN) {
            wb_diag_event("LEAVE with rejoin requested by network; stack will rejoin");
            ESP_LOGW(TAG, "Left network with rejoin flag; the stack will attempt to rejoin");
            break;
        }
        if (s_local_reset_requested) {
            s_local_reset_requested = false;
            wb_diag_event("LEAVE complete (local reset). Restarting.");
            ESP_LOGW(TAG, "Left network after local reset. Restarting in 1 s.");
            esp_zb_scheduler_alarm(restart_cb, 0, 1000);
            break;
        }
        wb_zb_status_t st;
        wb_zb_get_status(&st);
        bool soon = st.joined_at_ms && (now_ms() - st.joined_at_ms) < REMOVED_AFTER_JOIN_WINDOW_MS;
        ST_LOCK(); s_st.removed_after_join = soon; s_st.restored_from_nvram = false; ST_UNLOCK();
        set_state(WB_NS_IDLE);
        wb_diag_event("REMOVED from network by remote Leave (%llds after join)%s",
                      (long long)((now_ms() - st.joined_at_ms) / 1000),
                      soon ? " - likely rejected by hub" : "");
        ESP_LOGE(TAG, "The network told this device to leave (%s). Not rejoining automatically. %s",
                 err == ESP_OK ? "ok" : esp_err_to_name(err),
                 soon ? "This happened soon after joining: the hub probably rejected the device at application level."
                      : "");
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        esp_zb_zdo_signal_leave_indication_params_t *p =
            (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        char ieee[24];
        wb_format_ieee(p->device_addr, ieee, sizeof(ieee));
        wb_diag_count_leave_indication();
        wb_diag_event("Neighbour/child 0x%04x (%s) left%s", p->short_addr, ieee, p->rejoin ? " (will rejoin)" : "");
        ESP_LOGI(TAG, "Device 0x%04hx (%s) left the network%s", p->short_addr, ieee, p->rejoin ? " with rejoin" : "");
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *p =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        wb_diag_on_device_annce(p->device_short_addr, p->ieee_addr, p->capability);
        break;
    }

    case ESP_ZB_NWK_SIGNAL_DEVICE_ASSOCIATED: {
        esp_zb_nwk_signal_device_associated_params_t *p =
            (esp_zb_nwk_signal_device_associated_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        wb_diag_on_device_associated(p->device_addr);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        esp_zb_zdo_signal_device_update_params_t *p =
            (esp_zb_zdo_signal_device_update_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        char ieee[24];
        wb_format_ieee(p->long_addr, ieee, sizeof(ieee));
        wb_diag_event("Device update 0x%04x (%s) status %u parent 0x%04x", p->short_addr, ieee, p->status, p->parent_short);
        ESP_LOGI(TAG, "Device update: 0x%04hx (%s) status=%u parent=0x%04hx", p->short_addr, ieee, p->status, p->parent_short);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        esp_zb_zdo_signal_device_authorized_params_t *p =
            (esp_zb_zdo_signal_device_authorized_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        char ieee[24];
        wb_format_ieee(p->long_addr, ieee, sizeof(ieee));
        wb_diag_event("Device authorized 0x%04x (%s) type %u status %u", p->short_addr, ieee,
                      p->authorization_type, p->authorization_status);
        ESP_LOGI(TAG, "Device authorized: 0x%04hx (%s) type=%u status=%u", p->short_addr, ieee,
                 p->authorization_type, p->authorization_status);
        break;
    }

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t secs = *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
        wb_diag_count_permit_join(secs);
        if (secs) {
            wb_diag_event("Permit-join OPEN for %us (set by network)", secs);
            ESP_LOGI(TAG, "Network permit-join open for %u s (requested by the network, e.g. hub pairing mode)", secs);
        } else {
            wb_diag_event("Permit-join CLOSED");
            ESP_LOGI(TAG, "Network permit-join closed");
        }
        break;
    }

    case ESP_ZB_NLME_STATUS_INDICATION: {
        esp_zb_zdo_signal_nwk_status_indication_params_t *p =
            (esp_zb_zdo_signal_nwk_status_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        wb_diag_on_nlme_status(p->status, p->network_addr);
        break;
    }

    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        wb_diag_count_no_active_links();
        wb_diag_event("NO ACTIVE LINKS LEFT");
        ESP_LOGW(TAG, "No active links left: all neighbour router links have aged out. "
                      "The stack keeps its network state and will recover when neighbours are heard again.");
        break;

    case ESP_ZB_NWK_SIGNAL_PANID_CONFLICT_DETECTED:
        wb_diag_event("PAN ID conflict detected");
        ESP_LOGW(TAG, "PAN ID conflict detected (the coordinator decides how to resolve it)");
        break;

    case ESP_ZB_BDB_SIGNAL_TC_REJOIN_DONE:
        wb_diag_count_rejoin();
        wb_diag_event("Trust Center rejoin done (%s)", esp_err_to_name(err));
        ESP_LOGI(TAG, "Trust Center rejoin done (%s)", esp_err_to_name(err));
        if (err == ESP_OK) on_joined("REJOINED via Trust Center rejoin");
        break;

    case ESP_ZB_ZDO_SIGNAL_ERROR:
        wb_diag_event("ZDO error signal (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "ZDO error signal (%s)", esp_err_to_name(err));
        break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        break;

    case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "No production config in zb_fct partition (normal for this firmware)");
        } else {
            ESP_LOGI(TAG, "Production config loaded from zb_fct");
        }
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig), (unsigned)sig,
                 esp_err_to_name(err));
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Endpoint / clusters                                                       */
/* ------------------------------------------------------------------------- */

static void zcl_str(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_size - 2) n = dst_size - 2;
    dst[0] = (char)n;
    memcpy(dst + 1, src, n);
    dst[n + 1] = '\0';
}

static void identify_cb(uint8_t identify_on)
{
    ESP_LOGI(TAG, "Identify %s", identify_on ? "start" : "stop");
    wb_diag_event("IDENTIFY %s (from the network)", identify_on ? "start" : "stop");
    wb_board_identify(identify_on ? 30 : 0);
}

/* The hub drives the plug's On/Off cluster; the stack stores the attribute and calls us here.
 * We mirror the new state to the output GPIO. Any other attribute write is just logged. */
static void on_set_attr_value(const esp_zb_zcl_set_attr_value_message_t *m)
{
    if (!m) return;
    if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        m->attribute.data.value) {
        bool on = *(bool *)m->attribute.data.value;
        wb_board_relay_set(on);
        ESP_LOGI(TAG, "On/Off command from network: %s -> GPIO", on ? "ON" : "OFF");
        wb_diag_event("ONOFF %s (ep%u, from the network)", on ? "ON" : "OFF", m->info.dst_endpoint);
    } else {
        ESP_LOGD(TAG, "Attr write ep%u cl 0x%04x attr 0x%04x", m->info.dst_endpoint, m->info.cluster,
                 m->attribute.id);
    }
}

static esp_err_t action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        on_set_attr_value((const esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        wb_diag_on_read_attr_resp((const esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "ZCL action callback 0x%x", (unsigned)callback_id);
        break;
    }
    return ESP_OK;
}

/*
 * Add one extra cluster from the profile. Standard clusters are created with the SDK's own
 * constructors and default attribute values (so the stack's handlers find the attributes they
 * expect); client roles get an empty attribute list; manufacturer-specific IDs (>= 0xFC00) are
 * added as empty custom clusters. None of these have any behaviour behind them.
 */
static esp_err_t add_profile_cluster(esp_zb_cluster_list_t *list, uint16_t id, uint8_t role)
{
    const bool server = (role == ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_attribute_list_t *al;
    switch (id) {
    case ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY: {
        esp_zb_identify_cluster_cfg_t c = {.identify_time = 0};
        al = server ? esp_zb_identify_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_identify_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_GROUPS: {
        esp_zb_groups_cluster_cfg_t c = {.groups_name_support_id = 0};
        al = server ? esp_zb_groups_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_groups_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_SCENES: {
        esp_zb_scenes_cluster_cfg_t c = {0};
        al = server ? esp_zb_scenes_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_scenes_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF: {
        esp_zb_on_off_cluster_cfg_t c = {.on_off = false};
        al = server ? esp_zb_on_off_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_on_off_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_ALARMS: {
        esp_zb_alarms_cluster_cfg_t c = {.alarm_table_size = 0};
        al = server ? esp_zb_alarms_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_alarms_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_METERING: {
        esp_zb_metering_cluster_cfg_t c = {0}; /* 0 kWh delivered, electric metering, kWh units */
        al = server ? esp_zb_metering_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_metering_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT: {
        esp_zb_electrical_meas_cluster_cfg_t c = {.measured_type = 0};
        al = server ? esp_zb_electrical_meas_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        if (server && al) {
            /* Give the hub real-looking AC readings to poll/report (kept updated while joined). */
            uint16_t v = 2300, i = 0; /* 230.0 V (÷10), 0 A until switched on */
            esp_zb_electrical_meas_cluster_add_attr(al, ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSVOLTAGE_ID, &v);
            esp_zb_electrical_meas_cluster_add_attr(al, ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSCURRENT_ID, &i);
        }
        return esp_zb_cluster_list_add_electrical_meas_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_DIAGNOSTICS: {
        esp_zb_diagnostics_cluster_cfg_t c = {};
        al = server ? esp_zb_diagnostics_cluster_create(&c) : esp_zb_zcl_attr_list_create(id);
        return esp_zb_cluster_list_add_diagnostics_cluster(list, al, role);
    }
    case ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE:
        /* Deliberately not exposed: a real OTA client would invite the hub to push another vendor's
         * firmware image at this device. The profile keeps the ID so the operator sees it was skipped. */
        ESP_LOGW(TAG, "Cluster 0x0019 (OTA) in profile is NOT exposed on purpose (would solicit vendor firmware)");
        return ESP_OK;
    default:
        if (id >= 0xFC00) {
            return esp_zb_cluster_list_add_custom_cluster(list, esp_zb_zcl_attr_list_create(id), role);
        }
        return ESP_ERR_NOT_SUPPORTED; /* standard cluster without a constructor here */
    }
}

static esp_err_t register_endpoint(const wb_profile_t *p)
{
    zcl_str(s_zcl_manufacturer, sizeof(s_zcl_manufacturer), p->manufacturer);
    zcl_str(s_zcl_model, sizeof(s_zcl_model), p->model);
    zcl_str(s_zcl_sw_build, sizeof(s_zcl_sw_build), esp_app_get_description()->version);
    zcl_str(s_zcl_date_code, sizeof(s_zcl_date_code), p->date_code);

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = p->power_source,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_RETURN_ON_FALSE(basic, ESP_ERR_NO_MEM, TAG, "basic cluster");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_zcl_manufacturer),
                        TAG, "manufacturer attr");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_zcl_model),
                        TAG, "model attr");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, s_zcl_sw_build),
                        TAG, "sw build attr");
    if (p->date_code[0]) {
        ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, s_zcl_date_code),
                            TAG, "date code attr");
    }

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    ESP_RETURN_ON_FALSE(clusters, ESP_ERR_NO_MEM, TAG, "cluster list");
    ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_basic_cluster(clusters, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE),
                        TAG, "add basic");

    if (p->identify) {
        esp_zb_identify_cluster_cfg_t id_cfg = {.identify_time = 0};
        ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_identify_cluster(clusters, esp_zb_identify_cluster_create(&id_cfg),
                                                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE),
                            TAG, "add identify");
    }
    for (int i = 0; i < p->extra_in_count; ++i) {
        esp_err_t e = add_profile_cluster(clusters, p->extra_in[i], ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        if (e != ESP_OK) ESP_LOGW(TAG, "Could not add server cluster 0x%04x: %s", p->extra_in[i], esp_err_to_name(e));
    }
    for (int i = 0; i < p->extra_out_count; ++i) {
        esp_err_t e = add_profile_cluster(clusters, p->extra_out[i], ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        if (e != ESP_OK) ESP_LOGW(TAG, "Could not add client cluster 0x%04x: %s", p->extra_out[i], esp_err_to_name(e));
    }

    esp_zb_ep_list_t *eps = esp_zb_ep_list_create();
    ESP_RETURN_ON_FALSE(eps, ESP_ERR_NO_MEM, TAG, "ep list");
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = p->endpoint,
        .app_profile_id = p->profile_id,
        .app_device_id = p->device_id,
        .app_device_version = p->device_version,
    };
    ESP_RETURN_ON_ERROR(esp_zb_ep_list_add_ep(eps, clusters, ep_cfg), TAG, "add ep");
    ESP_RETURN_ON_ERROR(esp_zb_device_register(eps), TAG, "device register");

    if (p->identify) esp_zb_identify_notify_handler_register(p->endpoint, identify_cb);
    esp_zb_core_action_handler_register(action_handler);

    if (p->node_manufacturer_code) esp_zb_set_node_descriptor_manufacturer_code(p->node_manufacturer_code);
    esp_zb_set_node_descriptor_power_source(true);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Task                                                                      */
/* ------------------------------------------------------------------------- */

static void zigbee_task(void *arg)
{
    (void)arg;
    const wb_profile_t *p = wb_profile_get();

    wb_zb_apply_relay_config();
    esp_zb_overall_network_size_set(CONFIG_WB_NETWORK_SIZE);

    esp_zb_cfg_t cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {.max_children = p->max_children},
    };
    esp_zb_init(&cfg);

    uint32_t mask = wb_zb_effective_channel_mask();
    esp_zb_set_channel_mask(mask);
    esp_zb_set_primary_network_channel_set(mask);
    esp_zb_set_secondary_network_channel_set(mask);
    esp_zb_secur_link_key_exchange_required_set(wb_zb_effective_tclk_required());

    if (register_endpoint(p) != ESP_OK) {
        ESP_LOGE(TAG, "Endpoint registration failed - check the compatibility profile ('profile show')");
        ST_LOCK(); s_st.stack_error = true; ST_UNLOCK();
        set_state(WB_NS_ERROR);
    }

    esp_err_t err = esp_zb_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_start failed: %s", esp_err_to_name(err));
        ST_LOCK(); s_st.stack_error = true; ST_UNLOCK();
        set_state(WB_NS_ERROR);
    }
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

esp_err_t wb_zb_start(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_RETURN_ON_ERROR(esp_zb_platform_config(&config), TAG, "platform config");
    BaseType_t ok = xTaskCreate(zigbee_task, "Zigbee_main", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
