/*
 * diagnostics.c - Event log, counters, table dumps and on-air ZDO probes.
 */
#include "diagnostics.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "diag_logic.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "wiser_profile.h"
#include "zigbee_router.h"

static const char *TAG = "WB_DIAG";

#if CONFIG_WB_AUTO_JOIN_ON_FIRST_BOOT
#define WB_AUTO_JOIN_STR "on"
#else
#define WB_AUTO_JOIN_STR "off (use 'join' or BOOT short press)"
#endif

#define LOCK_TIMEOUT_MS   3000
#define EVENT_COUNT       64
#define EVENT_LEN         112
#define NLME_CODES        0x14
#define OP_STUCK_MS       30000
#define READ_ATTR_TIMEOUT_MS 8000

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------------------- */
/* Event log + counters                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    int64_t t_ms;
    char text[EVENT_LEN];
} wb_event_t;

static wb_event_t s_events[EVENT_COUNT];
static uint32_t s_event_head; /* total events ever written */
static SemaphoreHandle_t s_event_mtx;

typedef struct {
    uint32_t steering_attempts;
    uint32_t steering_ok;
    uint32_t steering_fail;
    uint32_t steering_fail_no_network;
    uint32_t steering_fail_tclk;
    uint32_t steering_fail_other;
    uint32_t rejoins;
    uint32_t leaves;
    uint32_t leave_indications;
    uint32_t device_annce;
    uint32_t child_annce;
    uint32_t child_associations;
    uint32_t permit_join_open;
    uint32_t no_active_links;
    uint32_t nlme[NLME_CODES + 1];
    uint32_t ping_ok;
    uint32_t ping_fail;
} wb_counters_t;

static wb_counters_t s_cnt;

typedef struct {
    bool done;
    uint8_t networks;
    uint8_t permit_join;
    uint8_t router_capacity;
    int64_t at_ms;
} wb_scan_summary_t;

static wb_scan_summary_t s_scan;

static struct {
    bool valid;
    bool ok;
    int64_t at_ms;
    uint32_t rtt_ms;
} s_coord_ping;

void wb_diag_init(void)
{
    s_event_mtx = xSemaphoreCreateMutex();
}

void wb_diag_event(const char *fmt, ...)
{
    char buf[EVENT_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_event_mtx && xSemaphoreTake(s_event_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        wb_event_t *e = &s_events[s_event_head % EVENT_COUNT];
        e->t_ms = now_ms();
        strlcpy(e->text, buf, sizeof(e->text));
        s_event_head++;
        xSemaphoreGive(s_event_mtx);
    }
    ESP_LOGI("WB_EVENT", "%s", buf);
}

void wb_diag_count_steering_attempt(void) { s_cnt.steering_attempts++; }

void wb_diag_count_steering_result(bool ok, uint8_t bdb)
{
    if (ok) { s_cnt.steering_ok++; return; }
    s_cnt.steering_fail++;
    if (bdb == WB_BDB_STATUS_NO_NETWORK) s_cnt.steering_fail_no_network++;
    else if (bdb == WB_BDB_STATUS_TCLK_EX_FAILURE) s_cnt.steering_fail_tclk++;
    else s_cnt.steering_fail_other++;
}

void wb_diag_count_rejoin(void) { s_cnt.rejoins++; }
void wb_diag_count_leave(void) { s_cnt.leaves++; }
void wb_diag_count_leave_indication(void) { s_cnt.leave_indications++; }
void wb_diag_count_permit_join(uint8_t s) { if (s) s_cnt.permit_join_open++; }
void wb_diag_count_no_active_links(void) { s_cnt.no_active_links++; }

void wb_diag_reset_scan_summary(void) { memset(&s_scan, 0, sizeof(s_scan)); }

/* ------------------------------------------------------------------------- */
/* Banner                                                                    */
/* ------------------------------------------------------------------------- */

static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default: return "other";
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "OTHER WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

void wb_diag_print_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[8] = {0};
    esp_read_mac(mac, ESP_MAC_IEEE802154);
    char ieee[24];
    wb_format_ieee(mac, ieee, sizeof(ieee));
    /* esp_read_mac returns MSB-first bytes; wb_format_ieee expects little-endian. */
    snprintf(ieee, sizeof(ieee), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[6], mac[7]);
    char chs[64];
    wb_format_channel_mask(wb_zb_effective_channel_mask(), chs, sizeof(chs));
    const wb_profile_t *p = wb_profile_get();
    uint8_t epid[8];
    bool has_epid = wb_opt_get_epid_filter(epid);
    char epid_s[24] = "none";
    if (has_epid) wb_format_ieee(epid, epid_s, sizeof(epid_s));

    printf("\n");
    printf("==================================================================\n");
    printf(" WiserBridge Zigbee Router  (generic ZR - NOT a Drayton product)\n");
    printf("==================================================================\n");
    printf(" Firmware        : %s %s (built %s %s)\n", app->project_name, app->version, app->date, app->time);
    printf(" Chip            : %s rev v%d.%d, %d core(s), flash %" PRIu32 " MB, 802.15.4 %s\n",
           chip_model_str(chip.model), chip.revision / 100, chip.revision % 100, chip.cores,
           flash / (1024 * 1024), (chip.features & CHIP_FEATURE_IEEE802154) ? "yes" : "NO");
    printf(" Reset reason    : %s\n", reset_reason_str(esp_reset_reason()));
    printf(" ESP-IDF         : %s\n", esp_get_idf_version());
    printf(" esp-zigbee-lib  : %d.%d.%d (ZBOSS-based v1.x LTS)\n", ESP_ZB_VER_MAJOR, ESP_ZB_VER_MINOR, ESP_ZB_VER_PATCH);
    printf(" Zigbee role     : ROUTER (ZR) - never forms a network\n");
    printf(" Factory IEEE    : %s (unique per chip, from eFuse)\n", ieee);
    printf(" Endpoint        : %u  profile 0x%04x  device 0x%04x v%u\n", p->endpoint, p->profile_id, p->device_id,
           p->device_version);
    printf(" Identity        : \"%s\" / \"%s\" (profile source: %s)\n", p->manufacturer, p->model, p->source);
    printf(" Child limit     : %u (configured; actual TRV acceptance unverified)\n", p->max_children);
    printf(" Network size    : %d (neighbour/routing table sizing)\n", CONFIG_WB_NETWORK_SIZE);
    printf(" Channels        : %s (mask 0x%08" PRIx32 ")\n", chs, wb_zb_effective_channel_mask());
    printf(" EPID filter     : %s\n", epid_s);
    printf(" TCLK exchange   : %s\n", wb_zb_effective_tclk_required() ? "required (Zigbee 3.0)" : "NOT required (legacy hub mode)");
    printf(" TX power        : %d dBm   Antenna: %s\n", CONFIG_WB_TX_POWER_DBM, wb_board_antenna_str());
    printf(" Auto-join       : %s\n", WB_AUTO_JOIN_STR);
    printf(" Wi-Fi           : disabled (never started)\n");
    printf("==================================================================\n");
    printf(" Type 'help' for commands. Network state follows once the stack starts.\n\n");
}

/* ------------------------------------------------------------------------- */
/* Table collection (caller holds lock or runs in Zigbee context)            */
/* ------------------------------------------------------------------------- */

static int collect_neighbors(esp_zb_nwk_neighbor_info_t *out, int max)
{
    esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    int n = 0;
    while (n < max && esp_zb_nwk_get_next_neighbor(&it, &out[n]) == ESP_OK) n++;
    return n;
}

