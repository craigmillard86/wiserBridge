/*
 * zigbee_router.h - Zigbee Router (ZR) lifecycle: init, commissioning, reset.
 *
 * Built on esp-zigbee-sdk v1.6 (ZBOSS). Routing, route discovery/maintenance,
 * parent/child handling and link status are performed entirely by the stack;
 * this module never manipulates routes or next hops.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_NS_INIT = 0,     /* stack starting */
    WB_NS_IDLE,         /* factory-new, waiting for a join request */
    WB_NS_STEERING,     /* join window open */
    WB_NS_JOINED,       /* member of a network */
    WB_NS_LEAVING,      /* local reset in progress */
    WB_NS_ERROR,        /* stack init/start failure (retrying) */
} wb_net_state_t;

typedef struct {
    wb_net_state_t state;
    bool stack_started;
    bool stack_error;
    bool join_attempted;
    bool removed_after_join;
    bool restored_from_nvram;
    uint8_t last_bdb_status;
    int last_steering_err;
    uint32_t steering_attempts;
    int64_t joined_at_ms;
    int64_t steering_deadline_ms;
    int64_t isolated_since_ms;     /* 0 = has router neighbours */
} wb_zb_status_t;

/* Start the Zigbee task (non-blocking). */
esp_err_t wb_zb_start(void);

/* Snapshot of lifecycle state (safe from any task). */
void wb_zb_get_status(wb_zb_status_t *out);
const char *wb_net_state_str(wb_net_state_t s);

/* Request joining (opens steering window). Refuses if already joined. */
esp_err_t wb_zb_request_join(const char **why);

/* Leave the network (if joined) and wipe Zigbee NVRAM, then reboot. */
esp_err_t wb_zb_request_network_reset(void);

/* Acquire/release the Zigbee stack lock from a non-Zigbee task. */
bool wb_zb_lock(uint32_t timeout_ms);
void wb_zb_unlock(void);

/* Effective configuration */
uint32_t wb_zb_effective_channel_mask(void);
bool wb_zb_effective_tclk_required(void);

#ifdef __cplusplus
}
#endif
