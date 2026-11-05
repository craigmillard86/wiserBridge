/*
 * aps_inspect.c - observe incoming APS frames (see aps_inspect.h).
 *
 * zb_af_set_data_indication() (esp-zboss-lib, zboss_api_zdo.h) calls us for every APS data frame
 * delivered to this node BEFORE the stack processes it. We decode just enough of the header to
 * name the request, then return ZB_FALSE so the stack handles the frame exactly as it would
 * without us.
 */
#include "aps_inspect.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "diagnostics.h"
#include "esp_log.h"
#include "zboss_api.h"

static const char *TAG = "WB_APSIN";
static volatile bool s_enabled = true;

static const char *zdo_name(uint16_t cl)
{
    switch (cl & 0x7fff) {
    case 0x0000: return "NWK_addr";
    case 0x0001: return "IEEE_addr";
    case 0x0002: return "Node_Desc";
    case 0x0003: return "Power_Desc";
    case 0x0004: return "Simple_Desc";
    case 0x0005: return "Active_EP";
    case 0x0006: return "Match_Desc";
    case 0x0010: return "Complex_Desc";
    case 0x0011: return "User_Desc";
    case 0x0013: return "Device_annce";
    case 0x0015: return "System_Server_Disc";
    case 0x001e: return "Parent_annce";
    case 0x0021: return "Bind";
    case 0x0022: return "Unbind";
    case 0x0031: return "Mgmt_Lqi";
    case 0x0032: return "Mgmt_Rtg";
    case 0x0033: return "Mgmt_Bind";
    case 0x0034: return "Mgmt_Leave";
    case 0x0036: return "Mgmt_Permit_Joining";
    case 0x0038: return "Mgmt_NWK_Update";
    default: return "ZDO?";
    }
}

static const char *zcl_global_name(uint8_t id)
{
    switch (id) {
    case 0x00: return "ReadAttr";
    case 0x01: return "ReadAttrRsp";
    case 0x02: return "WriteAttr";
    case 0x04: return "WriteAttrRsp";
    case 0x06: return "ConfigReport";
    case 0x07: return "ConfigReportRsp";
    case 0x08: return "ReadReportCfg";
    case 0x0a: return "ReportAttr";
    case 0x0b: return "DefaultRsp";
    case 0x0c: return "DiscoverAttr";
    case 0x0d: return "DiscoverAttrRsp";
    case 0x11: return "DiscoverCmdsRecv";
    case 0x13: return "DiscoverCmdsGen";
    case 0x15: return "DiscoverAttrExt";
    default: return "global?";
    }
}

/* APS header length from the frame-control byte (Zigbee spec 2.2.5.1). Read-only; never cuts. */
static size_t aps_hdr_len(const uint8_t *b, size_t len)
{
    if (len < 1) return len;
    uint8_t fc = b[0];
    uint8_t ft = fc & 0x03, dm = (fc >> 2) & 0x03, ackfmt = (fc >> 4) & 1, ext = (fc >> 7) & 1;
    size_t h = 1;
    if (dm == 0 || dm == 2) h += 1;                /* destination endpoint */
    if (dm == 3) h += 2;                            /* group address */
    if (ft == 0 || (ft == 2 && !ackfmt)) h += 5;    /* cluster id, profile id, source endpoint */
    h += 1;                                         /* APS counter */
    if (ext && h < len) {
        uint8_t efc = b[h];
        h += 1;
        if (efc & 0x03) h += 1;                     /* block number */
        if ((efc & 0x03) && ft == 2) h += 1;        /* ack bitfield */
    }
    return h > len ? len : h;
}

static zb_uint8_t data_indication(zb_bufid_t param)
{
    if (!s_enabled) return ZB_FALSE;

    zb_apsde_data_indication_t *ind = ZB_BUF_GET_PARAM(param, zb_apsde_data_indication_t);
    const uint8_t *b = (const uint8_t *)zb_buf_begin(param);
    size_t len = zb_buf_len(param);
    /* The buffer may or may not still carry the APS header; detect it via the parsed fc byte. */
    size_t off = (len > 0 && b[0] == ind->fc) ? aps_hdr_len(b, len) : 0;
    const uint8_t *p = b + off;
    size_t n = len - off;

    char desc[80] = "";
    if (ind->profileid == 0x0000) {
        snprintf(desc, sizeof desc, "ZDO %s_%s", zdo_name(ind->clusterid), (ind->clusterid & 0x8000) ? "rsp" : "req");
        if (ind->clusterid == 0x0004 && n >= 4) {
            size_t l = strlen(desc);
            snprintf(desc + l, sizeof desc - l, " ep=%u", p[3]);
        }
    } else if (n >= 3) {
        uint8_t zfc = p[0];
        bool mfr = (zfc & 0x04) != 0, clspec = (zfc & 0x01) != 0;
        uint16_t code = 0;
        size_t i = 1;
        if (mfr) { code = (uint16_t)(p[1] | (p[2] << 8)); i = 3; }
        if (n >= i + 2) {
            uint8_t cmd = p[i + 1];
            snprintf(desc, sizeof desc, "ZCL %s 0x%02x", clspec ? "cluster-cmd" : zcl_global_name(cmd), cmd);
            if (mfr) {
                size_t l = strlen(desc);
                snprintf(desc + l, sizeof desc - l, " MFR=0x%04x", code);
            }
            if (!clspec && cmd == 0x00) { /* Read Attributes: list the attribute ids asked for */
                strlcat(desc, " attrs=", sizeof desc);
                for (size_t a = i + 2; a + 1 < n && strlen(desc) + 6 < sizeof desc; a += 2) {
                    char t[8];
                    snprintf(t, sizeof t, "%04x,", (unsigned)(p[a] | (p[a + 1] << 8)));
                    strlcat(desc, t, sizeof desc);
                }
            }
        } else {
            snprintf(desc, sizeof desc, "ZCL short frame");
        }
    } else {
        snprintf(desc, sizeof desc, "%u byte payload", (unsigned)n);
    }

    wb_diag_event("APS-IN 0x%04x ep%u>%u cl 0x%04x %s", ind->src_addr, ind->src_endpoint, ind->dst_endpoint,
                  ind->clusterid, desc);

    char hex[3 * 24 + 1];
    size_t hn = n < 24 ? n : 24;
    for (size_t k = 0; k < hn; ++k) sprintf(hex + 3 * k, "%02x ", p[k]);
    hex[3 * hn] = '\0';
    ESP_LOGI(TAG, "  prof 0x%04x len=%u apsfc=0x%02x keysrc=%u keyattr=%u fromTC=%u lqi=%u rssi=%d payload: %s%s",
             ind->profileid, (unsigned)n, ind->fc, (unsigned)ind->aps_key_source, (unsigned)ind->aps_key_attrs,
             (unsigned)ind->aps_key_from_tc, (unsigned)ind->lqi, (int)ind->rssi, hex, n > 24 ? "..." : "");

    return ZB_FALSE; /* never consume: the stack processes the frame normally */
}

void wb_aps_inspect_install(void)
{
    zb_af_set_data_indication(data_indication);
    ESP_LOGI(TAG, "APS inbound inspection installed (log of requests addressed to this node; 'apslog off' to silence)");
}

void wb_aps_inspect_enable(bool on) { s_enabled = on; }
bool wb_aps_inspect_enabled(void) { return s_enabled; }