static int collect_routes(esp_zb_nwk_route_info_t *out, int max)
{
    esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    int n = 0;
    while (n < max && esp_zb_nwk_get_next_route(&it, &out[n]) == ESP_OK) n++;
    return n;
}

static int collect_route_records(esp_zb_nwk_route_record_info_t *out, int max)
{
    esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    int n = 0;
    while (n < max && esp_zb_nwk_get_next_route_record(&it, &out[n]) == ESP_OK) n++;
    return n;
}

static bool is_child_rel(uint8_t rel)
{
    return rel == ESP_ZB_NWK_RELATIONSHIP_CHILD || rel == ESP_ZB_NWK_RELATIONSHIP_UNAUTHENTICATED_CHILD;
}

static void summarize(const esp_zb_nwk_neighbor_info_t *nb, int nn, const esp_zb_nwk_route_info_t *rt, int nr,
                      wb_link_summary_t *s)
{
    memset(s, 0, sizeof(*s));
    s->route_to_coord_next_hop = 0xFFFF;
    s->total_neighbors = (uint8_t)nn;
    for (int i = 0; i < nn; ++i) {
        if (is_child_rel(nb[i].relationship)) {
            s->children++;
            if (!nb[i].rx_on_when_idle) s->sleepy_children++;
        } else if (nb[i].device_type == ESP_ZB_DEVICE_TYPE_ROUTER ||
                   nb[i].device_type == ESP_ZB_DEVICE_TYPE_COORDINATOR) {
            s->router_neighbors++;
        }
        if (nb[i].short_addr == 0x0000) {
            s->coordinator_is_neighbor = true;
            s->coordinator_lqi = nb[i].lqi;
            s->coordinator_rssi = nb[i].rssi;
        }
    }
    for (int i = 0; i < nr; ++i) {
        if (rt[i].flags.status == ESP_ZB_NWK_ROUTE_STATE_ACTIVE) s->routes_active++;
        if (rt[i].dest_addr == 0x0000 && !rt[i].flags.group_id) {
            s->route_to_coord_next_hop = rt[i].next_hop_addr;
            s->route_to_coord_state = rt[i].flags.status;
            s->route_to_coord_many_to_one = rt[i].flags.many_to_one;
            s->route_to_coord_relayed = rt[i].flags.status == ESP_ZB_NWK_ROUTE_STATE_ACTIVE &&
                                        rt[i].next_hop_addr != 0x0000;
        }
    }
}

static bool gather(esp_zb_nwk_neighbor_info_t **nb, int *nn, esp_zb_nwk_route_info_t **rt, int *nr)
{
    *nb = calloc(CONFIG_WB_NETWORK_SIZE, sizeof(**nb));
    *rt = calloc(CONFIG_WB_NETWORK_SIZE, sizeof(**rt));
    if (!*nb || !*rt) {
        free(*nb); free(*rt);
        *nb = NULL; *rt = NULL;
        return false;
    }
    *nn = collect_neighbors(*nb, CONFIG_WB_NETWORK_SIZE);
    *nr = collect_routes(*rt, CONFIG_WB_NETWORK_SIZE);
    return true;
}

void wb_diag_link_summary(wb_link_summary_t *out)
{
    esp_zb_nwk_neighbor_info_t *nb; esp_zb_nwk_route_info_t *rt; int nn, nr;
    if (!gather(&nb, &nn, &rt, &nr)) { memset(out, 0, sizeof(*out)); out->route_to_coord_next_hop = 0xFFFF; return; }
    summarize(nb, nn, rt, nr, out);
    free(nb); free(rt);
}

static wb_verdict_t classify_now(const wb_link_summary_t *ls)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    wb_diag_inputs_t in = {
        .stack_error = st.stack_error,
        .steering_active = st.state == WB_NS_STEERING,
        .join_attempted = st.join_attempted,
        .joined = st.state == WB_NS_JOINED,
        .removed_after_join = st.removed_after_join,
        .last_bdb_status = st.last_bdb_status,
        .scan_done = s_scan.done,
        .scan_networks = s_scan.networks,
        .scan_permit_join = s_scan.permit_join,
        .scan_router_capacity = s_scan.router_capacity,
        .router_neighbors = ls ? ls->router_neighbors : 0,
        .children = ls ? ls->children : 0,
        .coordinator_ping_ok = s_coord_ping.valid && s_coord_ping.ok,
        .route_to_coordinator_relayed = ls ? ls->route_to_coord_relayed : false,
    };
    return wb_classify(&in);
}

/* ------------------------------------------------------------------------- */
/* Network info / health                                                     */
/* ------------------------------------------------------------------------- */

static void print_network_locked(const char *title)
{
    esp_zb_ieee_addr_t epid, ieee;
    esp_zb_get_extended_pan_id(epid);
    esp_zb_get_long_address(ieee);
    char epid_s[24], ieee_s[24], chs[64];
    wb_format_ieee(epid, epid_s, sizeof(epid_s));
    wb_format_ieee(ieee, ieee_s, sizeof(ieee_s));
    wb_format_channel_mask(esp_zb_get_primary_network_channel_set(), chs, sizeof(chs));
    int8_t txp = 0;
    esp_zb_get_tx_power(&txp);
    bool joined = esp_zb_bdb_dev_joined();

    printf("%s\n", title);
    printf("  Joined/commissioned : %s (factory-new: %s)\n", joined ? "YES" : "no",
           esp_zb_bdb_is_factory_new() ? "yes" : "no");
    printf("  Role                : %s\n", wb_device_type_str((uint8_t)esp_zb_get_network_device_role()));
    printf("  Local IEEE          : %s\n", ieee_s);
    if (joined) {
        printf("  Short address       : 0x%04hx\n", esp_zb_get_short_address());
        printf("  PAN ID              : 0x%04hx\n", esp_zb_get_pan_id());
        printf("  Extended PAN ID     : %s\n", epid_s);
        printf("  Channel             : %u\n", esp_zb_get_current_channel());
        printf("  NWK update ID       : %u\n", esp_zb_nwk_get_update_id());
        printf("  Outgoing NWK frame counter: %" PRIu32 " (not secret; shown for frame-counter diagnostics)\n",
               esp_zb_nwk_get_frame_counter());
    }
    printf("  Max children        : %u\n", esp_zb_nwk_get_max_children());
    printf("  Link status period  : %u s\n", esp_zb_nwk_get_link_status_period());
    printf("  Scan channel set    : %s\n", chs);
    printf("  TX power            : %d dBm\n", txp);
}

void wb_diag_log_network(const char *how)
{
    ESP_LOGI(TAG, "%s", how);
    print_network_locked("NETWORK");
    wb_diag_event("NETWORK pan=0x%04x ch=%u short=0x%04x", esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                  esp_zb_get_short_address());
}

static void print_health_line(const wb_link_summary_t *ls)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    uint32_t nlme_err = 0;
    for (int i = 0; i <= NLME_CODES; ++i) nlme_err += s_cnt.nlme[i];
    bool joined = st.state == WB_NS_JOINED;
    printf("HEALTH up=%llds state=%s short=0x%04x pan=0x%04x ch=%u nbr=%u rtr=%u child=%u sleepy=%u routes=%u "
           "coord_nh=0x%04x heap=%" PRIu32 " minheap=%" PRIu32 " rejoins=%" PRIu32 " leaves=%" PRIu32
           " nlme=%" PRIu32 " noLinks=%" PRIu32 " verdict=%s\n",
           (long long)(now_ms() / 1000), wb_net_state_str(st.state),
           joined ? esp_zb_get_short_address() : 0xFFFF, joined ? esp_zb_get_pan_id() : 0xFFFF,
           joined ? esp_zb_get_current_channel() : 0, ls->total_neighbors, ls->router_neighbors, ls->children,
           ls->sleepy_children, ls->routes_active, ls->route_to_coord_next_hop,
           (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(), s_cnt.rejoins,
           s_cnt.leaves, nlme_err, s_cnt.no_active_links, wb_verdict_name(classify_now(ls)));
}

