# Wiser identity: public-source findings (2026-11-05)

Context: M2 showed the Wiser hub (PAN 0x382c, ch 16) admits the generic C6 router and sends it a Leave
~8–10 s later (outcome B′, reproduced twice, the second time as a direct neighbour of the hub with
LQI 117 / −57 dBm). This file records what public sources say. Nothing here is a WA104 value.

## Zigbee2MQTT (`zigbee-herdsman-converters/src/devices/schneider_electric.ts`, master, 2026-11-05)

Quoted from the definitions (Wiser heating devices that Z2M knows):

| Product | `zigbeeModel` (Basic 0x0005) | Z2M model | Notes |
|---|---|---|---|
| Drayton Wiser radiator thermostat | `iTRV` | WV704R0A0902 | uses custom cluster `wiserDeviceInfo` |
| Wiser radiator thermostat | `CCTFR6100` | CCTFR6100Z3 | uses `wiserDeviceInfo` |
| Wiser smart plug | `SMARTPLUG/1` | CCT711119 | on/off + metering only |
| Heating thermostat | fingerprint modelID `CCTFR6700`/`CCTFR6710`, manufacturerName `Schneider Electric` | CCTFR6700 | |

* Custom cluster `wiserDeviceInfo` = **0xFE03**, attribute `deviceInfo` 0x0020, CHAR_STR; reports
  comma-separated strings such as `ALG,…`, `ADC,…`, `UI,BoostUp`, `MOT,…`.
* `SCHNEIDER_ELECTRIC = 0x105e` (zigbee-herdsman `manufacturerCode.ts`).
* **No entry for the WA104 / Wiser Range Extender**, and no string "Drayton" in the file.

## Other sources

* Drayton sells two routing products: the WA104 range extender and the Wiser smart plug
  (WB704H1A0902), which is described as also extending range. The range extender instructions PDF
  (wiser.draytoncontrols.co.uk) gives pairing steps only, no Zigbee identifiers.
* Community reports (Home Assistant forum, diynot) say non-Wiser repeaters cannot extend a Wiser
  network; none documents a test or the mechanism. Consistent with what M2 observed.

## What is still unknown

The WA104's manufacturer name, model identifier, node-descriptor manufacturer code, endpoints and
clusters, and which of these the hub checks before deciding to keep or remove a device.

## M2 result with APS inbound logging (2026-11-05 17:37, firmware with `aps_inspect.c`)

Observed from the C6 itself (log `logs/m2.log`; air capture `captures/join-20261105-1732.pcap`, 6245
frames, channel 16, not yet decrypted — no Wireshark on the laptop):

| t after join | From hub (0x0000) | Detail |
|---|---|---|
| −0.3 s | ZDO Node_Desc_rsp | hub node descriptor: manufacturer code **0x105E**, server mask 0x2C41 → stack revision **22** (Zigbee 3.0 TC) |
| −0.3 s | ZDO Mgmt_Permit_Joining_req | permit 15 s, then 180 s |
| −0.2 s | ZDO IEEE_addr_req | for our short address |
| +0.7 s | ZCL Read Attributes, **manufacturer-specific code 0x105E**, cluster **Basic 0x0000**, attributes **0xE007, 0xE009** | sent to endpoints **1, 3, 5, 21, 6, 20, 7** in that order, ~20 ms apart |
| +0.8 s | APS acks for our replies | |
| +9.7 s | **ZDO Mgmt_Leave_req** | device removed |

The hub never read the standard ManufacturerName (0x0004) or ModelIdentifier (0x0005).

Meaning of the attributes, from the ZHA quirks `zhaquirks/schneiderelectric/__init__.py` (class `SEBasic`):

* **0xE007 `se_product_identifier`**, enum16 — "The ProductIdentifier attribute specifies the unique
  internal numerical identifier of the product. See device description for this value."
* **0xE009 `se_product_model`**, CharacterString — "The ProductModel attribute specifies the name of the
  product model. Same value as model identifier attribute 0x0005."
