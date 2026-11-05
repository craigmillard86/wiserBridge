/*
 * diag_logic.c - Pure C diagnostic logic. No ESP-IDF includes allowed here.
 */
#include "diag_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *wb_bdb_status_str(uint8_t s)
{
    switch (s) {
    case 0: return "SUCCESS";
    case 1: return "IN_PROGRESS";
    case 2: return "NOT_AA_CAPABLE";
    case 3: return "NO_NETWORK";
    case 4: return "TARGET_FAILURE";
    case 5: return "FORMATION_FAILURE";
    case 6: return "NO_IDENTIFY_QUERY_RESPONSE";
    case 7: return "BINDING_TABLE_FULL";
    case 8: return "NO_SCAN_RESPONSE";
    case 9: return "NOT_PERMITTED";
    case 10: return "TCLK_EX_FAILURE";
    case 11: return "NOT_ON_A_NETWORK";
    case 12: return "ON_A_NETWORK";
    case 13: return "CANCELLED";
    case 14: return "DEV_ANNCE_SEND_FAILURE";
    case 0xFF: return "UNKNOWN";
    default: return "?";
    }
}

const char *wb_nwk_cmd_status_str(uint8_t s)
{
    switch (s) {
    case 0x00: return "NO_ROUTE_AVAILABLE";
    case 0x01: return "TREE_LINK_FAILURE";
    case 0x02: return "NON_TREE_LINK_FAILURE";
    case 0x03: return "LOW_BATTERY";
    case 0x04: return "NO_ROUTING_CAPACITY";
    case 0x05: return "NO_INDIRECT_CAPACITY";
    case 0x06: return "INDIRECT_TRANSACTION_EXPIRY";
    case 0x07: return "TARGET_DEVICE_UNAVAILABLE";
    case 0x08: return "TARGET_ADDRESS_UNALLOCATED";
    case 0x09: return "PARENT_LINK_FAILURE";
    case 0x0a: return "VALIDATE_ROUTE";
    case 0x0b: return "SOURCE_ROUTE_FAILURE";
    case 0x0c: return "MANY_TO_ONE_ROUTE_FAILURE";
    case 0x0d: return "ADDRESS_CONFLICT";
    case 0x0e: return "VERIFY_ADDRESS";
    case 0x0f: return "PAN_ID_UPDATE";
    case 0x10: return "NETWORK_ADDRESS_UPDATE";
    case 0x11: return "BAD_FRAME_COUNTER";
    case 0x12: return "BAD_KEY_SEQUENCE_NUMBER";
    case 0x13: return "UNKNOWN_COMMAND";
    default: return "?";
    }
}

const char *wb_relationship_str(uint8_t r)
{
    switch (r) {
    case WB_REL_PARENT: return "parent";
    case WB_REL_CHILD: return "child";
    case WB_REL_SIBLING: return "sibling";
    case WB_REL_NONE: return "none";
    case WB_REL_PREVIOUS_CHILD: return "prev-child";
    case WB_REL_UNAUTH_CHILD: return "unauth-child";
    default: return "?";
    }
}

const char *wb_device_type_str(uint8_t t)
{
    switch (t) {
    case WB_DEV_COORDINATOR: return "ZC";
    case WB_DEV_ROUTER: return "ZR";
    case WB_DEV_END_DEVICE: return "ZED";
    default: return "?";
    }
}

const char *wb_zdp_status_str(uint8_t s)
{
    switch (s) {
    case 0x00: return "SUCCESS";
    case 0x80: return "INV_REQUESTTYPE";
    case 0x81: return "DEVICE_NOT_FOUND";
    case 0x82: return "INVALID_EP";
    case 0x83: return "NOT_ACTIVE";
    case 0x84: return "NOT_SUPPORTED";
    case 0x85: return "TIMEOUT";
    case 0x86: return "NO_MATCH";
    case 0x88: return "NO_ENTRY";
    case 0x89: return "NO_DESCRIPTOR";
    case 0x8a: return "INSUFFICIENT_SPACE";
    case 0x8b: return "NOT_PERMITTED";
    case 0x8c: return "TABLE_FULL";
    case 0x8d: return "NOT_AUTHORIZED";
    case 0x8e: return "BINDING_TABLE_FULL";
    case 0x8f: return "INVALID_INDEX";
    default: return "?";
    }
}

const char *wb_route_state_str(uint8_t s)
{
    switch (s) {
    case 0: return "ACTIVE";
    case 1: return "DISCOVERING";
    case 2: return "DISC_FAILED";
    case 3: return "INACTIVE";
    default: return "?";
    }
}