void wb_diag_print_health(void)
{
    wb_link_summary_t ls;
    wb_diag_link_summary(&ls);
    print_health_line(&ls);
}

/* ------------------------------------------------------------------------- */
/* Signal hooks                                                              */
/* ------------------------------------------------------------------------- */

static bool lookup_neighbor(uint16_t short_addr, esp_zb_nwk_neighbor_info_t *out)
{
    esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    esp_zb_nwk_neighbor_info_t nb;
    while (esp_zb_nwk_get_next_neighbor(&it, &nb) == ESP_OK) {
        if (nb.short_addr == short_addr) { *out = nb; return true; }
    }
    return false;
}

void wb_diag_on_device_annce(uint16_t short_addr, const uint8_t ieee[8], uint8_t cap)
{
    char ieee_s[24];
    wb_format_ieee(ieee, ieee_s, sizeof(ieee_s));
    s_cnt.device_annce++;
    bool ffd = cap & 0x02, rx_on = cap & 0x08;
    esp_zb_nwk_neighbor_info_t nb;
    bool is_nb = lookup_neighbor(short_addr, &nb);
    bool is_child = is_nb && is_child_rel(nb.relationship);
    if (is_child) s_cnt.child_annce++;
    ESP_LOGI(TAG, "Device announce 0x%04hx %s cap=0x%02x (%s, %s)%s", short_addr, ieee_s, cap,
             ffd ? "router-capable" : "end device", rx_on ? "rx-on" : "SLEEPY",
             is_child ? "  <== CHILD OF THIS REPEATER" : (is_nb ? "  (neighbour)" : ""));
    wb_diag_event("ANNCE 0x%04x %s %s%s", short_addr, ieee_s, rx_on ? "rx-on" : "sleepy",
                  is_child ? " CHILD-OF-US" : "");
}

void wb_diag_on_device_associated(const uint8_t ieee[8])
{
    char ieee_s[24];
    wb_format_ieee(ieee, ieee_s, sizeof(ieee_s));
    s_cnt.child_associations++;
    ESP_LOGI(TAG, "Device %s ASSOCIATED with this router (MAC association: this repeater is its parent)", ieee_s);
    wb_diag_event("ASSOCIATED child %s", ieee_s);
}

void wb_diag_on_nlme_status(uint8_t status, uint16_t addr)
{
    s_cnt.nlme[status <= NLME_CODES ? status : NLME_CODES]++;
    ESP_LOGW(TAG, "NLME-NWK-STATUS %s (0x%02x) for 0x%04hx", wb_nwk_cmd_status_str(status), status, addr);
    wb_diag_event("NWK-STATUS %s for 0x%04x", wb_nwk_cmd_status_str(status), addr);
}

/* ------------------------------------------------------------------------- */
/* CLI: status / network / tables                                            */
/* ------------------------------------------------------------------------- */

#define WITH_LOCK_OR_RETURN()                                                         \
    do {                                                                              \
        if (!wb_zb_lock(LOCK_TIMEOUT_MS)) {                                           \
            printf("ERROR: Zigbee stack busy (lock timeout); try again\n");           \
            return;                                                                   \
        }                                                                             \
    } while (0)

void wb_diag_cmd_status(void)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    const esp_app_desc_t *app = esp_app_get_description();
    WITH_LOCK_OR_RETURN();
    wb_link_summary_t ls;
    wb_diag_link_summary(&ls);
    bool joined = esp_zb_bdb_dev_joined();
    uint16_t sa = joined ? esp_zb_get_short_address() : 0xFFFF;
    uint16_t pan = joined ? esp_zb_get_pan_id() : 0xFFFF;
    uint8_t ch = joined ? esp_zb_get_current_channel() : 0;
    wb_verdict_t v = classify_now(&ls);
    wb_zb_unlock();

    printf("STATUS\n");
    printf("  Firmware   : %s\n", app->version);
    printf("  Uptime     : %lld s\n", (long long)(now_ms() / 1000));
    printf("  State      : %s%s\n", wb_net_state_str(st.state), st.restored_from_nvram ? " (restored from NVRAM)" : "");
    if (joined) printf("  Network    : PAN 0x%04x ch %u, short 0x%04x\n", pan, ch, sa);
    printf("  Links      : %u neighbours (%u router/coord), %u children (%u sleepy), %u active routes\n",
           ls.total_neighbors, ls.router_neighbors, ls.children, ls.sleepy_children, ls.routes_active);
    if (st.isolated_since_ms) printf("  WARNING    : no router neighbour for %lld s\n",
                                     (long long)((now_ms() - st.isolated_since_ms) / 1000));
    printf("  Verdict    : %s\n  Advice     : %s\n", wb_verdict_name(v), wb_verdict_advice(v));
    printf("  Heap       : %" PRIu32 " free, %" PRIu32 " min\n", (uint32_t)esp_get_free_heap_size(),
           (uint32_t)esp_get_minimum_free_heap_size());
}

void wb_diag_cmd_network(void)
{
    WITH_LOCK_OR_RETURN();
    print_network_locked("NETWORK");
    wb_zb_unlock();
}

void wb_diag_cmd_neighbors(void)
{
    esp_zb_nwk_neighbor_info_t *nb = calloc(CONFIG_WB_NETWORK_SIZE, sizeof(*nb));
    if (!nb) { printf("ERROR: out of memory\n"); return; }
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { free(nb); printf("ERROR: Zigbee stack busy\n"); return; }
    int n = collect_neighbors(nb, CONFIG_WB_NETWORK_SIZE);
    wb_zb_unlock();

    printf("NEIGHBOR TABLE (%d entries) - from esp_zb_nwk_get_next_neighbor()\n", n);
    printf("  %-6s %-23s %-4s %-12s %-5s %-4s %-5s %-4s %-3s %-5s\n", "short", "ieee", "type", "relation", "rxOn",
           "lqi", "rssi", "cost", "age", "depth");
    for (int i = 0; i < n; ++i) {
        char ieee[24];
        wb_format_ieee(nb[i].ieee_addr, ieee, sizeof(ieee));
        printf("  0x%04x %-23s %-4s %-12s %-5s %-4u %-5d %-4u %-3u %-5u%s\n", nb[i].short_addr, ieee,
               wb_device_type_str(nb[i].device_type), wb_relationship_str(nb[i].relationship),
               nb[i].rx_on_when_idle ? "yes" : "no", nb[i].lqi, nb[i].rssi, nb[i].outgoing_cost, nb[i].age,
               nb[i].depth, nb[i].short_addr == 0x0000 ? "  <- coordinator (hub)" : "");
    }
    if (n == 0) printf("  (empty)\n");
    printf("  Notes: lqi/rssi are for frames received from that neighbour. 'cost' is the outgoing link cost\n"
           "  reported in the neighbour's link status (0 = unknown). Children appear here with relation 'child'.\n");
    free(nb);
}

