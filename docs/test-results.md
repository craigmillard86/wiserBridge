# Test results

All results come from one Drayton Wiser installation: a Wiser Heat Hub on channel 16, a Wiser smart
plug (a Heiman SmartPlug-EF-3.0) and six TRVs. The routers were a Seeed XIAO ESP32-C6 and an ESP32-C6
SuperMini running this firmware. A third C6 running `sniffer/` captured traffic.

The dated lab notes are in [wiser-public-sources.md](wiser-public-sources.md). Test procedures are in
[multihop-testing.md](multihop-testing.md).

## Milestones

| | Milestone | Result | Evidence |
|---|---|---|---|
| M1 | Builds and runs | Passed 2026-11-05 | IDF v5.5.6, no errors or warnings for router and sniffer. Host tests 75/75 (C) and 11/11 (Python). Bench checks on a XIAO: [verification-report.md](verification-report.md) |
| M2 | Joins the Wiser hub | Passed, with the plug identity | A generic identity joined and was removed by the hub about 10 s later, 3 times out of 3. With the Wiser smart plug identity (HEIMAN SmartPlug-EF-3.0, mfr 0x120B, device 0x0051) it was kept |
| M3 | Survives reboots | Observed | `JOINED (restored from NVRAM)` after reboots and an app reflash. No re-pairing needed |
| M4 | Multi-hop | Passed 2026-11-06 | `ping 0x0000` 10/10 via the plug, verdict `JOINED_MULTIHOP_VERIFIED`. Sniffer: 0 direct C6-hub frames in either direction. The C6 stayed routed through the plug (including a 25 min ping soak, 17/17) until the stack moved it to a direct route to the hub |
| M5 | TRVs use the C6 as parent | Passed 2026-11-05 | After the plug was power-cycled, 5 TRVs stayed on the C6. Path TRV → C6 → plug → hub, `ping 0x0000` 7/8 at about 50 ms |
| M6 | Long-term running | 24 h passed, 7 days open | The XIAO rejoined at 18:18 on 2026-11-06 and ran for more than 24 h on a USB charger with all TRVs online in the Wiser app (reported by the owner). A sniffer capture on 2026-11-07 found it on the same short address, so it had not rejoined |
| | On/Off and metering | Works | Toggling the plug in the Wiser app drives a GPIO. The hub reads the metering values |

## Live capture, 2026-11-07

10 minutes on channel 16, sniffer next to the XIAO, decoded with `tools/sniff_device_summary.py`
(MAC and NWK headers only; payloads stay encrypted). Routers were identified by the IEEE address in
their NWK frames.

| Node | Device | Next hop to hub | Sent | Relayed for others | MAC retries | Route requests |
|---|---|---|---|---|---|---|
| 0x2c3c | C6 #1 (XIAO) | hub, 191/191 | 231 | 654 | 3 % | 0 |
| 0x578f | C6 #2 (SuperMini, joined 2026-11-06 evening) | hub | 229 | 522 | <1 % | 0 |
| 0x7798 | Wiser smart plug | hub | 121 | 172 | 0 % | 0 |

- TRVs 0x917f, 0x38e4, 0x6922 and 0x7183 use C6 #1 as their parent. Each sent about 105 frames.
- The hub reaches C6 #1 and its TRVs through C6 #2: hub → C6 #2 → C6 #1 → TRV.
- No TRV used C6 #2 as its parent during the capture.
- Each router sent one link-status broadcast about every 15 s and no route requests.
- The hub was at −96 dBm at the sniffer, so the hub's own counts are low.

## What we learned

**The hub decides by device identity.** After a join, the hub reads Schneider attributes 0xE007 and
0xE009 (mfr code 0x105E) from the Basic cluster on endpoints 1, 3, 5, 21, 6, 20 and 7. The real plug
answers "unsupported" and is kept anyway. A device with an identity the hub doesn't know gets
`Mgmt_Leave` about 10 s after joining. That is why the plug identity is now the default.

**Routes to the hub can fade at the edge of range.** The hub is a concentrator: routers reach it
through a many-to-one route learned from its MTORR broadcasts. At a spot about 15 m and several walls
from the hub, the C6's uplink sometimes aged out (`NO_ROUTE_AVAILABLE`, ping 0/N) while the downlink
still worked. An external antenna helped for about 25 minutes. esp-zigbee has no API to install a
static route, so the firmware can't force "hub via plug". The experimental `pinroute` command didn't
help.

**A routing loop formed behind a weak router.** Decrypted captures showed the C6 relaying the hub's
MTORR at path cost 12-13 while the plug rated its own hub link at cost 7, the worst. The plug sometimes
picked the C6 as its next hop to the hub, and frames bounced between them until their radius ran out.

**Incident on 2026-11-06.** During testing, the plug's branch lost its route to the hub and several
TRVs went offline in the app. The C6 was switched off and removed from the app, and the hub and plug
were restarted. A capture next to the hub then showed the plug's link was one-way: the hub heard the
plug and replied, but the plug barely heard the hub. That link was already at cost 7 before testing.
All TRVs were back the same evening. It is not known whether the C6 caused this or made it worse. The
C6 was rejoined that evening next to the plug and is the board in M6.

**Crash fixed.** Calling `esp_zb_zcl_report_attr_cmd_req()` on an attribute the hub hadn't set up for
reporting hit a ZBOSS assertion and rebooted the C6 every 60 s. The firmware now updates attribute
values and lets the stack report them.

## Not yet tested

- A 7-day run, and a 24 h run measured with `HEALTH` logs instead of the app.
- Whether the hub drops the plug identity over time.
- Multi-hop at the far, edge-of-range spot (the 24 h run was next to the plug).
- A C6 behind a weak router without the routing loop.
