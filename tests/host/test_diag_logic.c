/*
 * Host unit tests for main/diag_logic.c (pure C, no ESP-IDF).
 * Build & run:  make -C tests/host   (or tests/run_all.sh)
 */
#include <stdio.h>
#include <string.h>

#include "diag_logic.h"

static int s_fail, s_pass;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (cond) s_pass++;                                                 \
        else { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_STR(a, b)                                                     \
    do {                                                                    \
        if (strcmp((a), (b)) == 0) s_pass++;                                \
        else { s_fail++; printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } \
    } while (0)

static wb_diag_inputs_t base(void)
{
    wb_diag_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.last_bdb_status = WB_BDB_STATUS_UNKNOWN;
    return in;
}

static void test_parse_u16(void)
{
    uint16_t v = 0;
    CHECK(wb_parse_u16("0x0000", &v) && v == 0);
    CHECK(wb_parse_u16("0x1A2b", &v) && v == 0x1a2b);
    CHECK(wb_parse_u16("1a2b", &v) && v == 0x1a2b);
    CHECK(wb_parse_u16("65535", &v) && v == 65535);
    CHECK(!wb_parse_u16("65536", &v));
    CHECK(!wb_parse_u16("0x", &v));
    CHECK(!wb_parse_u16("", &v));
    CHECK(!wb_parse_u16("0x12345", &v));
    CHECK(!wb_parse_u16("12zz", &v));
}

static void test_ieee(void)
{
    uint8_t le[8];
    char buf[24];
    CHECK(wb_parse_ieee("98:a3:16:ff:fe:bb:7a:38", le));
    CHECK(le[0] == 0x38 && le[7] == 0x98);
    wb_format_ieee(le, buf, sizeof(buf));
    CHECK_STR(buf, "98:a3:16:ff:fe:bb:7a:38");
    CHECK(wb_parse_ieee("0x98A316FFFEBB7A38", le) && le[0] == 0x38);
    CHECK(wb_parse_ieee("98-a3-16-ff-fe-bb-7a-38", le));
    CHECK(!wb_parse_ieee("98:a3:16", le));
    CHECK(!wb_parse_ieee("98:a3:16:ff:fe:bb:7a:38:00", le));
    CHECK(!wb_parse_ieee("zz:a3:16:ff:fe:bb:7a:38", le));
    uint8_t zero[8] = {0}, ff[8], mixed[8] = {1};
    memset(ff, 0xff, sizeof(ff));
    CHECK(wb_ieee_is_unset(zero));
    CHECK(wb_ieee_is_unset(ff));
    CHECK(!wb_ieee_is_unset(mixed));
}

static void test_channel_mask(void)
{
    char buf[64];
    wb_format_channel_mask(0x07FFF800u, buf, sizeof(buf));
    CHECK_STR(buf, "11-26");
    wb_format_channel_mask(1u << 15, buf, sizeof(buf));
    CHECK_STR(buf, "15");
    wb_format_channel_mask((1u << 15) | (1u << 20) | (1u << 25), buf, sizeof(buf));
    CHECK_STR(buf, "15,20,25");
    wb_format_channel_mask((1u << 11) | (1u << 12), buf, sizeof(buf));
    CHECK_STR(buf, "11,12");
    wb_format_channel_mask(0, buf, sizeof(buf));
    CHECK_STR(buf, "none");
}

static void test_strings(void)
{
    CHECK_STR(wb_bdb_status_str(WB_BDB_STATUS_TCLK_EX_FAILURE), "TCLK_EX_FAILURE");
    CHECK_STR(wb_bdb_status_str(WB_BDB_STATUS_NO_NETWORK), "NO_NETWORK");
    CHECK_STR(wb_nwk_cmd_status_str(0x11), "BAD_FRAME_COUNTER");
    CHECK_STR(wb_relationship_str(WB_REL_CHILD), "child");
    CHECK_STR(wb_device_type_str(WB_DEV_ROUTER), "ZR");
    CHECK_STR(wb_zdp_status_str(0x85), "TIMEOUT");
    for (int v = 0; v < WB_VERDICT_COUNT; ++v) {
        CHECK(strlen(wb_verdict_name((wb_verdict_t)v)) > 0);
        CHECK(strlen(wb_verdict_advice((wb_verdict_t)v)) > 0);
    }
}

static void test_classify(void)
{
    wb_diag_inputs_t in = base();
    CHECK(wb_classify(&in) == WB_VERDICT_NOT_COMMISSIONED);

    in = base(); in.stack_error = true; in.joined = true;
    CHECK(wb_classify(&in) == WB_VERDICT_STACK_ERROR);

    in = base(); in.join_attempted = true; in.steering_active = true;
    CHECK(wb_classify(&in) == WB_VERDICT_JOIN_IN_PROGRESS);

    in = base(); in.join_attempted = true; in.last_bdb_status = WB_BDB_STATUS_NO_NETWORK;
    CHECK(wb_classify(&in) == WB_VERDICT_STEERING_FAILED_UNSCANNED);

    in.scan_done = true;
    CHECK(wb_classify(&in) == WB_VERDICT_NO_NETWORK_VISIBLE);
    in.scan_networks = 3;
    CHECK(wb_classify(&in) == WB_VERDICT_PERMIT_JOIN_CLOSED);
    in.scan_permit_join = 1;
    CHECK(wb_classify(&in) == WB_VERDICT_NO_ROUTER_CAPACITY);
    in.scan_router_capacity = 1;
    CHECK(wb_classify(&in) == WB_VERDICT_JOIN_REFUSED_OR_AUTH_FAILED);

    in.last_bdb_status = WB_BDB_STATUS_TCLK_EX_FAILURE; /* TCLK wins over scan evidence */
    CHECK(wb_classify(&in) == WB_VERDICT_SECURITY_TCLK_FAILURE);

    in = base(); in.join_attempted = true; in.removed_after_join = true;
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_THEN_REMOVED);

    in = base(); in.joined = true;
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_NO_ROUTING_EVIDENCE);
    in.children = 1;
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_LINKS_PRESENT);
    in.children = 0; in.router_neighbors = 2;
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_LINKS_PRESENT);
    in.route_to_coordinator_relayed = true; /* relay route alone is not proof */
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_LINKS_PRESENT);
    in.coordinator_ping_ok = true;
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_MULTIHOP_VERIFIED);
    in.route_to_coordinator_relayed = false;
    CHECK(wb_classify(&in) == WB_VERDICT_JOINED_COORDINATOR_REACHABLE);
}

int main(void)
{
    test_parse_u16();
    test_ieee();
    test_channel_mask();
    test_strings();
    test_classify();
    printf("diag_logic: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
