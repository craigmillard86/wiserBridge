/*
 * wiser_profile.h - Configurable device identity / compatibility profile.
 *
 * The default profile is generic (Kconfig). It can be overridden at runtime and
 * persisted in NVS. Values for a Wiser-specific profile must come from captured
 * evidence (JSON files in fingerprints/) - nothing here is a guessed Drayton value.
 *
 * Profile changes only take effect after reboot, because the endpoint/simple
 * descriptor is registered once at stack start. If the device is already joined,
 * the hub will keep its earlier interview data until the device rejoins.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WB_PROFILE_STR_MAX        32
#define WB_PROFILE_MAX_EXTRA      8

typedef struct {
    char manufacturer[WB_PROFILE_STR_MAX + 1];
    char model[WB_PROFILE_STR_MAX + 1];
    char date_code[16 + 1];
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t device_version;
    uint16_t node_manufacturer_code;   /* 0 = keep SDK default */
    bool identify;
    uint8_t max_children;
    uint8_t power_source;              /* ZCL Basic PowerSource enum, 0x01 = mains single phase */
    uint8_t extra_in_count;
    uint16_t extra_in[WB_PROFILE_MAX_EXTRA];    /* extra server clusters (empty attribute lists) */
    uint8_t extra_out_count;
    uint16_t extra_out[WB_PROFILE_MAX_EXTRA];   /* extra client clusters */
    char source[WB_PROFILE_STR_MAX + 1];        /* "kconfig-default" or evidence reference */
} wb_profile_t;

/* Load profile (Kconfig defaults, overridden by NVS if present). */
esp_err_t wb_profile_init(void);

/* Current active profile (read-only). */
const wb_profile_t *wb_profile_get(void);

/* Pending (edited, not yet saved) profile for CLI editing. */
wb_profile_t *wb_profile_pending(void);

/* Apply "key value" to the pending profile. Returns ESP_ERR_INVALID_ARG with a message. */
esp_err_t wb_profile_set(const char *key, const char *value, const char **why);

/* Persist pending profile to NVS (takes effect after reboot). */
esp_err_t wb_profile_save(void);

/* Erase NVS override; Kconfig defaults apply after reboot. */
esp_err_t wb_profile_reset_to_defaults(void);

/* Print a profile to the console. */
void wb_profile_print(const wb_profile_t *p, const char *title);

/* --- Persistent runtime options stored next to the profile ---------------- */

/* Optional extended PAN ID filter used when joining (little-endian). */
bool wb_opt_get_epid_filter(uint8_t epid_le[8]);
esp_err_t wb_opt_set_epid_filter(const uint8_t epid_le[8]);  /* NULL clears */

/* Channel mask override (0 = use Kconfig). */
uint32_t wb_opt_get_channel_mask(void);
esp_err_t wb_opt_set_channel_mask(uint32_t mask);

/* TCLK exchange required override: -1 = Kconfig, 0 = off, 1 = on. */
int wb_opt_get_tclk_required(void);
esp_err_t wb_opt_set_tclk_required(int v);

#ifdef __cplusplus
}
#endif
