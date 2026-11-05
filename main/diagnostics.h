/*
 * diagnostics.h - Event log, counters, table dumps and on-air ZDO probes.
 *
 * Only public esp-zigbee-sdk v1.6 APIs are used. Where the SDK does not expose a
 * value (e.g. per-frame relay counters, per-child poll statistics), the
 * corresponding command says so instead of inventing data.
 *
 * Network security material (network key, link keys) is never read or printed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t total_neighbors;
    uint8_t router_neighbors;       /* ZC/ZR neighbours that are not our children */
    uint8_t children;
    uint8_t sleepy_children;        /* rx_on_when_idle == 0 */
    uint8_t routes_active;
    bool coordinator_is_neighbor;
    uint8_t coordinator_lqi;
    int8_t coordinator_rssi;
    uint16_t route_to_coord_next_hop;   /* 0xFFFF = no routing entry */
    uint8_t route_to_coord_state;
    bool route_to_coord_many_to_one;
    bool route_to_coord_relayed;
} wb_link_summary_t;

/* --- called from app_main before the stack starts ------------------------ */
void wb_diag_init(void);
void wb_diag_print_banner(void);

/* --- called from Zigbee task context (no lock needed) --------------------- */
void wb_diag_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wb_diag_count_steering_attempt(void);
void wb_diag_count_steering_result(bool ok, uint8_t bdb_status);
void wb_diag_count_rejoin(void);
void wb_diag_count_leave(void);
void wb_diag_count_leave_indication(void);
void wb_diag_count_permit_join(uint8_t seconds);
void wb_diag_count_no_active_links(void);
void wb_diag_reset_scan_summary(void);
void wb_diag_start_scan_internal(bool automatic);
void wb_diag_link_summary(wb_link_summary_t *out);
void wb_diag_print_health(void);
void wb_diag_log_network(const char *how);
void wb_diag_on_device_annce(uint16_t short_addr, const uint8_t ieee[8], uint8_t capability);
void wb_diag_on_device_associated(const uint8_t ieee[8]);
void wb_diag_on_nlme_status(uint8_t status, uint16_t addr);
void wb_diag_on_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg);

/* --- CLI entry points (take the Zigbee lock internally) ------------------- */
void wb_diag_cmd_status(void);
void wb_diag_cmd_network(void);
void wb_diag_cmd_neighbors(void);
void wb_diag_cmd_routes(void);
void wb_diag_cmd_children(void);
void wb_diag_cmd_diagnostics(void);
void wb_diag_cmd_events(void);
void wb_diag_cmd_multihop(void);
esp_err_t wb_diag_cmd_scan(bool force, const char **why);
esp_err_t wb_diag_cmd_ping(uint16_t addr, uint8_t count, const char **why);
esp_err_t wb_diag_cmd_lqi(uint16_t addr, const char **why);
esp_err_t wb_diag_cmd_interview(uint16_t addr, const char **why);

/* Resolve "0x1234" or an IEEE address string to a short address (takes lock). */
bool wb_diag_resolve_addr(const char *s, uint16_t *short_out);

#ifdef __cplusplus
}
#endif
