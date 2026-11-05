/*
 * diag_logic.h - Pure C diagnostic logic (no ESP-IDF dependencies).
 *
 * Everything in this module is host-testable (see tests/host). Numeric codes
 * mirror the esp-zigbee-sdk v1.6 enums; zigbee_router.c static-asserts that the
 * SDK values still match, so a future SDK change breaks the build rather than
 * silently mislabelling diagnostics.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Code tables (values mirror esp-zigbee-sdk v1.6.x) -------------------- */

/* esp_zb_bdb_commissioning_status_t */
#define WB_BDB_STATUS_SUCCESS            0
#define WB_BDB_STATUS_IN_PROGRESS        1
#define WB_BDB_STATUS_NO_NETWORK         3
#define WB_BDB_STATUS_NOT_PERMITTED      9
#define WB_BDB_STATUS_TCLK_EX_FAILURE    10
#define WB_BDB_STATUS_UNKNOWN            0xFF

/* esp_zb_nwk_relationship_t */
#define WB_REL_PARENT                    0
#define WB_REL_CHILD                     1
#define WB_REL_SIBLING                   2
#define WB_REL_NONE                      3
#define WB_REL_PREVIOUS_CHILD            4
#define WB_REL_UNAUTH_CHILD              5

/* esp_zb_nwk_device_type_t */
#define WB_DEV_COORDINATOR               0
#define WB_DEV_ROUTER                    1
#define WB_DEV_END_DEVICE                2

const char *wb_bdb_status_str(uint8_t status);
const char *wb_nwk_cmd_status_str(uint8_t status);
const char *wb_relationship_str(uint8_t rel);
const char *wb_device_type_str(uint8_t type);
const char *wb_zdp_status_str(uint8_t status);
const char *wb_route_state_str(uint8_t state);
const char *wb_zcl_status_str(uint8_t status);

/* --- Address helpers ------------------------------------------------------ */

/* Parse "0x1A2B", "1A2B" or decimal "6699" into a 16-bit value. */
bool wb_parse_u16(const char *s, uint16_t *out);

/*
 * Parse an IEEE address written MSB-first ("00:12:4b:00:01:02:03:04" or
 * "00124b0001020304") into the little-endian byte order used by the SDK.
 */
bool wb_parse_ieee(const char *s, uint8_t out_le[8]);

/* Format a little-endian IEEE address as MSB-first "aa:bb:...:hh". buf >= 24. */
void wb_format_ieee(const uint8_t ieee_le[8], char *buf, size_t len);

/* True if every byte is 0x00 or every byte is 0xFF. */
bool wb_ieee_is_unset(const uint8_t ieee_le[8]);

/* Render a channel mask as e.g. "11-26" or "15,20,25". */
void wb_format_channel_mask(uint32_t mask, char *buf, size_t len);

/* --- Join / routing outcome classification -------------------------------- */

typedef enum {
    WB_VERDICT_STACK_ERROR = 0,
    WB_VERDICT_NOT_COMMISSIONED,
    WB_VERDICT_JOIN_IN_PROGRESS,
    WB_VERDICT_NO_NETWORK_VISIBLE,
    WB_VERDICT_PERMIT_JOIN_CLOSED,
    WB_VERDICT_NO_ROUTER_CAPACITY,
    WB_VERDICT_SECURITY_TCLK_FAILURE,
    WB_VERDICT_JOIN_REFUSED_OR_AUTH_FAILED,
    WB_VERDICT_STEERING_FAILED_UNSCANNED,
    WB_VERDICT_JOINED_THEN_REMOVED,
    WB_VERDICT_JOINED_NO_ROUTING_EVIDENCE,
    WB_VERDICT_JOINED_LINKS_PRESENT,
    WB_VERDICT_JOINED_COORDINATOR_REACHABLE,
    WB_VERDICT_JOINED_MULTIHOP_VERIFIED,
    WB_VERDICT_COUNT
} wb_verdict_t;

typedef struct {
    bool stack_error;            /* stack init/start failed */
    bool steering_active;        /* join window currently open */
    bool join_attempted;         /* at least one steering attempt since boot */
    bool joined;                 /* esp_zb_bdb_dev_joined() */
    bool removed_after_join;     /* left (reset type) soon after a successful join */
    uint8_t last_bdb_status;     /* WB_BDB_STATUS_* from last failed steering */

    bool scan_done;              /* an active scan completed since last join attempt */
    uint8_t scan_networks;       /* PANs seen */
    uint8_t scan_permit_join;    /* PANs advertising permit-join */
    uint8_t scan_router_capacity;/* PANs with permit-join AND router capacity */

    uint8_t router_neighbors;    /* neighbour entries that are routers/coordinator */
    uint8_t children;            /* child entries */
    bool coordinator_ping_ok;    /* last ZDO request to 0x0000 answered */
    bool route_to_coordinator_relayed; /* route entry to 0x0000 with next hop != 0x0000 */
} wb_diag_inputs_t;

wb_verdict_t wb_classify(const wb_diag_inputs_t *in);
const char *wb_verdict_name(wb_verdict_t v);
const char *wb_verdict_advice(wb_verdict_t v);

#ifdef __cplusplus
}
#endif