void wb_diag_cmd_routes(void)
{
    esp_zb_nwk_route_info_t *rt = calloc(CONFIG_WB_NETWORK_SIZE, sizeof(*rt));
    esp_zb_nwk_route_record_info_t *rr = calloc(CONFIG_WB_NETWORK_SIZE, sizeof(*rr));
    if (!rt || !rr) { free(rt); free(rr); printf("ERROR: out of memory\n"); return; }
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { free(rt); free(rr); printf("ERROR: Zigbee stack busy\n"); return; }
    int n = collect_routes(rt, CONFIG_WB_NETWORK_SIZE);
    int m = collect_route_records(rr, CONFIG_WB_NETWORK_SIZE);
    wb_zb_unlock();

    printf("ROUTING TABLE (%d entries) - from esp_zb_nwk_get_next_route()\n", n);
    printf("  %-6s %-8s %-12s %-4s %-6s %-6s\n", "dest", "nextHop", "state", "MTO", "expiry", "relay?");
    for (int i = 0; i < n; ++i) {
        const char *relay = rt[i].next_hop_addr == rt[i].dest_addr ? "direct" : "VIA";
        printf("  0x%04x 0x%04x   %-12s %-4s %-6u %-6s%s\n", rt[i].dest_addr, rt[i].next_hop_addr,
               wb_route_state_str(rt[i].flags.status), rt[i].flags.many_to_one ? "yes" : "no", rt[i].expiry, relay,
               rt[i].dest_addr == 0x0000 ? "  <- route to hub" : "");
    }
    if (n == 0) printf("  (empty - routes are created on demand; run 'ping 0x0000' to exercise one)\n");

    printf("ROUTE RECORDS (%d entries) - source routes known to this node\n", m);
    for (int i = 0; i < m; ++i) {
        printf("  dest 0x%04x relays=%u path:", rr[i].dest_address, rr[i].relay_count);
        for (int j = 0; j < rr[i].relay_count && j < ESP_ZB_NWK_MAX_SOURCE_ROUTE; ++j) printf(" 0x%04x", rr[i].path[j]);
        printf("\n");
    }
    if (m == 0) printf("  (empty - normal for a router; the hub (concentrator) holds source routes)\n");
    free(rt);
    free(rr);
}

void wb_diag_cmd_children(void)
{
    esp_zb_nwk_neighbor_info_t *nb = calloc(CONFIG_WB_NETWORK_SIZE, sizeof(*nb));
    if (!nb) { printf("ERROR: out of memory\n"); return; }
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { free(nb); printf("ERROR: Zigbee stack busy\n"); return; }
    int n = collect_neighbors(nb, CONFIG_WB_NETWORK_SIZE);
    uint8_t max_children = esp_zb_nwk_get_max_children();
    wb_zb_unlock();

    int c = 0;
    printf("CHILD DEVICES (devices using THIS repeater as their parent)\n");
    printf("  %-6s %-23s %-4s %-12s %-6s %-4s %-5s %-10s %-12s\n", "short", "ieee", "type", "relation", "sleepy",
           "lqi", "rssi", "timeout_s", "aging_ms");
    for (int i = 0; i < n; ++i) {
        if (!is_child_rel(nb[i].relationship) && nb[i].relationship != ESP_ZB_NWK_RELATIONSHIP_PREVIOUS_CHILD) continue;
        char ieee[24];
        wb_format_ieee(nb[i].ieee_addr, ieee, sizeof(ieee));
        printf("  0x%04x %-23s %-4s %-12s %-6s %-4u %-5d %-10" PRIu32 " %-12" PRIu32 "\n", nb[i].short_addr, ieee,
               wb_device_type_str(nb[i].device_type), wb_relationship_str(nb[i].relationship),
               nb[i].rx_on_when_idle ? "no" : "YES", nb[i].lqi, nb[i].rssi, nb[i].device_timeout,
               nb[i].timeout_counter);
        if (is_child_rel(nb[i].relationship)) c++;
    }
    printf("  %d active child(ren) of max %u.\n", c, max_children);
    printf("  Limitation: the SDK exposes no per-child poll counters or buffered-frame counts.\n");
    free(nb);
}

void wb_diag_cmd_events(void)
{
    if (!s_event_mtx || xSemaphoreTake(s_event_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("ERROR: event log busy\n");
        return;
    }
    uint32_t total = s_event_head;
    uint32_t first = total > EVENT_COUNT ? total - EVENT_COUNT : 0;
    printf("EVENTS (%" PRIu32 " total, showing last %" PRIu32 ")\n", total, total - first);
    for (uint32_t i = first; i < total; ++i) {
        const wb_event_t *e = &s_events[i % EVENT_COUNT];
        printf("  [%8lld.%03lld] %s\n", (long long)(e->t_ms / 1000), (long long)(e->t_ms % 1000), e->text);
    }
    xSemaphoreGive(s_event_mtx);
}

void wb_diag_cmd_diagnostics(void)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    WITH_LOCK_OR_RETURN();
    wb_link_summary_t ls;
    wb_diag_link_summary(&ls);
    wb_verdict_t v = classify_now(&ls);
    wb_zb_unlock();

    printf("DIAGNOSTICS\n");
    printf("  Verdict              : %s\n", wb_verdict_name(v));
    printf("  Advice               : %s\n", wb_verdict_advice(v));
    printf("  State                : %s\n", wb_net_state_str(st.state));
    printf("  Join attempted       : %s (attempts %" PRIu32 ", ok %" PRIu32 ", fail %" PRIu32 ")\n",
           st.join_attempted ? "yes" : "no", s_cnt.steering_attempts, s_cnt.steering_ok, s_cnt.steering_fail);
    printf("    fail NO_NETWORK    : %" PRIu32 "  (no joinable network found / join refused)\n", s_cnt.steering_fail_no_network);
    printf("    fail TCLK_EX       : %" PRIu32 "  (link-key exchange with trust centre failed)\n", s_cnt.steering_fail_tclk);
    printf("    fail other         : %" PRIu32 "\n", s_cnt.steering_fail_other);
    printf("  Last BDB status      : %s\n", wb_bdb_status_str(st.last_bdb_status));
    printf("  Removed after join   : %s\n", st.removed_after_join ? "YES" : "no");
    if (s_scan.done)
        printf("  Last scan            : %u PAN(s), %u permit-join, %u with router capacity (%lld s ago)\n",
               s_scan.networks, s_scan.permit_join, s_scan.router_capacity,
               (long long)((now_ms() - s_scan.at_ms) / 1000));
    else
        printf("  Last scan            : none since last join request ('scan' when not joined)\n");
    printf("  Coordinator ping     : %s\n", !s_coord_ping.valid ? "never" : (s_coord_ping.ok ? "OK" : "FAILED"));
    if (s_coord_ping.valid)
        printf("                         %lld s ago, rtt %" PRIu32 " ms\n",
               (long long)((now_ms() - s_coord_ping.at_ms) / 1000), s_coord_ping.rtt_ms);
    printf("  Rejoins/restores     : %" PRIu32 "\n", s_cnt.rejoins);
    printf("  Leaves (self)        : %" PRIu32 "\n", s_cnt.leaves);
    printf("  Leave indications    : %" PRIu32 "\n", s_cnt.leave_indications);
    printf("  Device announces     : %" PRIu32 " (%" PRIu32 " from our children)\n", s_cnt.device_annce, s_cnt.child_annce);
    printf("  Child associations   : %" PRIu32 "\n", s_cnt.child_associations);
    printf("  Permit-join opened   : %" PRIu32 " time(s)\n", s_cnt.permit_join_open);
    printf("  No-active-links      : %" PRIu32 "\n", s_cnt.no_active_links);
    printf("  Ping ok/fail         : %" PRIu32 "/%" PRIu32 "\n", s_cnt.ping_ok, s_cnt.ping_fail);
    printf("  NWK status indications:\n");
    bool any = false;
    for (int i = 0; i <= NLME_CODES; ++i) {
        if (s_cnt.nlme[i]) {
            printf("    %-28s %" PRIu32 "\n", i < NLME_CODES ? wb_nwk_cmd_status_str((uint8_t)i) : "OTHER", s_cnt.nlme[i]);
            any = true;
        }
    }
    if (!any) printf("    (none)\n");
    printf("  Not available via public SDK API: relayed-frame counters, MAC retry/CCA failure counters,\n"
           "  per-child poll statistics. Use a sniffer capture for these (docs/protocol-investigation.md).\n");
}

