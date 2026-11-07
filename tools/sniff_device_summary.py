#!/usr/bin/env python3
"""Per-device summary of a LINKTYPE_IEEE802_15_4_TAP (283) pcap from sniffer/ + sniff_to_pcap.py.

For every NWK short address seen: role guess, parent / next hop to the hub, frames originated,
frames relayed for others, MAC retransmissions, NWK command broadcasts split into one-hop
(link status, radius 1) and multi-hop (route requests etc.), and RSSI at the sniffer.
Only clear-text MAC/NWK headers are used; payloads stay encrypted.

    python3 tools/sniff_device_summary.py captures/x.pcap
"""
import struct
import sys
from collections import Counter, defaultdict

KNOWN = {0x0000: "hub", 0x7798: "plug", 0x2c3c: "C6 router"}
BCAST = (0xFFFF, 0xFFFC, 0xFFFD)


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def tap_rssi(rec, taplen):
    o = 4
    while o + 4 <= taplen:
        t, ln = u16(rec, o), u16(rec, o + 2)
        if t == 1 and ln == 4:
            return struct.unpack_from("<f", rec, o + 4)[0]
        o += 4 + ((ln + 3) & ~3)
    return None


def parse(frame):
    if len(frame) < 3:
        return None
    fcf = u16(frame, 0)
    ftype, dmode, smode, panc = fcf & 7, (fcf >> 10) & 3, (fcf >> 14) & 3, (fcf >> 6) & 1
    seq, o, dst, src = frame[2], 3, None, None
    if dmode == 2:
        dst = u16(frame, o + 2); o += 4
    elif dmode == 3:
        o += 10
    if smode:
        if not (panc and dmode):
            o += 2
        if smode == 2:
            src = u16(frame, o); o += 2
        elif smode == 3:
            o += 8
    return ftype, seq, src, dst, o


def main(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\xd4\xc3\xb2\xa1", "not a little-endian pcap"
    o, first_ts, last_ts = 24, None, None
    orig = Counter(); relayed = Counter(); mac_tx = Counter(); retries = Counter()
    ls_cmds = Counter(); mh_cmds = Counter(); rssi = defaultdict(list)
    uplink_next = defaultdict(Counter)      # own frames to hub: MAC dst
    ed_parent = defaultdict(Counter)        # MAC dst of own frames (any NWK dst)
    seen_seq = {}
    while o + 16 <= len(data):
        ts = struct.unpack_from("<I", data, o)[0]
        caplen = struct.unpack_from("<I", data, o + 8)[0]
        rec = data[o + 16:o + 16 + caplen]; o += 16 + caplen
        first_ts = first_ts or ts; last_ts = ts
        if len(rec) < 4:
            continue
        taplen = u16(rec, 2)
        m = parse(rec[taplen:])
        if not m:
            continue
        ftype, seq, msrc, mdst, no = m
        frame = rec[taplen:]
        if msrc is not None:
            r = tap_rssi(rec, taplen)
            if r is not None:
                rssi[msrc].append(r)
            key = (msrc, mdst)
            if ftype == 1 and seen_seq.get(key) == seq:
                retries[msrc] += 1
            seen_seq[key] = seq
            mac_tx[msrc] += 1
        if ftype != 1 or no + 8 > len(frame):
            continue
        nfcf = u16(frame, no)
        ntype, ndst, nsrc, radius = nfcf & 3, u16(frame, no + 2), u16(frame, no + 4), frame[no + 6]
        if msrc is None:
            continue
        if msrc == nsrc:
            orig[nsrc] += 1
            if mdst not in BCAST:
                ed_parent[nsrc][mdst] += 1
            if ndst == 0x0000 and mdst is not None:
                uplink_next[nsrc][mdst] += 1
            if ntype == 1 and ndst in BCAST:
                (ls_cmds if radius == 1 else mh_cmds)[nsrc] += 1
        else:
            relayed[msrc] += 1

    mins = max((last_ts - first_ts) / 60, 1 / 60) if first_ts else 1
    devs = set(orig) | set(relayed) | set(mac_tx)
    devs = {d for d in devs if d not in BCAST}
    name = lambda a: KNOWN.get(a, f"0x{a:04x}")
    rows = []
    for d in devs:
        router = relayed[d] > 0 or ls_cmds[d] > 0 or d == 0
        par = ed_parent[d].most_common(1)
        hop = uplink_next[d].most_common(2)
        rows.append((
            0 if d in KNOWN else (1 if router else 2), d,
            name(d), "coordinator" if d == 0 else ("router" if router else "end device"),
            ", ".join(f"{name(a)}×{c}" for a, c in hop) if hop else (name(par[0][0]) if par else "-"),
            orig[d], relayed[d], retries[d], f"{100 * retries[d] / mac_tx[d]:.0f}%" if mac_tx[d] else "-",
            ls_cmds[d], mh_cmds[d],
            f"{sum(rssi[d]) / len(rssi[d]):.0f}" if rssi[d] else "-",
        ))
    rows.sort()
    print(f"{path}: {mins:.1f} min\n")
    hdr = ("device", "role", "next hop to hub", "orig", "relayed", "retx", "retx%", "linkstat", "multihop-cmd", "rssi")
    w = [11, 11, 26, 6, 8, 5, 6, 9, 13, 5]
    print("  ".join(h.ljust(x) for h, x in zip(hdr, w)))
    for r in rows:
        print("  ".join(str(v).ljust(x) for v, x in zip(r[2:], w)))
    print("\norig = frames the device originated; relayed = frames it forwarded for others;"
          "\nretx = repeated MAC sequence numbers to the same destination (retransmissions);"
          "\nlinkstat = radius-1 NWK command broadcasts (link status, ~1 per 15 s for a router);"
          "\nmultihop-cmd = own NWK command broadcasts with radius > 1 (route requests / MTORR);"
          "\nrssi = average dBm at the sniffer (depends on where the sniffer is, not link quality).")


if __name__ == "__main__":
    main(sys.argv[1])