* Neighbours in the same cluster: 0xE001 application FW version, 0xE002 HW version, 0xE004 serial
  number (15 hex chars), 0xE008 ProductRange, 0xE00A ProductFamily, 0xE00B VendorURL.
* ZHA quirks place `SEBasic` on endpoint 21 (switches, dimmers), 3 (dimmers), 6 (outlet) — matching the
  endpoint sweep the hub performed.

**Conclusion:** the Wiser hub's admission test is "answer Schneider's proprietary Basic attributes
ProductIdentifier/ProductModel on one of the Wiser endpoints". A generic Zigbee 3.0 router cannot pass
it. The numeric ProductIdentifier values are not published anywhere found so far; the smart plug's own
value could only be learned by reading 0xE007/0xE009 from the plug over the air.

## The router the hub *does* accept (interviewed 2026-11-05 17:53, inside the 10 s window)

The hub's neighbour table (Mgmt_Lqi) listed exactly one router besides the C6: **0x7798**, the owner's
"Wiser smart plug". Its interview (`fingerprints/wiser-plug.captured.json`, IEEE deliberately not
reused) shows it is **not a Schneider device at all**:

| Item | Value |
|---|---|
| Node descriptor | ZR, **manufacturer code 0x120B (HEIMAN)**, MAC cap 0x8E, server mask 0x2C00 (stack rev 22) |
| Endpoint 1 | profile 0x0104, **device 0x0051 (Smart Plug)**, in: 0x0000 0x0003 0x0004 0x0005 0x0006 0x0009 0x0702 0x0B04 0x0B05; out: 0x0003 0x0019 |
| Endpoint 242 | Green Power proxy (profile 0xA1E0, device 0x0061, out 0x0021) |
| Basic | ZCLVersion 3, AppVersion 32, StackVersion 2, HWVersion 22, **Manufacturer "HEIMAN"**, **Model "SmartPlug-EF-3.0"**, DateCode "20231122", PowerSource 1 (mains) |
| Schneider attrs 0xE001/2/7/8/9/A/B (code 0x105E) | **all UNSUPPORTED_ATTRIBUTE** |

So the hub keeps a device that cannot answer 0xE007/0xE009. The Schneider probe is therefore not the
whole admission test; the hub evidently also recognises known OEM devices by their standard identity
(manufacturer code and/or manufacturer/model strings and/or device type), and only rejects what it
recognises as neither. Which of those fields it keys on is unknown; the next experiment is to present
the plug's *product* identity (never its IEEE) from the C6 and watch the APS-IN log.

Air captures of the three joins: `captures/join-20261105-1732.pcap`, `join2-…-1748.pcap` (board dropped
off USB mid-capture), `join3-…-1752.pcap` (complete, 969 kB).

## Join #4 with the plug's identity loaded (2026-11-05 18:08) — INCONCLUSIVE

The C6 was given the plug's product identity (HEIMAN / SmartPlug-EF-3.0 / code 0x120B / device
0x0051, cluster list exposed with the SDK's standard constructors; OTA 0x0019 deliberately not
exposed). On this attempt it **failed to associate** — 10 BDB steering attempts, all `NO_NETWORK`,
verdict `PERMIT_JOIN_CLOSED`. Air capture `captures/join4-heiman-20261105-1802.pcap` (31013 frames):
no Association Request/Response and no frame from the C6's IEEE during the window; the C6's own scans
reported PAN 0x382c `closed`/0-permit every time, although the hub's broadcast beacon carried
assoc-permit=1 from 18:08:33 to 18:10:35.

Because association never completed, the hub never read the Basic cluster, so **this run does not
test whether the plug identity satisfies the admission check** — the identity cannot affect MAC
association. Likely causes, not separated: the hub opens the association window only briefly (the
first three joins associated within ~2-3 s; here the first attempt missed it), and/or the hub now
treats the C6's repeatedly-rejected IEEE differently. Next: retry to get a clean association, then
observe the keep-vs-kick outcome with the plug identity.

## Join #5 — ACCEPTED and RETAINED (2026-11-05 18:18)