const char *wb_zcl_status_str(uint8_t s)
{
    switch (s) {
    case 0x00: return "SUCCESS";
    case 0x01: return "FAILURE";
    case 0x7e: return "NOT_AUTHORIZED";
    case 0x81: return "UNSUP_CLUSTER_COMMAND";
    case 0x86: return "UNSUPPORTED_ATTRIBUTE";
    case 0x87: return "INVALID_VALUE";
    case 0x8b: return "NOT_FOUND";
    case 0x94: return "TIMEOUT";
    case 0xc3: return "UNSUPPORTED_CLUSTER";
    default: return "?";
    }
}

/* ------------------------------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool wb_parse_u16(const char *s, uint16_t *out)
{
    if (!s || !*s || !out) return false;
    char *end = NULL;
    unsigned long v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        if (!s[2]) return false;
        v = strtoul(s + 2, &end, 16);
    } else {
        /* Bare 4-hex-digit strings containing a-f are treated as hex. */
        bool has_alpha = false;
        for (const char *p = s; *p; ++p) {
            if (isalpha((unsigned char)*p)) has_alpha = true;
        }
        v = strtoul(s, &end, has_alpha ? 16 : 10);
    }
    if (!end || *end != '\0' || v > 0xFFFFul) return false;
    *out = (uint16_t)v;
    return true;
}

bool wb_parse_ieee(const char *s, uint8_t out_le[8])
{
    if (!s || !out_le) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint8_t msb_first[8];
    int nibbles = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == ':' || *p == '-') continue;
        int v = hexval((unsigned char)*p);
        if (v < 0 || nibbles >= 16) return false;
        if ((nibbles & 1) == 0) msb_first[nibbles / 2] = (uint8_t)(v << 4);
        else msb_first[nibbles / 2] |= (uint8_t)v;
        nibbles++;
    }
    if (nibbles != 16) return false;
    for (int i = 0; i < 8; ++i) out_le[i] = msb_first[7 - i];
    return true;
}

void wb_format_ieee(const uint8_t ieee_le[8], char *buf, size_t len)
{
    if (!buf || len == 0) return;
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             ieee_le[7], ieee_le[6], ieee_le[5], ieee_le[4],
             ieee_le[3], ieee_le[2], ieee_le[1], ieee_le[0]);
}

bool wb_ieee_is_unset(const uint8_t ieee_le[8])
{
    bool all0 = true, allf = true;
    for (int i = 0; i < 8; ++i) {
        if (ieee_le[i] != 0x00) all0 = false;
        if (ieee_le[i] != 0xFF) allf = false;
    }
    return all0 || allf;
}

void wb_format_channel_mask(uint32_t mask, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    buf[0] = '\0';
    size_t used = 0;
    int ch = 11;
    bool first = true;
    while (ch <= 26) {
        if (!(mask & (1ul << ch))) { ch++; continue; }
        int start = ch;
        while (ch + 1 <= 26 && (mask & (1ul << (ch + 1)))) ch++;
        int n;
        if (ch - start >= 2) n = snprintf(buf + used, len - used, "%s%d-%d", first ? "" : ",", start, ch);
        else if (ch != start) n = snprintf(buf + used, len - used, "%s%d,%d", first ? "" : ",", start, ch);
        else n = snprintf(buf + used, len - used, "%s%d", first ? "" : ",", start);
        if (n < 0 || (size_t)n >= len - used) { buf[len - 1] = '\0'; return; }
        used += (size_t)n;
        first = false;
        ch++;
    }
    if (first) snprintf(buf, len, "none");
}

/* ------------------------------------------------------------------------- */

wb_verdict_t wb_classify(const wb_diag_inputs_t *in)
{
    if (in->stack_error) return WB_VERDICT_STACK_ERROR;

    if (in->joined) {
        if (in->coordinator_ping_ok && in->route_to_coordinator_relayed)
            return WB_VERDICT_JOINED_MULTIHOP_VERIFIED;
        if (in->coordinator_ping_ok) return WB_VERDICT_JOINED_COORDINATOR_REACHABLE;
        if (in->router_neighbors > 0 || in->children > 0) return WB_VERDICT_JOINED_LINKS_PRESENT;
        return WB_VERDICT_JOINED_NO_ROUTING_EVIDENCE;
    }

    if (in->removed_after_join) return WB_VERDICT_JOINED_THEN_REMOVED;
    if (in->steering_active) return WB_VERDICT_JOIN_IN_PROGRESS;
    if (!in->join_attempted) return WB_VERDICT_NOT_COMMISSIONED;
    if (in->last_bdb_status == WB_BDB_STATUS_TCLK_EX_FAILURE) return WB_VERDICT_SECURITY_TCLK_FAILURE;

    if (in->scan_done) {
        if (in->scan_networks == 0) return WB_VERDICT_NO_NETWORK_VISIBLE;
        if (in->scan_permit_join == 0) return WB_VERDICT_PERMIT_JOIN_CLOSED;
        if (in->scan_router_capacity == 0) return WB_VERDICT_NO_ROUTER_CAPACITY;
        return WB_VERDICT_JOIN_REFUSED_OR_AUTH_FAILED;
    }
    return WB_VERDICT_STEERING_FAILED_UNSCANNED;
}