void wb_diag_cmd_multihop(void)
{
    WITH_LOCK_OR_RETURN();
    esp_zb_nwk_neighbor_info_t *nb; esp_zb_nwk_route_info_t *rt; int nn, nr;
    if (!gather(&nb, &nn, &rt, &nr)) { wb_zb_unlock(); printf("ERROR: out of memory\n"); return; }
    bool joined = esp_zb_bdb_dev_joined();
    uint16_t self = esp_zb_get_short_address();
    wb_link_summary_t ls;
    summarize(nb, nn, rt, nr, &ls);
    esp_zb_nwk_neighbor_info_t nh = {0};
    bool nh_known = false;
    uint8_t nh_ieee[8] = {0};
    if (ls.route_to_coord_next_hop != 0xFFFF) {
        for (int i = 0; i < nn; ++i) {
            if (nb[i].short_addr == ls.route_to_coord_next_hop) { nh = nb[i]; nh_known = true; }
        }
        if (!nh_known) esp_zb_ieee_address_by_short(ls.route_to_coord_next_hop, nh_ieee);
    }
    wb_verdict_t v = classify_now(&ls);
    wb_zb_unlock();

    printf("MULTI-HOP EVIDENCE (local view of short 0x%04x)\n", self);
    if (!joined) { printf("  Not joined.\n"); free(nb); free(rt); return; }

    printf("  1) Hub (0x0000) as direct radio neighbour: %s", ls.coordinator_is_neighbor ? "YES" : "no");
    if (ls.coordinator_is_neighbor) printf(" (lqi %u, rssi %d)", ls.coordinator_lqi, ls.coordinator_rssi);
    printf("\n");
    if (ls.route_to_coord_next_hop == 0xFFFF) {
        printf("  2) Routing entry to hub: none yet (run 'ping 0x0000'; the hub's many-to-one route requests\n"
               "     also create one periodically)\n");
    } else {
        char ieee[24];
        if (nh_known) wb_format_ieee(nh.ieee_addr, ieee, sizeof(ieee));
        else wb_format_ieee(nh_ieee, ieee, sizeof(ieee));
        printf("  2) Routing entry to hub: next hop 0x%04x (%s) state %s%s\n", ls.route_to_coord_next_hop, ieee,
               wb_route_state_str(ls.route_to_coord_state), ls.route_to_coord_many_to_one ? " many-to-one" : "");
        if (nh_known)
            printf("     next hop link: type %s lqi %u rssi %d cost %u\n", wb_device_type_str(nh.device_type), nh.lqi,
                   nh.rssi, nh.outgoing_cost);
        printf("     => traffic to the hub is %s\n",
               ls.route_to_coord_relayed ? "RELAYED via another router (multi-hop)" : "sent DIRECTLY to the hub");
    }
    printf("  3) Coordinator ping: %s\n", !s_coord_ping.valid ? "not run (use 'ping 0x0000')"
                                       : (s_coord_ping.ok ? "answered (end-to-end delivery confirmed)" : "FAILED"));
    printf("  4) Children using this repeater: %u (%u sleepy)\n", ls.children, ls.sleepy_children);
    printf("  Verdict: %s\n", wb_verdict_name(v));
    printf("  Caveats:\n"
           "   - A route entry shows the stack's CURRENT choice. Routes change; repeat over time.\n"
           "   - If (1) is YES with good LQI, the stack may bypass the WA104. For a clean multi-hop test,\n"
           "     place the C6 where the hub is NOT a neighbour (see docs/multihop-testing.md).\n"
           "   - Confirm the next hop is the WA104: compare its IEEE with the WA104 label/Z2M, or 'interview'.\n"
           "   - 'lqi 0x0000' shows the hub's own neighbour view; 'lqi <WA104>' should list this repeater.\n"
           "   - Independent proof: sniffer capture showing MAC src != NWK src on frames (docs).\n");
    free(nb);
    free(rt);
}

/* ------------------------------------------------------------------------- */
/* On-air operations: one at a time                                          */
/* ------------------------------------------------------------------------- */

typedef enum { OP_NONE = 0, OP_SCAN, OP_PING, OP_LQI, OP_INTERVIEW } wb_op_t;
static volatile wb_op_t s_op;
static volatile int64_t s_op_started_ms;

static bool op_begin(wb_op_t op, const char **why)
{
    if (s_op != OP_NONE && (now_ms() - s_op_started_ms) < OP_STUCK_MS) {
        *why = "another scan/ping/lqi/interview is in progress";
        return false;
    }
    s_op = op;
    s_op_started_ms = now_ms();
    return true;
}

static void op_end(void) { s_op = OP_NONE; }

/* --- scan --- */

static void scan_cb(esp_zb_zdp_status_t status, uint8_t count, esp_zb_network_descriptor_t *nd)
{
    s_scan.done = true;
    s_scan.at_ms = now_ms();
    s_scan.networks = 0;
    s_scan.permit_join = 0;
    s_scan.router_capacity = 0;
    printf("SCAN RESULT: %s, %u network(s)\n", wb_zdp_status_str((uint8_t)status), count);
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        printf("  %-6s %-23s %-3s %-6s %-9s %-7s\n", "PAN", "extended PAN ID", "ch", "permit", "routerCap", "edCap");
        for (int i = 0; i < count; ++i) {
            char epid[24];
            wb_format_ieee(nd[i].extended_pan_id, epid, sizeof(epid));
            printf("  0x%04x %-23s %-3u %-6s %-9s %-7s\n", nd[i].short_pan_id, epid, nd[i].logic_channel,
                   nd[i].permit_joining ? "OPEN" : "closed", nd[i].router_capacity ? "yes" : "no",
                   nd[i].end_device_capacity ? "yes" : "no");
            s_scan.networks++;
            if (nd[i].permit_joining) {
                s_scan.permit_join++;
                if (nd[i].router_capacity) s_scan.router_capacity++;
            }
        }
        printf("  Notes: one line per PAN (aggregated over beacons). Your Wiser PAN is the one whose channel/EPID\n"
               "  matches the hub; neighbours' networks may also appear. Pin it with 'epid <EPID>'.\n");
    }
    wb_diag_event("SCAN %u PAN(s), %u permit-join, %u router-capacity", s_scan.networks, s_scan.permit_join,
                  s_scan.router_capacity);
    wb_verdict_t v = classify_now(NULL);
    printf("  Verdict now: %s - %s\n", wb_verdict_name(v), wb_verdict_advice(v));
    op_end();
}