Same firmware/identity as #4 (HEIMAN / SmartPlug-EF-3.0 / mfr 0x120B / device 0x0051 / plug cluster
set). This time association succeeded on the first attempt and **the hub did NOT send Leave**. State
stayed `JOINED` (verified ~1 min and via the helper's run), `Removed after join: no`, `Leaves: 0`,
5 active routes, neighbours = hub 0x0000 and the real plug 0x7798.

Sequence after join (from APS-IN log + `captures/join5-heiman-*.pcap`):
1. Hub read Schneider Basic 0xE007/0xE009 on every endpoint — **the device returned UNSUPPORTED**
   (we never set those values) — and the hub kept it anyway.
2. Hub then did Mgmt_Bind/Bind_req, sent On/Off (cluster 0x0006) commands, and read metering (0x0702)
   and other clusters: it is treating the C6 as a real smart plug.
3. Some NO_ROUTE_AVAILABLE / PARENT_LINK_FAILURE during settling, then stable with routes.

**Conclusion (central question answered):** a generic ESP32-C6 router *can* join and stay on this
Drayton Wiser network, but only by presenting the **standard identity of a device model the hub
recognises** (here the Heiman OEM smart plug: manufacturer code 0x120B + model + device id). The
Schneider manufacturer-specific attributes are NOT the gate (the device failed them and was kept).
A plain generic identity is rejected ~10 s after join (#1–#3); the recognised-model identity is
retained (#5).

Still unverified: long-term retention (hours/days), whether the hub eventually marks the fake plug
unresponsive, and whether TRVs route through it (M4/M5). These are now testable because it stays joined.

## M4 multi-hop routing — VERIFIED (2026-11-05 ~18:30)

With the C6 relocated out of the hub's radio range (only neighbour = plug 0x7798, LQI 122/-56):

* The plug's own neighbour table (Mgmt_Lqi of 0x7798) lists the hub 0x0000 as its **parent** — so
  plug→hub is a real link.
* C6 route to hub: **next hop 0x7798 (plug), state ACTIVE** (relayed), hub NOT a direct neighbour.
* `ping 0x0000 10` → 10/10 replies across the relay (RTT 45 ms–3.3 s, variable).
* Firmware verdict reached **`JOINED_MULTIHOP_VERIFIED`** (requires: hub not a neighbour + relayed
  route to hub + end-to-end ping ok).

Caveat: the path is functional but not yet rock-solid at this location — the verdict flapped between
`JOINED_MULTIHOP_VERIFIED` and `JOINED_COORDINATOR_REACHABLE` as the route to 0x0000 occasionally
reverted to a stale DISCOVERING/direct entry, and RTT spiked to seconds. Multi-hop delivery is proven;
long-term stability at a given spot is a placement/soak question (M6).

## On/Off control + metering keep-alive — WORKING (2026-11-05 ~18:50)

The C6 (as the Heiman plug identity) now exposes a functional On/Off output and plausible metering:
* App toggle → hub sends ZCL On/Off `cluster-cmd 0x01`(On)/`0x00`(Off) to ep1 cl 0x0006 → firmware
  drives **GPIO0** (configurable) and logs `ONOFF ON/OFF (from the network)`; hub reads the attribute
  back and we answer. Verified end-to-end from the Wiser app.
* Output pin is runtime-configurable and NVS-persisted: CLI `relay [on|off|pin <n>|invert on|off]`
  (Kconfig default WB_RELAY_GPIO=0, active-high). Local `relay on/off` overrides for bench testing.
* Metering keep-alive: while joined, every WB_PLUG_REPORT_S (default 60 s) the firmware refreshes
  Electrical Measurement RMSVoltage (~230 V) / RMSCurrent (0 when off) and re-reports On/Off + current
  to the hub, so the fake plug looks live. (It stayed joined throughout; longer soak still TODO.)
* Persistence: after reflashing the app partition it returned `JOINED (restored from NVRAM)` with no
  re-commissioning (M3 behaviour observed).

New firmware: main/aps_inspect.*, relay in board.*, On/Off handler + plug_keepalive + relay config in
zigbee_router.c, `relay`/`apslog` CLI, Kconfig WB_RELAY_GPIO/WB_RELAY_ACTIVE_LOW/WB_PLUG_REPORT_S.
A single 802.15.4 radio = one Zigbee network only; Wi-Fi can run concurrently (coexistence, with
the caveats in wifi-bridge-feasibility.md §6).

## M5 + M4 together — TRVs parented to the C6, relayed to the hub (2026-11-05 ~19:25)

Natural re-parenting test: the owner powered the plug (0x7798) OFF, then ON.

* With the plug off, 7 of its end devices re-parented onto the C6 within ~2 min (C6 `children` 0 -> 7).
  The C6 was then isolated from the hub (plug was its only uplink), so those TRVs were temporarily
  cut off — expected, and why the plug was restored promptly.
* With the plug back on, the C6 **retained 5 sleepy TRVs as children** (0x38e4, 0x917f, 0x6922,
  0x72e8, 0x14f1; sleepy=YES) **and** regained an ACTIVE relayed route to the hub via the plug.
* Confirmed end-to-end: `ping 0x0000` 7/8 replies ~50 ms, verdict **JOINED_MULTIHOP_VERIFIED**.

Live path verified: **TRV -> C6 -> plug -> hub**. The C6 is a functioning Wiser range extender:
it both parents real Wiser TRVs and relays their traffic to the hub. (RTT fell from multi-second at
the mesh edge to ~50 ms once it carried children mid-mesh.)

Milestones now: M1 build, M2 join (recognised identity), M3 persistence (restore from NVRAM),
M4 multi-hop, M5 TRV-as-child all demonstrated. Remaining: M6 long-term soak (24 h / 7 day) and
confirming the hub never drops the fake plug over time.

## 2026-11-06 — Multi-hop proven (sniffer) and found antenna-dependent

After fixing the keep-alive panic (see git history) the C6 was reset, removed from the app, and
re-joined cleanly. In the far spot (~15 m, several walls, plug between C6 and hub) the uplink route
would not form on the on-board **ceramic** antenna: the C6 faintly heard the hub's own broadcasts and
recorded a dead **direct** route (`coord_nh=0x0000`) instead of relaying via the plug, so ping 0x0000
was 0/N with continuous `NO_ROUTE_AVAILABLE`. The hub still reached the C6 (source-routed downlink and
ZDO address/bind requests arrived), confirming a one-way (asymmetric) link, not a hub policy block.

Fitting an **external U.FL antenna** (`CONFIG_WB_ANTENNA_EXTERNAL=y`, GPIO14 high) fixed it. Captured
on the Wiser channel (16) with the second C6 running `sniffer/` and decoded with
`tools/sniff_route_decode.py` (MAC+NWK headers are in the clear; command payloads are not):

* Uplink  C6→hub: `C6 →64→ plug →91→ hub`, **0** direct C6→hub frames.
* Downlink hub→C6: `hub →(source route)→ plug →90→ C6`, **0** direct hub→C6 frames.
* Stability: 25-min hub-ping soak, 17/17 cycles 3/3 replies, ~45-60 ms, 0 drops.

Conclusion at the time (**superseded — see the next section**): multi-hop hub↔plug↔C6 works and is stable with the external antenna; the Wiser hub
source-routes to our third-party router fine. It is NOT a closed-ecosystem block and NOT a firmware
bug — the limiter was antenna/link asymmetry driving a bad route choice on the ceramic antenna.

## 2026-11-06 afternoon — uplink decays, routing loop, live-network incident

**The uplink decayed.** After the clean 25-min soak, the C6's route to the hub aged out:
`routes` showed `0x0000 0x0000 DISCOVERING MTO=no`, ping 0x0000 0/N, continuous
`NLME NO_ROUTE_AVAILABLE`, while the hub's source-routed downlink still arrived. The Wiser hub is a
**concentrator**: routers reach it via a many-to-one route learned from its MTORR broadcasts, and
the hub sends those infrequently in a quiet network. The external antenna only helped transiently.
esp-zigbee exposes **no API to install a route**, so `hub via plug` cannot be pinned; `pinroute`
(route-cost policy `use_route_for_neighbour`, optionally MAC-filtering the hub) and a boot-time pin
only choose among *existing* routes and left ping at 0/N. In a 20-min capture the C6 recovered once
after a unicast NWK command from the plug (likely a route reply), not after an MTORR.

**A plug↔C6 routing loop.** With the network key exported from our own C6 (opt-in
`CONFIG_WB_NETKEY_EXPORT` build; key kept out of logs) the captures were decrypted. The C6 relays the
hub's MTORR honestly at path cost 12–13, while the plug rates its own hub link at the worst cost (7).
About 0.5 s after an MTORR the plug sometimes adopted the C6 as its many-to-one next hop (its first
misrouted frames were MANY_TO_ONE_ROUTE_FAILURE reports), and the C6 sent the frames back — ping-pong
until the radius expired. The loop also occurred with the pin disabled, so the pin did not cause it.
The hub does answer the C6's route requests (route reply from the hub in ~250 ms).

**Live-network incident.** At 12:22 the plug's branch was healthy (no route requests). By ~16:00 the
C6 (~600 per 20 min), the plug and all six TRVs were flooding route requests for the hub, and several
TRVs showed offline in the Wiser app. Actions: C6 silenced at 16:31 (held in the ROM bootloader,
radio off); plug restarted (relayed for ~1 min, then stuck again); hub restarted ~16:54; C6 removed
from the Wiser app ~17:10; plug restarted again.

**Root cause found at the plug (~17:40, sniffer beside the hub):** a **one-way link**. The hub heard
the plug (87 % of plug→hub frames acknowledged) and answered it (145 route replies + 108 MTORRs in
2 min, ~1,700 source-routed frames for the plug's TRVs), but the plug almost never heard the hub, so
it kept flooding route requests and its TRVs were cut off. The link was already rated cost 7 in the
healthy 12:22 capture; interference near the plug (channel 16 = 2430 MHz, overlapping Wi-Fi ch 3–6)
is suspected. All TRVs were reporting again by ~17:45.

**Causation is not established.** The plug stayed stuck with the C6 silent, but the C6's afternoon of
rejoins, reboots and route-request storm, and the loop, preceded it. The C6 has been kept off the
live network since.

**Where this leaves the project:** the hub accepts the C6 (with a recognised identity), relays to it
and uses it as a TRV parent, but an edge-of-range C6 cannot hold a reliable uplink, and it must not be
placed behind a marginal router where it can form a loop. Next steps, on a test network or a healthy
plug↔hub link: place the C6 one hop from the hub; consider an auto-rejoin watchdog (risk: frequent
rejoins); re-run the loop test with a strong plug↔hub link.

## 2026-11-06 evening → 2026-11-07 — rejoined beside the plug, 24 h+ run

After the incident the C6 was rejoined (~18:18) with the plug identity, placed next to the plug
rather than at the far, edge-of-range spot. From `logs/post_join.log`, `c6_at_plugspot*.log` and
`children_check.log`:

* `HEALTH … state=JOINED … verdict=JOINED_COORDINATOR_REACHABLE`. `ping 0x0000` returned 3/3
  (27–33 ms) and then 5/5 (37–46 ms). The hub immediately read On/Off and metering, treating the C6 as
  a plug.
* By 18:50, two sleepy TRVs (0x6922, 0x38e4) had chosen the C6 as their parent.

It was then moved to a phone-charger supply (no serial access). On **2026-11-07 the owner reported
it had run for over 24 h** with the C6 and all TRVs online in the Wiser app, and no recurrence of the
afternoon's route-request storm. This is owner-reported; no HEALTH log covers the period. Opening the
USB-JTAG port would reboot the board, so the next measured check should be a single `health` +
`multihop` read at the end of a soak, or a passive sniffer capture.