static const struct {
    const char *name;
    const char *advice;
} s_verdicts[WB_VERDICT_COUNT] = {
    [WB_VERDICT_STACK_ERROR] = {"STACK_ERROR",
        "Zigbee stack failed to initialise/start. Check the boot log for the failing call; erase flash and reflash if zb_storage is corrupt."},
    [WB_VERDICT_NOT_COMMISSIONED] = {"NOT_COMMISSIONED",
        "Factory-new. Put the Wiser hub into pairing mode, then type 'join' or short-press BOOT."},
    [WB_VERDICT_JOIN_IN_PROGRESS] = {"JOIN_IN_PROGRESS",
        "Network steering is running. Keep the hub in pairing mode."},
    [WB_VERDICT_NO_NETWORK_VISIBLE] = {"NO_NETWORK_VISIBLE",
        "No Zigbee beacons heard on the scanned channels. Move closer to the hub/WA104 or check the channel mask/antenna."},
    [WB_VERDICT_PERMIT_JOIN_CLOSED] = {"PERMIT_JOIN_CLOSED",
        "Networks are visible but none advertise permit-join. Start pairing on the Wiser hub/app, then retry 'join'."},
    [WB_VERDICT_NO_ROUTER_CAPACITY] = {"NO_ROUTER_CAPACITY",
        "Permit-join is open but no beaconing device accepts new routers. Try nearer a different router or the hub."},
    [WB_VERDICT_SECURITY_TCLK_FAILURE] = {"SECURITY_TCLK_FAILURE",
        "Association and key transport worked but Trust Center link-key exchange failed. The hub may be pre-Zigbee-3.0: try 'tclk off', reboot, retry."},
    [WB_VERDICT_JOIN_REFUSED_OR_AUTH_FAILED] = {"JOIN_REFUSED_OR_AUTH_FAILED",
        "An open network with router capacity was visible but joining failed: association refused, or the trust centre did not deliver/accept the network key (e.g. allow-list or install-code policy). A sniffer capture is needed to tell which."},
    [WB_VERDICT_STEERING_FAILED_UNSCANNED] = {"STEERING_FAILED",
        "Joining failed. Run 'scan' (while not joined) to separate 'no network' from 'permit-join closed' from 'refused'."},
    [WB_VERDICT_JOINED_THEN_REMOVED] = {"JOINED_THEN_REMOVED",
        "Network join succeeded but the device was told to leave shortly afterwards. The hub likely rejected it at application level (unrecognised device)."},
    [WB_VERDICT_JOINED_NO_ROUTING_EVIDENCE] = {"JOINED_NO_ROUTING_EVIDENCE",
        "Joined, but no router neighbours or children yet. Wait for link-status exchange (~15-60 s) and run 'ping 0x0000'."},
    [WB_VERDICT_JOINED_LINKS_PRESENT] = {"JOINED_LINKS_PRESENT",
        "Joined with neighbour links. No end-to-end proof yet: run 'ping 0x0000' and 'multihop'."},
    [WB_VERDICT_JOINED_COORDINATOR_REACHABLE] = {"JOINED_COORDINATOR_REACHABLE",
        "Hub answered a ZDO request. Route appears direct (or unknown): this does not prove multi-hop. See 'multihop'."},
    [WB_VERDICT_JOINED_MULTIHOP_VERIFIED] = {"JOINED_MULTIHOP_VERIFIED",
        "Hub answered a ZDO request and the routing table shows a relay next hop toward 0x0000. Confirm the next hop is the WA104 via 'neighbors'/'lqi'."},
};

const char *wb_verdict_name(wb_verdict_t v)
{
    return (v < WB_VERDICT_COUNT) ? s_verdicts[v].name : "?";
}

const char *wb_verdict_advice(wb_verdict_t v)
{
    return (v < WB_VERDICT_COUNT) ? s_verdicts[v].advice : "";
}
