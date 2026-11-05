/*
 * wiser_profile.c - Device identity profile with NVS persistence.
 */
#include "wiser_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_logic.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "WB_PROFILE";

#define NVS_NS            "wb_cfg"
#define NVS_KEY_PROFILE   "profile"
#define NVS_KEY_EPID      "epid"
#define NVS_KEY_CHMASK    "chmask"
#define NVS_KEY_TCLK      "tclk"
#define NVS_KEY_RLYGPIO   "rlygpio"
#define NVS_KEY_RLYINV    "rlyinv"
#define PROFILE_MAGIC     0x57425031u /* "WBP1" */

typedef struct {
    uint32_t magic;
    uint32_t size;
    wb_profile_t profile;
} wb_profile_blob_t;

static wb_profile_t s_active;
static wb_profile_t s_pending;

static void profile_defaults(wb_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    strlcpy(p->manufacturer, CONFIG_WB_MANUFACTURER_NAME, sizeof(p->manufacturer));
    strlcpy(p->model, CONFIG_WB_MODEL_IDENTIFIER, sizeof(p->model));
    strlcpy(p->date_code, "", sizeof(p->date_code));
    p->endpoint = CONFIG_WB_ENDPOINT;
    p->profile_id = CONFIG_WB_PROFILE_ID;
    p->device_id = CONFIG_WB_DEVICE_ID;
    p->device_version = CONFIG_WB_DEVICE_VERSION;
    p->node_manufacturer_code = CONFIG_WB_NODE_MANUFACTURER_CODE;
#if CONFIG_WB_ENABLE_IDENTIFY
    p->identify = true;
#endif
    p->max_children = CONFIG_WB_MAX_CHILDREN;
    p->power_source = 0x01;
    strlcpy(p->source, "kconfig-default", sizeof(p->source));
}

static bool profile_valid(const wb_profile_t *p, const char **why)
{
    if (p->endpoint < 1 || p->endpoint > 240) { *why = "endpoint must be 1..240"; return false; }
    if (p->max_children < 1 || p->max_children > 32) { *why = "max_children must be 1..32"; return false; }
    if (p->device_version > 15) { *why = "device_version must be 0..15"; return false; }
    if (p->extra_in_count > WB_PROFILE_MAX_EXTRA || p->extra_out_count > WB_PROFILE_MAX_EXTRA) {
        *why = "too many extra clusters"; return false;
    }
    for (int i = 0; i < p->extra_in_count; ++i) {
        if (p->extra_in[i] == 0x0000 || (p->identify && p->extra_in[i] == 0x0003)) {
            *why = "extra_in must not repeat Basic(0x0000)/Identify(0x0003)"; return false;
        }
    }
    return true;
}

esp_err_t wb_profile_init(void)
{
    profile_defaults(&s_active);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        wb_profile_blob_t blob;
        size_t len = sizeof(blob);
        err = nvs_get_blob(h, NVS_KEY_PROFILE, &blob, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            const char *why = "";
            if (len == sizeof(blob) && blob.magic == PROFILE_MAGIC && blob.size == sizeof(wb_profile_t) &&
                profile_valid(&blob.profile, &why)) {
                blob.profile.manufacturer[WB_PROFILE_STR_MAX] = '\0';
                blob.profile.model[WB_PROFILE_STR_MAX] = '\0';
                blob.profile.date_code[16] = '\0';
                blob.profile.source[WB_PROFILE_STR_MAX] = '\0';
                s_active = blob.profile;
                ESP_LOGI(TAG, "Loaded profile override from NVS (source: %s)", s_active.source);
            } else {
                ESP_LOGW(TAG, "Ignoring invalid/old profile in NVS (%s); using Kconfig defaults", why);
            }
        }
    }
    s_pending = s_active;
    return ESP_OK;
}

const wb_profile_t *wb_profile_get(void) { return &s_active; }
wb_profile_t *wb_profile_pending(void) { return &s_pending; }

static bool parse_num(const char *v, unsigned long max, unsigned long *out)
{
    if (!v || !*v) return false;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 0);
    if (!end || *end != '\0' || n > max) return false;
    *out = n;
    return true;
}

static bool parse_cluster_list(const char *v, uint16_t *list, uint8_t *count)
{
    *count = 0;
    if (!v || strcmp(v, "none") == 0 || strcmp(v, "-") == 0) return true;
    char tmp[128];
    strlcpy(tmp, v, sizeof(tmp));
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        uint16_t id;
        if (*count >= WB_PROFILE_MAX_EXTRA || !wb_parse_u16(tok, &id)) return false;
        list[(*count)++] = id;
    }
    return true;
}

