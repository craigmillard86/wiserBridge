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

#include "board.h"
#include "diag_logic.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
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

static void housekeeping_cb(uint8_t param)
{
    (void)param;
    wb_zb_status_t st;
    wb_zb_get_status(&st);

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
    wb_board_identify(identify_on ? 30 : 0);
}

static esp_err_t action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        wb_diag_on_read_attr_resp((const esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "ZCL action callback 0x%x", (unsigned)callback_id);
        break;
    }
    return ESP_OK;
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
        esp_err_t e = esp_zb_cluster_list_add_custom_cluster(clusters, esp_zb_zcl_attr_list_create(p->extra_in[i]),
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        if (e != ESP_OK) ESP_LOGW(TAG, "Could not add server cluster 0x%04x: %s", p->extra_in[i], esp_err_to_name(e));
    }
    for (int i = 0; i < p->extra_out_count; ++i) {
        esp_err_t e = esp_zb_cluster_list_add_custom_cluster(clusters, esp_zb_zcl_attr_list_create(p->extra_out[i]),
                                                             ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
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