void wb_diag_start_scan_internal(bool automatic)
{
    const char *why = "";
    if (!op_begin(OP_SCAN, &why)) {
        ESP_LOGW(TAG, "Scan not started: %s", why);
        return;
    }
    if (automatic) ESP_LOGI(TAG, "Automatic active scan (beacon request on all configured channels)...");
    esp_zb_zdo_active_scan_request(wb_zb_effective_channel_mask(), 4, scan_cb);
}

esp_err_t wb_diag_cmd_scan(bool force, const char **why)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    if (st.state == WB_NS_JOINED && !force) {
        *why = "refused while joined: an active scan leaves the operating channel for several seconds and "
               "children/routed traffic would be missed. Use 'scan force' only on a bench network";
        return ESP_ERR_INVALID_STATE;
    }
    if (st.state == WB_NS_STEERING) { *why = "join in progress"; return ESP_ERR_INVALID_STATE; }
    if (!st.stack_started) { *why = "stack not started"; return ESP_ERR_INVALID_STATE; }
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { *why = "stack busy"; return ESP_ERR_TIMEOUT; }
    if (!op_begin(OP_SCAN, why)) { wb_zb_unlock(); return ESP_ERR_INVALID_STATE; }
    esp_zb_zdo_active_scan_request(wb_zb_effective_channel_mask(), 4, scan_cb);
    wb_zb_unlock();
    *why = "scanning (~4 s per 16 channels)...";
    return ESP_OK;
}

/* --- ping (ZDO Node_Desc_req) --- */

static struct {
    uint16_t addr;
    uint8_t remaining;
    uint8_t sent;
    uint8_t ok;
    int64_t sent_us;
    uint32_t rtt_min, rtt_max, rtt_sum;
} s_ping;

static void ping_send(uint8_t param);

static void ping_cb(esp_zb_zdp_status_t status, uint16_t addr, esp_zb_af_node_desc_t *nd, void *ctx)
{
    (void)ctx;
    uint32_t rtt = (uint32_t)((esp_timer_get_time() - s_ping.sent_us) / 1000);
    bool ok = status == ESP_ZB_ZDP_STATUS_SUCCESS;
    if (ok) {
        s_ping.ok++;
        s_cnt.ping_ok++;
        if (rtt < s_ping.rtt_min) s_ping.rtt_min = rtt;
        if (rtt > s_ping.rtt_max) s_ping.rtt_max = rtt;
        s_ping.rtt_sum += rtt;
        printf("  reply from 0x%04x: Node_Desc_rsp in %" PRIu32 " ms (manuf code 0x%04x)\n", s_ping.addr, rtt,
               nd ? nd->manufacturer_code : 0);
    } else {
        s_cnt.ping_fail++;
        printf("  no reply from 0x%04x: %s after %" PRIu32 " ms\n", s_ping.addr, wb_zdp_status_str((uint8_t)status), rtt);
    }
    (void)addr;
    if (s_ping.addr == 0x0000) {
        s_coord_ping.valid = true;
        s_coord_ping.ok = ok;
        s_coord_ping.at_ms = now_ms();
        s_coord_ping.rtt_ms = rtt;
    }
    if (s_ping.remaining > 0) {
        esp_zb_scheduler_alarm(ping_send, 0, 1000);
        return;
    }
    printf("PING 0x%04x: %u/%u replies", s_ping.addr, s_ping.ok, s_ping.sent);
    if (s_ping.ok)
        printf(", rtt min/avg/max %" PRIu32 "/%" PRIu32 "/%" PRIu32 " ms", s_ping.rtt_min, s_ping.rtt_sum / s_ping.ok,
               s_ping.rtt_max);
    printf("\n");
    wb_diag_event("PING 0x%04x %u/%u", s_ping.addr, s_ping.ok, s_ping.sent);
    if (s_ping.addr == 0x0000) printf("  Run 'multihop' to see which next hop carried this traffic.\n");
    op_end();
}

static void ping_send(uint8_t param)
{
    (void)param;
    esp_zb_zdo_node_desc_req_param_t req = {.dst_nwk_addr = s_ping.addr};
    s_ping.remaining--;
    s_ping.sent++;
    s_ping.sent_us = esp_timer_get_time();
    esp_zb_zdo_node_desc_req(&req, ping_cb, NULL);
}

esp_err_t wb_diag_cmd_ping(uint16_t addr, uint8_t count, const char **why)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    if (st.state != WB_NS_JOINED) { *why = "not joined"; return ESP_ERR_INVALID_STATE; }
    if (count == 0 || count > 20) count = 3;
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { *why = "stack busy"; return ESP_ERR_TIMEOUT; }
    if (!op_begin(OP_PING, why)) { wb_zb_unlock(); return ESP_ERR_INVALID_STATE; }
    memset(&s_ping, 0, sizeof(s_ping));
    s_ping.addr = addr;
    s_ping.remaining = count;
    s_ping.rtt_min = UINT32_MAX;
    printf("PING 0x%04x with ZDO Node_Desc_req x%u (read-only, harmless; sleepy devices may not answer in time)\n",
           addr, count);
    ping_send(0);
    wb_zb_unlock();
    *why = "sent";
    return ESP_OK;
}

/* --- remote neighbour table (Mgmt_Lqi_req) --- */

static uint16_t s_lqi_addr;

static void lqi_cb(const esp_zb_zdo_mgmt_lqi_rsp_t *rsp, void *ctx)
{
    (void)ctx;
    if (!rsp || rsp->status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        printf("LQI 0x%04x: %s\n", s_lqi_addr, rsp ? wb_zdp_status_str(rsp->status) : "no response");
        op_end();
        return;
    }
    uint16_t self = esp_zb_get_short_address();
    if (rsp->start_index == 0) {
        printf("REMOTE NEIGHBOR TABLE of 0x%04x (%u entries) - via ZDO Mgmt_Lqi_req\n", s_lqi_addr,
               rsp->neighbor_table_entries);
        printf("  %-6s %-23s %-4s %-12s %-5s %-6s %-5s %-4s\n", "short", "ieee", "type", "relation", "rxOn",
               "permit", "depth", "lqi");
    }
    for (int i = 0; i < rsp->neighbor_table_list_count; ++i) {
        const esp_zb_zdo_neighbor_table_list_record_t *r = &rsp->neighbor_table_list[i];
        char ieee[24];
        wb_format_ieee(r->extended_addr, ieee, sizeof(ieee));
        printf("  0x%04x %-23s %-4s %-12s %-5s %-6s %-5u %-4u%s\n", r->network_addr, ieee,
               wb_device_type_str(r->device_type), wb_relationship_str(r->relationship),
               r->rx_when_idle == 1 ? "yes" : (r->rx_when_idle == 0 ? "no" : "?"),
               r->permit_join == 1 ? "yes" : (r->permit_join == 0 ? "no" : "?"), r->depth, r->lqi,
               r->network_addr == self ? "  <- THIS REPEATER" : "");
    }
    uint8_t next = rsp->start_index + rsp->neighbor_table_list_count;
    if (rsp->neighbor_table_list_count > 0 && next < rsp->neighbor_table_entries) {
        esp_zb_zdo_mgmt_lqi_req_param_t req = {.start_index = next, .dst_addr = s_lqi_addr};
        esp_zb_zdo_mgmt_lqi_req(&req, lqi_cb, NULL);
        return;
    }
    printf("  'lqi' here is the remote node's estimate of frames received FROM each neighbour.\n");
    op_end();
}