esp_err_t wb_profile_set(const char *key, const char *value, const char **why)
{
    static const char *ok = "ok";
    *why = ok;
    wb_profile_t t = s_pending;
    unsigned long n = 0;

    if (!key || !value) { *why = "usage: profile set <key> <value>"; return ESP_ERR_INVALID_ARG; }

    if (strcmp(key, "manufacturer") == 0) {
        if (strlen(value) > WB_PROFILE_STR_MAX) { *why = "max 32 chars"; return ESP_ERR_INVALID_ARG; }
        strlcpy(t.manufacturer, value, sizeof(t.manufacturer));
    } else if (strcmp(key, "model") == 0) {
        if (strlen(value) > WB_PROFILE_STR_MAX) { *why = "max 32 chars"; return ESP_ERR_INVALID_ARG; }
        strlcpy(t.model, value, sizeof(t.model));
    } else if (strcmp(key, "date_code") == 0) {
        if (strlen(value) > 16) { *why = "max 16 chars"; return ESP_ERR_INVALID_ARG; }
        strlcpy(t.date_code, value, sizeof(t.date_code));
    } else if (strcmp(key, "endpoint") == 0) {
        if (!parse_num(value, 240, &n) || n < 1) { *why = "1..240"; return ESP_ERR_INVALID_ARG; }
        t.endpoint = (uint8_t)n;
    } else if (strcmp(key, "profile_id") == 0) {
        if (!parse_num(value, 0xFFFF, &n)) { *why = "0x0000..0xFFFF"; return ESP_ERR_INVALID_ARG; }
        t.profile_id = (uint16_t)n;
    } else if (strcmp(key, "device_id") == 0) {
        if (!parse_num(value, 0xFFFF, &n)) { *why = "0x0000..0xFFFF"; return ESP_ERR_INVALID_ARG; }
        t.device_id = (uint16_t)n;
    } else if (strcmp(key, "device_version") == 0) {
        if (!parse_num(value, 15, &n)) { *why = "0..15"; return ESP_ERR_INVALID_ARG; }
        t.device_version = (uint8_t)n;
    } else if (strcmp(key, "manufacturer_code") == 0) {
        if (!parse_num(value, 0xFFFF, &n)) { *why = "0x0000..0xFFFF (0 = SDK default)"; return ESP_ERR_INVALID_ARG; }
        t.node_manufacturer_code = (uint16_t)n;
    } else if (strcmp(key, "identify") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) t.identify = true;
        else if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0) t.identify = false;
        else { *why = "on|off"; return ESP_ERR_INVALID_ARG; }
    } else if (strcmp(key, "max_children") == 0) {
        if (!parse_num(value, 32, &n) || n < 1) { *why = "1..32"; return ESP_ERR_INVALID_ARG; }
        t.max_children = (uint8_t)n;
    } else if (strcmp(key, "power_source") == 0) {
        if (!parse_num(value, 0xFF, &n)) { *why = "0x00..0xFF (ZCL Basic PowerSource)"; return ESP_ERR_INVALID_ARG; }
        t.power_source = (uint8_t)n;
    } else if (strcmp(key, "in_clusters") == 0) {
        if (!parse_cluster_list(value, t.extra_in, &t.extra_in_count)) {
            *why = "comma list of up to 8 cluster IDs, e.g. 0x0019,0xFC00 (or 'none')"; return ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(key, "out_clusters") == 0) {
        if (!parse_cluster_list(value, t.extra_out, &t.extra_out_count)) {
            *why = "comma list of up to 8 cluster IDs (or 'none')"; return ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(key, "source") == 0) {
        strlcpy(t.source, value, sizeof(t.source));
    } else {
        *why = "unknown key (manufacturer, model, date_code, endpoint, profile_id, device_id, device_version, "
               "manufacturer_code, identify, max_children, power_source, in_clusters, out_clusters, source)";
        return ESP_ERR_INVALID_ARG;
    }

    if (!profile_valid(&t, why)) return ESP_ERR_INVALID_ARG;
    s_pending = t;
    return ESP_OK;
}

esp_err_t wb_profile_save(void)
{
    const char *why = "";
    if (!profile_valid(&s_pending, &why)) {
        ESP_LOGE(TAG, "Pending profile invalid: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    wb_profile_blob_t blob = {.magic = PROFILE_MAGIC, .size = sizeof(wb_profile_t), .profile = s_pending};
    err = nvs_set_blob(h, NVS_KEY_PROFILE, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wb_profile_reset_to_defaults(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, NVS_KEY_PROFILE);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    profile_defaults(&s_pending);
    return err;
}

void wb_profile_print(const wb_profile_t *p, const char *title)
{
    printf("%s\n", title);
    printf("  manufacturer      : \"%s\"\n", p->manufacturer);
    printf("  model             : \"%s\"\n", p->model);
    printf("  date_code         : \"%s\"\n", p->date_code);
    printf("  endpoint          : %u\n", p->endpoint);
    printf("  profile_id        : 0x%04x\n", p->profile_id);
    printf("  device_id         : 0x%04x\n", p->device_id);
    printf("  device_version    : %u\n", p->device_version);
    printf("  manufacturer_code : 0x%04x%s\n", p->node_manufacturer_code,
           p->node_manufacturer_code ? "" : " (SDK default)");
    printf("  power_source      : 0x%02x\n", p->power_source);
    printf("  identify          : %s\n", p->identify ? "on" : "off");
    printf("  max_children      : %u\n", p->max_children);
    printf("  in_clusters       : 0x0000(Basic)%s", p->identify ? ",0x0003(Identify)" : "");
    for (int i = 0; i < p->extra_in_count; ++i) printf(",0x%04x", p->extra_in[i]);
    printf("\n  out_clusters      : ");
    if (!p->extra_out_count) printf("none");
    for (int i = 0; i < p->extra_out_count; ++i) printf("%s0x%04x", i ? "," : "", p->extra_out[i]);
    printf("\n  source            : %s\n", p->source);
}

/* ------------------------------------------------------------------------- */

static esp_err_t nvs_set_blob_key(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (data) err = nvs_set_blob(h, key, data, len);
    else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_get_blob_key(const char *key, void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t got = len;
    esp_err_t err = nvs_get_blob(h, key, data, &got);
    nvs_close(h);
    return err == ESP_OK && got == len;
}

bool wb_opt_get_epid_filter(uint8_t epid_le[8])
{
    return nvs_get_blob_key(NVS_KEY_EPID, epid_le, 8);
}

esp_err_t wb_opt_set_epid_filter(const uint8_t epid_le[8])
{
    return nvs_set_blob_key(NVS_KEY_EPID, epid_le, 8);
}

uint32_t wb_opt_get_channel_mask(void)
{
    uint32_t m = 0;
    if (!nvs_get_blob_key(NVS_KEY_CHMASK, &m, sizeof(m))) return 0;
    return m;
}

esp_err_t wb_opt_set_channel_mask(uint32_t mask)
{
    return nvs_set_blob_key(NVS_KEY_CHMASK, mask ? &mask : NULL, sizeof(mask));
}

int wb_opt_get_tclk_required(void)
{
    int8_t v = -1;
    if (!nvs_get_blob_key(NVS_KEY_TCLK, &v, sizeof(v))) return -1;
    return v;
}

esp_err_t wb_opt_set_tclk_required(int v)
{
    int8_t b = (int8_t)v;
    return nvs_set_blob_key(NVS_KEY_TCLK, (v < 0) ? NULL : &b, sizeof(b));
}

int wb_opt_get_relay_gpio(void)
{
    int16_t v;
    if (!nvs_get_blob_key(NVS_KEY_RLYGPIO, &v, sizeof(v))) return WB_RELAY_GPIO_UNSET;
    return v;
}

esp_err_t wb_opt_set_relay_gpio(int gpio)
{
    int16_t v = (int16_t)gpio;
    return nvs_set_blob_key(NVS_KEY_RLYGPIO, (gpio == WB_RELAY_GPIO_UNSET) ? NULL : &v, sizeof(v));
}

int wb_opt_get_relay_active_low(void)
{
    int8_t v;
    if (!nvs_get_blob_key(NVS_KEY_RLYINV, &v, sizeof(v))) return -1;
    return v;
}

esp_err_t wb_opt_set_relay_active_low(int v)
{
    int8_t b = (int8_t)v;
    return nvs_set_blob_key(NVS_KEY_RLYINV, (v < 0) ? NULL : &b, sizeof(b));
}
