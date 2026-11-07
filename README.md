# WiserBridge — ESP32-C6 Zigbee router for extending a Drayton Wiser network

An inexpensive, open-source **Zigbee 3.0 router (ZR)** firmware for the **Seeed Studio XIAO ESP32-C6**,
intended to extend a Drayton Wiser heating system's Zigbee mesh to distant radiator thermostats:

```
Wiser Heat Hub ──► WA104 repeater ──► ESP32-C6 (this firmware) ──► distant Wiser TRVs
   (coordinator / trust centre)        (ordinary Zigbee router)       (sleepy end devices)
```

> **Status (2026-11-07): working on a real Wiser network.** Two ESP32-C6 boards running this firmware
> are joined to the Wiser Heat Hub as routers and show in the Wiser app as smart plugs. They relay hub
> traffic over multiple hops and act as parent to real Wiser TRVs. The first has run on the live heating
> network for **over 24 h with all TRVs reporting** (owner-reported). A 10-minute sniffer capture on
> 2026-11-07 measured both routers relaying cleanly, with no route-request flooding. Read the earlier
> [live-network incident](#tested-evidence) before deploying, and give each C6 a solid link to its
> parent router.

This is **not a Drayton product** and does not pretend to be one. It joins your existing network as a
standard router using standard Zigbee commissioning and security; it never forms its own network and
never acts as a coordinator.

## What it does

* Joins an existing Zigbee network as a **router** (esp-zigbee-sdk v1.6.8 / ZBOSS, ESP-IDF v5.5.6).
  Routing, route discovery/repair, link status, parent/child handling and sleepy end-device buffering
  are all done by the Zigbee stack — no custom mesh logic, no hard-coded next hops.
* Accepts up to **20 child devices** (configurable 1–32; 32 is the stack's default ceiling).
  Configuring 20 slots does **not** guarantee 20 TRVs will use it — TRVs pick their own parent.
* Keeps its network membership across power cycles (`zb_storage` partition) and rejoins
  automatically; leaves cleanly on a deliberate, confirmed reset.
* **Never joins by accident:** joining only happens on an explicit `join` command or BOOT short press,
  can be restricted to one network (`epid`) and channel (`channels`), and is refused while already
  joined (on a joined router, "steering" would *open your Wiser network*).
* Rich **USB serial diagnostics**: neighbour table (LQI/RSSI/cost/age), routing table, children,
  remote neighbour tables (`lqi`), hub reachability (`ping`), a multi-hop evidence report
  (`multihop`), a join-failure classifier, an event log and a machine-readable `HEALTH` line for
  soak tests. Network keys are never read or printed in a default build (an opt-in, Kconfig-gated
  `netkey` diagnostic exists only for decrypting your own captures).
* A device **`interview`** command (ZDO descriptors + Basic cluster) that can fingerprint the genuine
  WA104 *over the network* without modifying it, feeding a configurable compatibility profile.
* A **separate passive sniffer firmware** + pcap converter for Wireshark.

Wi-Fi is never started. Bluetooth is disabled.

## Tested evidence

Every result below comes from one real Drayton Wiser installation: a Wiser Heat Hub on channel 16, a
Wiser smart plug and six TRVs. The hardware was a Seeed XIAO ESP32-C6 running this firmware, with a
second C6 running `sniffer/` as a passive capture radio. The full logs are in
[docs/wiser-public-sources.md](docs/wiser-public-sources.md) and the milestones are in
[docs/multihop-testing.md](docs/multihop-testing.md).

| | Milestone | Result | Evidence |
|---|---|---|---|
| M1 | Builds and runs | ✅ **Passed** 2026-11-05 | IDF v5.5.6, 0 errors and 0 warnings for both router and sniffer; host tests pass (75/75 C, 11/11 Python); bench checks on a real XIAO ([verification report](docs/verification-report.md)) |
| M2 | Joins the Wiser hub | ✅ **Passed**, with a recognised identity | A generic router identity was joined and then **removed by the hub after about 10 s** (3 out of 3 attempts). With the Wiser smart plug's standard identity (HEIMAN `SmartPlug-EF-3.0`, mfr 0x120B, device 0x0051) it was **accepted and kept** |
| M3 | Survives reboots | ✅ **Observed** | `JOINED (restored from NVRAM)` after an app reflash and reboots, with no re-commissioning |
| M4 | Multi-hop hub ↔ plug ↔ C6 | ✅ **Passed** 2026-11-06 | `ping 0x0000` 10/10 via the plug, verdict `JOINED_MULTIHOP_VERIFIED`. The sniffer shows 0 direct frames in either direction (uplink C6→plug→hub, downlink source-routed hub→plug→C6). The C6 stayed routed through the plug for a sustained period (including a clean 25-min ping soak, 17/17) until the stack switched to a direct route to the hub once that became the better path. That switch is normal Zigbee route selection. At the far edge-of-range spot, the direct route it chose was too weak to hold (see below) |
| M5 | TRVs use the C6 as parent | ✅ **Demonstrated** 2026-11-05 | After a plug power-cycle, 5 sleepy TRVs stayed children of the C6. Traffic went TRV → C6 → plug → hub, with `ping 0x0000` 7/8 at about 50 ms and verdict `JOINED_MULTIHOP_VERIFIED` |
| M6 | Long-term reliability | ✅ **24 h passed** (owner-reported, then checked with a sniffer), 7-day open | The XIAO rejoined on 2026-11-06 about 18:18 next to the plug, with the hub reachable (`ping 0x0000` 3/3 and 5/5, about 30–45 ms) and 2 sleepy TRVs as children by 18:50. Since then it has run more than 24 h on a phone-charger supply, with all TRVs online in the Wiser app. A 10-minute capture on 2026-11-07 confirmed it was still on the same short address 0x2c3c, which means no rejoin, and was healthy (see [Live capture](#live-capture-2026-11-07)) |
| — | Works as a smart plug in the app | ✅ | Wiser app On/Off toggles drive a GPIO, and the hub reads back metering values |

### What the tests revealed

* **The hub decides who stays by device identity, not by Schneider attributes.** It reads the
  manufacturer-specific Basic attributes 0xE007 and 0xE009 (mfr code 0x105E) on endpoints 1, 3, 5, 21, 6,
  20 and 7. The real plug answers *unsupported* to these and is still kept. A router with an unknown
  identity receives `Mgmt_Leave` about 10 s after joining.
* **The Wiser hub is a concentrator.** A router can only reach it through a many-to-one route, which is
  learned from the hub's periodic MTORR broadcasts. At the far, edge-of-range spot, the uplink worked
  through the plug and then, once the route moved off the plug, it sometimes aged out (`NO_ROUTE_AVAILABLE`, ping 0/N) while the downlink kept working. The external antenna only
  helped for a while. esp-zigbee has **no API to install a static route**, so the firmware cannot pin
  `hub via plug`.
* **A routing loop formed between the plug and the C6.** Decrypted captures show the C6 correctly
  relaying the MTORR at path cost 12–13, while the plug rates its own hub link at the worst cost (7). In
  that state the plug sometimes chose the C6 as its next hop to the hub, and frames bounced between the
  two until their radius ran out.
* **Live-network incident (2026-11-06, before the successful run).** During C6 testing the plug's TRV branch lost its route to
  the hub, and several TRVs went offline in the app. The C6 was silenced and removed from the app; the
  hub and the plug were restarted. Sniffing next to the hub then showed a **one-way link at the plug**:
  the hub heard the plug and answered it, but the plug barely heard the hub. That link had already been
  rated cost 7 before any of this testing. All TRVs came back the same evening. **It has not been
  established whether the C6 caused the incident or only made it worse.** The C6 was then rejoined that
  evening at a better spot, next to the plug, and has run for 24 h+ since (M6 above).
* Fixed along the way: a ZBOSS assertion that rebooted the C6 every ~60 s. The cause was calling
  `esp_zb_zcl_report_attr_cmd_req()` on attributes the hub had never configured for reporting.

### Live capture, 2026-11-07

This is a 10-minute passive capture on channel 16, from a third C6 running `sniffer/` placed next to the
XIAO. It was decoded with `tools/sniff_device_summary.py`, which reads only the clear-text MAC and NWK
headers. Each router was identified by the IEEE address in its NWK frames.

| Node | Device | Next hop to hub | Frames sent | Frames relayed for others | MAC retransmits | Route requests |
|---|---|---|---|---|---|---|
| 0x2c3c | **C6 router #1** (XIAO) | hub, direct (191/191) | 231 | **654** | 3 % | 0 |
| 0x578f | **C6 router #2** (the previous sniffer board, converted to a router on 2026-11-06) | hub, direct (189) | 229 | **522** | <1 % | 0 |
| 0x7798 | Wiser smart plug (Heiman) | hub, direct | 121 | 172 | 0 % | 0 |

* **Four TRVs use C6 #1 as their parent:** 0x917f, 0x38e4, 0x6922 and 0x7183. Each sent about 105
  frames in the 10 minutes, and C6 #1 forwarded them to the hub. The hub's replies were source-routed
  back down to each TRV via **hub → C6 #2 → C6 #1 → TRV**.
* **The hub's traffic to C6 #1 goes via C6 #2.** That is a multi-hop path made only of WiserBridge
  routers.
* **Together the two C6s relayed about 1,180 frames in 10 minutes,** about 7× as many as the real plug.
* **Only normal housekeeping from every router.** Each router sent only link-status broadcasts, about
  one every 15 s, and **no route requests**. During the 2026-11-06 incident, C6 #1 had sent about 600 in
  20 minutes.
* The sniffer sat next to C6 #1 and heard the hub only faintly (−96 dBm). The hub's own frame counts are
  therefore undercounts.

### Not yet verified

A 7-day soak, and a 24 h run measured by `HEALTH` logs rather than reported from the app. Also
unverified: whether the hub eventually drops the impersonated plug; whether multi-hop holds at the far,
edge-of-range spot (the 24 h run was next to the plug); and whether the C6 can share a branch with a
marginal router without forming a loop.

## Repository layout

| Path | Contents |
|---|---|
| `main/` | Router firmware (ESP-IDF component) |
| `main/zigbee_router.c` | Stack init, endpoint, commissioning state machine, leave/reset |
| `main/diagnostics.c` | Tables, probes (`scan`/`ping`/`lqi`/`interview`), event log, HEALTH |
| `main/diag_logic.c` | Pure-C classification/parsing (host unit-tested) |
| `main/wiser_profile.c` | Identity/compatibility profile (Kconfig defaults + NVS overrides) |
| `main/serial_cli.c` | Serial command interface |
| `main/board.c` | XIAO RF switch, status LED, BOOT button |
| `sniffer/` | Separate passive IEEE 802.15.4 sniffer firmware |
| `tools/` | Build script, serial monitor/logger, health report, pcap converter and decoders (`sniff_route_decode.py`, `sniff_device_summary.py`), fingerprint tools |
| `tests/` | Host C unit tests + Python tool tests (`tests/run_all.sh`) |
| `fingerprints/` | WA104 fingerprint **template** (all unknown) + JSON schema |
| `release/` | Merged flash images (generated by `tools/build.sh`) |
| `docs/` | Everything below |

## Quick start (Windows)

1. Build (Docker, from WSL or Git Bash): `tools/build.sh` — or see [docs/building.md](docs/building.md).
2. Find the board's COM port and flash — [docs/flashing.md](docs/flashing.md):
   ```
   py -m esptool --chip esp32c6 -p COM8 erase-flash
   py -m esptool --chip esp32c6 -p COM8 write-flash 0x0 release\wiserbridge-router-0.1.0-xiao-esp32c6.bin
   ```
3. Open the console: `py tools\wb_monitor.py --port COM8 --duration 0` (or `idf.py -p COM8 monitor`),
   type `help`.
4. Follow [docs/commissioning.md](docs/commissioning.md) to attempt joining the Wiser hub, then
   [docs/multihop-testing.md](docs/multihop-testing.md).

## Documentation

* [Hardware](docs/hardware.md) · [Building](docs/building.md) · [Flashing](docs/flashing.md)
* [Commissioning with the Wiser hub](docs/commissioning.md) · [Serial command reference](docs/cli-reference.md)
* [Multi-hop verification & reliability tests (M1–M6)](docs/multihop-testing.md)
* [Troubleshooting](docs/troubleshooting.md)
* [WA104 identity investigation & sniffing](docs/protocol-investigation.md)
* [Wi-Fi extension feasibility](docs/wifi-bridge-feasibility.md)
* [Verification report](docs/verification-report.md) — what has and has not been verified

## Limitations (read before relying on it for heating)

* The hub only keeps routers whose identity it recognises. A generic identity is removed after about
  10 s. Presenting another product's identity (the Heiman plug) works, but it impersonates a real
  device model, and the hub may treat it differently in future firmware.
* Uplink to the hub depends on many-to-one routes that the firmware cannot pin, so multi-hop
  can decay at the edge of range (see [Tested evidence](#tested-evidence)).
* The public SDK exposes no relayed-frame counters, MAC retry counters or per-child poll statistics.
  Where proof needs those, use a second sniffer radio.
* One C6 cannot be a router and a sniffer at the same time.
* A TRV only changes parent when it rejoins; being physically close to the C6 does not mean it uses it.
* If the hub rotates the network key while the C6 is powered off, the C6 may need a reset and rejoin.
* Run reliability tests at times when temporary heating disruption is acceptable. Never unpair
  existing TRVs or factory-reset the WA104 for these tests.

License: Apache-2.0 (add the full LICENSE text before publishing).