esp_err_t wb_diag_cmd_lqi(uint16_t addr, const char **why)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    if (st.state != WB_NS_JOINED) { *why = "not joined"; return ESP_ERR_INVALID_STATE; }
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { *why = "stack busy"; return ESP_ERR_TIMEOUT; }
    if (!op_begin(OP_LQI, why)) { wb_zb_unlock(); return ESP_ERR_INVALID_STATE; }
    s_lqi_addr = addr;
    esp_zb_zdo_mgmt_lqi_req_param_t req = {.start_index = 0, .dst_addr = addr};
    esp_zb_zdo_mgmt_lqi_req(&req, lqi_cb, NULL);
    wb_zb_unlock();
    *why = "Mgmt_Lqi_req sent (remote may not support it: NOT_SUPPORTED is a valid answer)";
    return ESP_OK;
}

/* --- interview (fingerprint) --- */

#define IV_MAX_EP   16
#define IV_JSON_LEN 2048

static struct {
    uint16_t addr;
    uint8_t eps[IV_MAX_EP];
    uint8_t n_eps;
    uint8_t idx;
    uint8_t basic_ep;
    char json[IV_JSON_LEN];
    size_t len;
    bool first_ep;
    bool se_phase; /* true while the manufacturer-specific (Schneider) Basic read is outstanding */
} s_iv;

/* Schneider Electric's Zigbee manufacturer code, from public sources (zigbee-herdsman manufacturerCode.ts).
 * Used only to *ask* a device for its manufacturer-specific Basic attributes; the Wiser hub reads exactly
 * these (0xE007 ProductIdentifier, 0xE009 ProductModel) from every new device before deciding to keep it.
 * 0xE004 (device serial number) is deliberately not read: it is per-device and must never be copied. */
#define SE_MANUF_CODE 0x105E
static uint16_t s_se_attrs[] = {0xE007, 0xE009, 0xE008, 0xE00A, 0xE00B, 0xE001, 0xE002};

static void iv_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void iv_append(const char *fmt, ...)
{
    if (s_iv.len >= IV_JSON_LEN - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_iv.json + s_iv.len, IV_JSON_LEN - s_iv.len, fmt, ap);
    va_end(ap);
    if (n > 0) s_iv.len += (size_t)n;
    if (s_iv.len >= IV_JSON_LEN) s_iv.len = IV_JSON_LEN - 1;
}

static void iv_finish(const char *note)
{
    iv_append(",\"note\":\"%s\"}", note);
    printf("INTERVIEW 0x%04x complete: %s\n", s_iv.addr, note);
    printf("FINGERPRINT-JSON %s\n", s_iv.json);
    printf("  Feed the line above to tools/fingerprint_from_log.py to populate a fingerprint file.\n");
    op_end();
}

static void iv_read_timeout(uint8_t param)
{
    (void)param;
    if (s_op != OP_INTERVIEW) return;
    if (s_iv.se_phase) {
        printf("  Manufacturer-specific (0x%04x) Basic read timed out\n", SE_MANUF_CODE);
        iv_append(",\"schneider_basic_attributes\":\"timeout\"");
        iv_finish("ok (manufacturer-specific read timed out)");
        return;
    }
    printf("  Basic cluster read timed out\n");
    iv_append(",\"basic_attributes\":\"timeout\"");
    iv_finish("basic read timed out");
}

static void iv_read_se_basic(void)
{
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {.dst_addr_u.addr_short = s_iv.addr, .dst_endpoint = s_iv.basic_ep,
                          .src_endpoint = wb_profile_get()->endpoint},
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        .manuf_specific = 1,
        .manuf_code = SE_MANUF_CODE,
        .attr_number = sizeof(s_se_attrs) / sizeof(s_se_attrs[0]),
        .attr_field = s_se_attrs,
    };
    s_iv.se_phase = true;
    printf("  Reading manufacturer-specific Basic attributes (code 0x%04x: E007 ProductIdentifier, E009 ProductModel, ...) "
           "from endpoint %u...\n", SE_MANUF_CODE, s_iv.basic_ep);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    esp_zb_scheduler_alarm(iv_read_timeout, 0, READ_ATTR_TIMEOUT_MS);
}

static void iv_read_basic(void)
{
    static uint16_t attrs[] = {0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x4000};
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {.dst_addr_u.addr_short = s_iv.addr, .dst_endpoint = s_iv.basic_ep,
                          .src_endpoint = wb_profile_get()->endpoint},
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        .attr_number = sizeof(attrs) / sizeof(attrs[0]),
        .attr_field = attrs,
    };
    printf("  Reading Basic cluster attributes from endpoint %u...\n", s_iv.basic_ep);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    esp_zb_scheduler_alarm(iv_read_timeout, 0, READ_ATTR_TIMEOUT_MS);
}

static void iv_next_simple_desc(void);

static void iv_simple_desc_cb(esp_zb_zdp_status_t status, esp_zb_af_simple_desc_1_1_t *sd, void *ctx)
{
    (void)ctx;
    uint8_t ep = s_iv.eps[s_iv.idx];
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS || !sd) {
        printf("  Simple_Desc ep %u: %s\n", ep, wb_zdp_status_str((uint8_t)status));
    } else {
        printf("  Endpoint %u: profile 0x%04x device 0x%04x v%u\n    in :", sd->endpoint, sd->app_profile_id,
               sd->app_device_id, (unsigned)sd->app_device_version);
        iv_append("%s{\"endpoint\":%u,\"profile_id\":\"0x%04x\",\"device_id\":\"0x%04x\",\"device_version\":%u,"
                  "\"input_clusters\":[",
                  s_iv.first_ep ? "" : ",", sd->endpoint, sd->app_profile_id, sd->app_device_id,
                  (unsigned)sd->app_device_version);
        s_iv.first_ep = false;
        for (int i = 0; i < sd->app_input_cluster_count; ++i) {
            uint16_t c = sd->app_cluster_list[i];
            printf(" 0x%04x", c);
            iv_append("%s\"0x%04x\"", i ? "," : "", c);
            if (c == ESP_ZB_ZCL_CLUSTER_ID_BASIC && !s_iv.basic_ep) s_iv.basic_ep = sd->endpoint;
        }
        printf("\n    out:");
        iv_append("],\"output_clusters\":[");
        for (int i = 0; i < sd->app_output_cluster_count; ++i) {
            uint16_t c = sd->app_cluster_list[sd->app_input_cluster_count + i];
            printf(" 0x%04x", c);
            iv_append("%s\"0x%04x\"", i ? "," : "", c);
        }
        printf("\n");
        iv_append("]}");
    }
    s_iv.idx++;
    iv_next_simple_desc();
}

static void iv_next_simple_desc(void)
{
    if (s_iv.idx < s_iv.n_eps) {
        esp_zb_zdo_simple_desc_req_param_t req = {.addr_of_interest = s_iv.addr, .endpoint = s_iv.eps[s_iv.idx]};
        esp_zb_zdo_simple_desc_req(&req, iv_simple_desc_cb, NULL);
        return;
    }
    iv_append("]"); /* close "endpoints" */
    if (s_iv.basic_ep) {
        iv_read_basic();
    } else {
        iv_finish("no Basic server cluster found");
    }
}

