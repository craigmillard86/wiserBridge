#!/usr/bin/env python3
"""Decode a LINKTYPE_IEEE802_15_4_TAP(283) pcap and summarise MAC/NWK routing among
hub(0x0000), plug(0x7798), C6(0x2c3c). Headers are unencrypted; payload/command-id is not."""
import struct
import sys
from collections import Counter

HUB, PLUG, C6 = 0x0000, 0x7798, 0x2c3c
NAMES = {HUB: "hub", PLUG: "plug", C6: "C6", 0xFFFF: "bcast", 0xFFFC: "bcast-rtr", 0xFFFD: "bcast-rxon"}
OURS = {HUB, PLUG, C6}


def nm(a):
    return NAMES.get(a, f"0x{a:04x}") if a is not None else "-"


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def parse_mac(f):
    """Return (mac_src, mac_dst, nwk_off) or None. Short-address data frames only."""
    if len(f) < 3:
        return None
    fcf = u16(f, 0)
    ftype = fcf & 0x7
    dst_mode = (fcf >> 10) & 0x3
    src_mode = (fcf >> 14) & 0x3
    pan_comp = (fcf >> 6) & 0x1
    o = 3  # fcf(2)+seq(1)
    dst = src = None
    if dst_mode == 2:
        dst = u16(f, o + 2); o += 4
    elif dst_mode == 3:
        o += 10
    elif dst_mode == 0:
        pass
    if src_mode != 0:
        if not (pan_comp and dst_mode != 0):
            o += 2  # src PAN
        if src_mode == 2:
            src = u16(f, o); o += 2
        elif src_mode == 3:
            o += 8
    return ftype, src, dst, o


def parse_nwk(f, o):
    if o + 8 > len(f):
        return None
    nfcf = u16(f, o)
    ntype = nfcf & 0x3           # 0=data 1=command
    ndst = u16(f, o + 2)
    nsrc = u16(f, o + 4)
    srcroute = (nfcf >> 10) & 1
    return ntype, nsrc, ndst, srcroute


def main(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\xd4\xc3\xb2\xa1", "not a little-endian pcap"
    o = 24
    flows = Counter()
    nwk_cmd_bcast_from_hub = 0
    c6_uplink_via = Counter()
    hub_downlink_to_c6_via = Counter()
    total = 0
    while o + 16 <= len(data):
        caplen = u16(data, o + 8) | (data[o + 10] << 16) | (data[o + 11] << 24)
        o += 16
        rec = data[o:o + caplen]
        o += caplen
        if len(rec) < 4:
            continue
        taplen = u16(rec, 2)
        frame = rec[taplen:]
        m = parse_mac(frame)
        if not m:
            continue
        ftype, msrc, mdst, nwk_off = m
        if ftype != 1:  # data frames carry NWK; skip MAC cmd/ack/beacon
            continue
        n = parse_nwk(frame, nwk_off)
        if not n:
            continue
        total += 1
        ntype, nsrc, ndst, srcroute = n
        # only frames touching our three nodes at MAC or NWK level
        if not ({msrc, mdst, nsrc, ndst} & OURS):
            continue
        tag = "CMD" if ntype == 1 else "data"
        flows[(nm(msrc), nm(mdst), nm(nsrc), nm(ndst), tag, "SR" if srcroute else "")] += 1
        if nsrc == HUB and ntype == 1 and ndst in (0xFFFF, 0xFFFC, 0xFFFD):
            nwk_cmd_bcast_from_hub += 1
        if nsrc == C6 and ndst == HUB:
            c6_uplink_via[nm(mdst)] += 1
        if nsrc == HUB and ndst == C6:
            hub_downlink_to_c6_via[nm(msrc)] += 1

    print(f"parsed {total} NWK data frames\n")
    print("=== flows touching hub/plug/C6  [MACsrc->MACdst | NWKsrc->NWKdst  type] = count ===")
    for (ms, md, ns, nd, tag, sr), c in flows.most_common(40):
        print(f"  {ms:>10} -> {md:<10} | {ns:>6} -> {nd:<10} {tag:4}{sr:3} = {c}")
    print(f"\nNWK command broadcasts from hub (MTORR/link-status etc.): {nwk_cmd_bcast_from_hub}")
    print(f"C6 uplink to hub (NWK 0x2c3c->0x0000) next-hop (MAC dst): {dict(c6_uplink_via)}")
    print(f"Hub downlink to C6 (NWK 0x0000->0x2c3c) prev-hop (MAC src): {dict(hub_downlink_to_c6_via)}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "captures/route.pcap")