static void iv_active_ep_cb(esp_zb_zdp_status_t status, uint8_t count, uint8_t *list, void *ctx)
{
    (void)ctx;
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        printf("  Active_EP: %s\n", wb_zdp_status_str((uint8_t)status));
        iv_finish("active endpoint request failed");
        return;
    }
    s_iv.n_eps = count > IV_MAX_EP ? IV_MAX_EP : count;
    printf("  Active endpoints (%u):", count);
    for (int i = 0; i < s_iv.n_eps; ++i) {
        s_iv.eps[i] = list[i];
        printf(" %u", list[i]);
    }
    printf("\n");
    iv_append(",\"endpoints\":[");
    s_iv.first_ep = true;
    s_iv.idx = 0;
    iv_next_simple_desc();
}

static void iv_node_desc_cb(esp_zb_zdp_status_t status, uint16_t addr, esp_zb_af_node_desc_t *nd, void *ctx)
{
    (void)ctx;
    (void)addr;
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS || !nd) {
        printf("  Node_Desc: %s\n", wb_zdp_status_str((uint8_t)status));
        iv_append("\"node_descriptor\":null");
    } else {
        uint8_t logical = nd->node_desc_flags & 0x07;
        uint8_t stack_rev = (uint8_t)((nd->server_mask >> 9) & 0x7F);
        printf("  Node descriptor: logical type %s, manufacturer code 0x%04x, MAC capability 0x%02x\n",
               wb_device_type_str(logical), nd->manufacturer_code, nd->mac_capability_flags);
        printf("    server mask 0x%04x (stack compliance revision %u%s), max buf %u, max in/out %u/%u\n",
               nd->server_mask, stack_rev, stack_rev >= 21 ? " => R21+/Zigbee 3.0 capable" : " => pre-R21 or unset",
               nd->max_buf_size, nd->max_incoming_transfer_size, nd->max_outgoing_transfer_size);
        iv_append("\"node_descriptor\":{\"logical_type\":\"%s\",\"manufacturer_code\":\"0x%04x\","
                  "\"mac_capability_flags\":\"0x%02x\",\"server_mask\":\"0x%04x\",\"stack_compliance_revision\":%u,"
                  "\"max_buffer_size\":%u,\"max_incoming_transfer_size\":%u,\"max_outgoing_transfer_size\":%u,"
                  "\"node_desc_flags\":\"0x%04x\"}",
                  wb_device_type_str(logical), nd->manufacturer_code, nd->mac_capability_flags, nd->server_mask,
                  stack_rev, nd->max_buf_size, nd->max_incoming_transfer_size, nd->max_outgoing_transfer_size,
                  nd->node_desc_flags);
    }
    esp_zb_zdo_active_ep_req_param_t req = {.addr_of_interest = s_iv.addr};
    esp_zb_zdo_active_ep_req(&req, iv_active_ep_cb, NULL);
}

static void json_escape_append(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') iv_append("\\%c", c);
        else if (c < 0x20 || c > 0x7e) iv_append("\\u%04x", c);
        else iv_append("%c", c);
    }
}

void wb_diag_on_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    if (!msg) return;
    if (s_op != OP_INTERVIEW || msg->info.src_address.u.short_addr != s_iv.addr ||
        msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_BASIC) {
        ESP_LOGI(TAG, "Read-attr response from 0x%04x cluster 0x%04x (not part of an interview)",
                 msg->info.src_address.u.short_addr, msg->info.cluster);
        return;
    }
    esp_zb_scheduler_alarm_cancel(iv_read_timeout, 0);
    if (s_iv.se_phase) {
        printf("  Manufacturer-specific Basic attributes (code 0x%04x):\n", SE_MANUF_CODE);
        iv_append(",\"schneider_manufacturer_code\":\"0x%04x\",\"schneider_basic_attributes\":{", SE_MANUF_CODE);
    } else {
        printf("  Basic cluster attributes:\n");
        iv_append(",\"basic_attributes\":{");
    }
    bool first = true;
    for (esp_zb_zcl_read_attr_resp_variable_t *v = msg->variables; v; v = v->next) {
        const esp_zb_zcl_attribute_t *a = &v->attribute;
        if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            printf("    0x%04x: %s\n", a->id, wb_zcl_status_str((uint8_t)v->status));
            continue;
        }
        iv_append("%s\"0x%04x\":", first ? "" : ",", a->id);
        first = false;
        const uint8_t *val = (const uint8_t *)a->data.value;
        if ((a->data.type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) && val) {
            uint8_t n = val[0];
            if (n == 0xFF) n = 0; /* invalid/empty string */
            printf("    0x%04x: \"%.*s\"\n", a->id, n, (const char *)(val + 1));
            iv_append("\"");
            json_escape_append(val + 1, n);
            iv_append("\"");
        } else if (val && a->data.size == 1) {
            printf("    0x%04x: %u (type 0x%02x)\n", a->id, val[0], a->data.type);
            iv_append("%u", val[0]);
        } else if (val && a->data.size == 2) {
            uint16_t x = (uint16_t)(val[0] | (val[1] << 8));
            printf("    0x%04x: 0x%04x (type 0x%02x)\n", a->id, x, a->data.type);
            iv_append("%u", x);
        } else {
            printf("    0x%04x: <type 0x%02x, %u bytes>\n", a->id, a->data.type, a->data.size);
            iv_append("\"<type 0x%02x>\"", a->data.type);
        }
    }
    iv_append("}");
    if (!s_iv.se_phase) {
        iv_read_se_basic(); /* second pass: the attributes the Wiser hub actually checks */
        return;
    }
    iv_finish("ok");
}

esp_err_t wb_diag_cmd_interview(uint16_t addr, const char **why)
{
    wb_zb_status_t st;
    wb_zb_get_status(&st);
    if (st.state != WB_NS_JOINED) { *why = "not joined"; return ESP_ERR_INVALID_STATE; }
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) { *why = "stack busy"; return ESP_ERR_TIMEOUT; }
    if (!op_begin(OP_INTERVIEW, why)) { wb_zb_unlock(); return ESP_ERR_INVALID_STATE; }
    memset(&s_iv, 0, sizeof(s_iv));
    s_iv.addr = addr;
    uint8_t ieee[8] = {0};
    char ieee_s[24] = "unknown";
    if (esp_zb_ieee_address_by_short(addr, ieee) == ESP_OK && !wb_ieee_is_unset(ieee)) {
        wb_format_ieee(ieee, ieee_s, sizeof(ieee_s));
    }
    /* IEEE is recorded only so the operator can match the device; do not copy it into a profile. */
    iv_append("{\"short_addr\":\"0x%04x\",\"ieee_addr_do_not_copy\":\"%s\",", addr, ieee_s);
    printf("INTERVIEW 0x%04x (%s): Node_Desc -> Active_EP -> Simple_Desc -> Basic attributes\n", addr, ieee_s);
    esp_zb_zdo_node_desc_req_param_t req = {.dst_nwk_addr = addr};
    esp_zb_zdo_node_desc_req(&req, iv_node_desc_cb, NULL);
    wb_zb_unlock();
    *why = "started";
    return ESP_OK;
}

bool wb_diag_resolve_addr(const char *s, uint16_t *out)
{
    if (!s) return false;
    if (strlen(s) <= 6 && wb_parse_u16(s, out)) return true;
    uint8_t ieee[8];
    if (!wb_parse_ieee(s, ieee)) return false;
    if (!wb_zb_lock(LOCK_TIMEOUT_MS)) return false;
    uint16_t a = esp_zb_address_short_by_ieee(ieee);
    wb_zb_unlock();
    if (a == 0xFFFF || a == 0xFFFE) return false;
    *out = a;
    return true;
}
