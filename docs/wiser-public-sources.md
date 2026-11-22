# Wiser investigation log

Lab log, 2026-11-05 to 2026-11-07, in date order. Later sections can supersede earlier conclusions.

## 2026-11-05: public-source findings

M2 showed that the Wiser hub (PAN 0x382c, ch 16) admits the generic C6 router and sends it a Leave
~8–10 s later (outcome B′). Reproduced twice, the second time as a direct neighbour of the hub (LQI
117 / −57 dBm). Below is what public sources say. Nothing here is a WA104 value.

### Zigbee2MQTT

Source: `zigbee-herdsman-converters/src/devices/schneider_electric.ts`, master, 2026-11-05. Wiser
heating devices that Z2M knows:

| Product | `zigbeeModel` (Basic 0x0005) | Z2M model | Notes |
|---|---|---|---|
| Drayton Wiser radiator thermostat | `iTRV` | WV704R0A0902 | uses custom cluster `wiserDeviceInfo` |
| Wiser radiator thermostat | `CCTFR6100` | CCTFR6100Z3 | uses `wiserDeviceInfo` |
| Wiser smart plug | `SMARTPLUG/1` | CCT711119 | on/off + metering only |
| Heating thermostat | fingerprint modelID `CCTFR6700`/`CCTFR6710`, manufacturerName `Schneider Electric` | CCTFR6700 | |

* Custom cluster `wiserDeviceInfo` = 0xFE03, attribute `deviceInfo` 0x0020, CHAR_STR. It reports
  comma-separated strings such as `ALG,…`, `ADC,…`, `UI,BoostUp`, `MOT,…`.
* `SCHNEIDER_ELECTRIC = 0x105e` (zigbee-herdsman `manufacturerCode.ts`).
* There is no entry for the WA104 / Wiser Range Extender, and the string "Drayton" does not appear
  in the file.

### Other sources

* Drayton sells two routing products: the WA104 range extender and the Wiser smart plug
  (WB704H1A0902), which is also described as extending range. The range extender instructions PDF
  (wiser.draytoncontrols.co.uk) gives pairing steps only, with no Zigbee identifiers.
* Community reports (Home Assistant forum, diynot) say non-Wiser repeaters cannot extend a Wiser
  network, without a documented test or mechanism. This matches M2.

### Still unknown

The WA104's manufacturer name, model identifier, node-descriptor manufacturer code, endpoints and
clusters, and which of these the hub checks before keeping or removing a device.

## 2026-11-05 17:37: M2 with APS inbound logging

Firmware with `aps_inspect.c`; observed from the C6 (log `logs/m2.log`). Air capture
`captures/join-20261105-1732.pcap` (6245 frames, channel 16), not decrypted (no Wireshark on the
laptop).

| t after join | From hub (0x0000) | Detail |
|---|---|---|
| −0.3 s | ZDO Node_Desc_rsp | hub node descriptor: manufacturer code 0x105E, server mask 0x2C41 → stack revision 22 (Zigbee 3.0 TC) |
| −0.3 s | ZDO Mgmt_Permit_Joining_req | permit 15 s, then 180 s |
| −0.2 s | ZDO IEEE_addr_req | for our short address |
| +0.7 s | ZCL Read Attributes, manufacturer-specific code 0x105E, cluster Basic 0x0000, attributes 0xE007, 0xE009 | sent to endpoints 1, 3, 5, 21, 6, 20, 7 in that order, ~20 ms apart |
| +0.8 s | APS acks for our replies | |
| +9.7 s | ZDO Mgmt_Leave_req | device removed |

The hub never read the standard ManufacturerName (0x0004) or ModelIdentifier (0x0005).

What the attributes mean, from the ZHA quirks `zhaquirks/schneiderelectric/__init__.py` (class
`SEBasic`):

* 0xE007 `se_product_identifier`, enum16: "The ProductIdentifier attribute specifies the unique
  internal numerical identifier of the product. See device description for this value."
* 0xE009 `se_product_model`, CharacterString: "The ProductModel attribute specifies the name of the
  product model. Same value as model identifier attribute 0x0005."
* Others in the same cluster: 0xE001 application FW version, 0xE002 HW version, 0xE004 serial number
  (15 hex chars), 0xE008 ProductRange, 0xE00A ProductFamily, 0xE00B VendorURL.
* ZHA quirks put `SEBasic` on endpoint 21 (switches, dimmers), 3 (dimmers) and 6 (outlet), which
  matches the hub's endpoint sweep.

Conclusion at the time (superseded by join #5 below): the hub's admission test is "answer
Schneider's proprietary Basic attributes ProductIdentifier/ProductModel on one of the Wiser
endpoints", which a generic Zigbee 3.0 router cannot pass. No published source found so far gives
the numeric ProductIdentifier values. The smart plug's value could only be learned by reading
0xE007/0xE009 from the plug over the air.

## 2026-11-05 17:53: interview of the router the hub accepts

Done inside the 10 s window. The hub's neighbour table (Mgmt_Lqi) listed one router besides the C6:
0x7798, the owner's "Wiser smart plug". Its interview (`fingerprints/wiser-plug.captured.json`; IEEE
not reused) shows it is not a Schneider device:

| Item | Value |
|---|---|
| Node descriptor | ZR, manufacturer code 0x120B (HEIMAN), MAC cap 0x8E, server mask 0x2C00 (stack rev 22) |
| Endpoint 1 | profile 0x0104, device 0x0051 (Smart Plug), in: 0x0000 0x0003 0x0004 0x0005 0x0006 0x0009 0x0702 0x0B04 0x0B05; out: 0x0003 0x0019 |
| Endpoint 242 | Green Power proxy (profile 0xA1E0, device 0x0061, out 0x0021) |
| Basic | ZCLVersion 3, AppVersion 32, StackVersion 2, HWVersion 22, Manufacturer "HEIMAN", Model "SmartPlug-EF-3.0", DateCode "20231122", PowerSource 1 (mains) |
| Schneider attrs 0xE001/2/7/8/9/A/B (code 0x105E) | all UNSUPPORTED_ATTRIBUTE |

So the hub keeps a device that cannot answer 0xE007/0xE009, and the Schneider probe is not the whole
admission test. The hub must also recognise known OEM devices by their standard identity
(manufacturer code, manufacturer/model strings or device type, in some combination), and remove only
devices it recognises by neither route. Which fields it checks is unknown. Next experiment: present
the plug's product identity (never its IEEE) from the C6 and watch the APS-IN log.

Air captures of the three joins: `captures/join-20261105-1732.pcap`, `join2-…-1748.pcap` (board
dropped off USB mid-capture), `join3-…-1752.pcap` (complete, 969 kB).

## 2026-11-05 18:08: join #4 with the plug identity (inconclusive)

The C6 was given the plug's product identity: HEIMAN / SmartPlug-EF-3.0 / code 0x120B / device
0x0051, with the cluster list exposed through the SDK's standard constructors. OTA 0x0019 was not
exposed.

The C6 failed to associate: 10 BDB steering attempts, all `NO_NETWORK`, verdict
`PERMIT_JOIN_CLOSED`. Air capture `captures/join4-heiman-20261105-1802.pcap` (31013 frames) has no
Association Request/Response and no frame from the C6's IEEE during the window. The C6's own scans
reported PAN 0x382c `closed`/0-permit every time, although the hub's broadcast beacon carried
assoc-permit=1 from 18:08:33 to 18:10:35.

Association never completed, so the hub never read the Basic cluster, and identity cannot affect MAC
association. This run does not test the admission check. Possible causes, not separated:

* The hub opens the association window only briefly. The first three joins associated within ~2-3 s;
  here the first attempt missed it.
* The hub now treats the C6's repeatedly rejected IEEE differently.

Next: retry for a clean association, then see whether the hub keeps the C6.

## 2026-11-05 18:18: join #5 accepted and retained

Same firmware and identity as #4 (HEIMAN / SmartPlug-EF-3.0 / mfr 0x120B / device 0x0051 / plug
cluster set). Association succeeded on the first attempt and the hub did not send Leave. State
stayed `JOINED` (checked after ~1 min and by the helper's run), `Removed after join: no`,
`Leaves: 0`, 5 active routes, neighbours = hub 0x0000 and the real plug 0x7798.

Sequence after join (from the APS-IN log and `captures/join5-heiman-*.pcap`):

1. The hub read Schneider Basic 0xE007/0xE009 on every endpoint. The device returned UNSUPPORTED
   (those values were never set), and the hub kept it.
2. The hub then sent Mgmt_Bind/Bind_req and On/Off (cluster 0x0006) commands, and read metering
   (0x0702) and other clusters. It treats the C6 as a real smart plug.
3. Some NO_ROUTE_AVAILABLE / PARENT_LINK_FAILURE while settling, then stable with routes.

Conclusion (answers the project's main question): a generic ESP32-C6 router can join and stay on this
Drayton Wiser network, but only by presenting the standard identity of a device model the hub
recognises. Here that is the Heiman OEM smart plug: manufacturer code 0x120B + model + device ID. The
Schneider manufacturer-specific attributes are not the gate; the device failed them and was kept. A
generic identity is removed ~10 s after join (#1–#3); the recognised identity is retained (#5).

Not yet verified, but now testable: retention over hours or days, whether the hub eventually marks
the fake plug unresponsive, and whether TRVs route through it (M4/M5).

## 2026-11-05 ~18:30: M4 multi-hop routing verified

The C6 was moved out of the hub's radio range. Its only neighbour was the plug 0x7798 (LQI 122/-56).

* The plug's own neighbour table (Mgmt_Lqi of 0x7798) lists the hub 0x0000 as its parent, so
  plug→hub is a real link.
* C6 route to hub: next hop 0x7798 (plug), state ACTIVE (relayed). The hub is not a direct
  neighbour.
* `ping 0x0000 10`: 10/10 replies across the relay (RTT 45 ms–3.3 s, variable).
* Firmware verdict reached `JOINED_MULTIHOP_VERIFIED` (hub not a neighbour + relayed route to hub
  + end-to-end ping ok).

The path worked but was not stable at this location. The verdict flapped between
`JOINED_MULTIHOP_VERIFIED` and `JOINED_COORDINATOR_REACHABLE` when the route to 0x0000 reverted to a
stale DISCOVERING/direct entry, and RTT spiked to seconds. Multi-hop delivery is proven; long-term
stability at a given spot is a placement and soak question (M6).

## 2026-11-05 ~18:50: On/Off control and metering keep-alive working

With the Heiman plug identity, the C6 now has a working On/Off output and plausible metering:

* App toggle: the hub sends ZCL On/Off `cluster-cmd 0x01` (On) / `0x00` (Off) to ep1 cl 0x0006. The
  firmware drives GPIO0 (configurable) and logs `ONOFF ON/OFF (from the network)`. The hub reads the
  attribute back and the C6 answers. Verified end to end from the Wiser app.
* The output pin is set at runtime and persisted in NVS: CLI
  `relay [on|off|pin <n>|invert on|off]` (Kconfig default WB_RELAY_GPIO=0, active-high). Local
  `relay on/off` overrides it for bench testing.
* Metering keep-alive: while joined, every WB_PLUG_REPORT_S (default 60 s) the firmware refreshes
  Electrical Measurement RMSVoltage (~230 V) and RMSCurrent (0 when off) and re-reports On/Off and
  current to the hub, so the fake plug looks live. It stayed joined throughout; a longer soak is
  still to do.
* Persistence: after reflashing the app partition it came back `JOINED (restored from NVRAM)`
  without re-commissioning (M3 behaviour).

New firmware: `main/aps_inspect.*`, relay in `board.*`, On/Off handler, plug_keepalive and relay
config in `zigbee_router.c`, `relay`/`apslog` CLI, Kconfig
WB_RELAY_GPIO/WB_RELAY_ACTIVE_LOW/WB_PLUG_REPORT_S. The single 802.15.4 radio supports one Zigbee
network only. Wi-Fi can run at the same time (coexistence, with the caveats in
[wifi-bridge-feasibility.md](wifi-bridge-feasibility.md) §6).

## 2026-11-05 ~19:25: M5 and M4 together, TRVs parented to the C6 and relayed to the hub

Re-parenting test: the owner turned the plug (0x7798) off, then on again.

* With the plug off, 7 of its end devices moved to the C6 within ~2 min (C6 `children` 0 -> 7). The
  plug had been the C6's only uplink, so the C6 and those TRVs were cut off from the hub for that
  time. This was expected, and the plug was turned back on promptly.
* With the plug back on, the C6 kept 5 sleepy TRVs as children (0x38e4, 0x917f, 0x6922, 0x72e8,
  0x14f1; sleepy=YES) and regained an ACTIVE relayed route to the hub via the plug.
* End to end: `ping 0x0000` 7/8 replies at ~50 ms, verdict `JOINED_MULTIHOP_VERIFIED`.

Live path verified: TRV -> C6 -> plug -> hub. The C6 parents real Wiser TRVs and relays their
traffic to the hub. RTT fell from several seconds at the mesh edge to ~50 ms once it carried
children mid-mesh.

Milestones demonstrated: M1 build, M2 join (recognised identity), M3 persistence (restore from
NVRAM), M4 multi-hop, M5 TRV as child. Remaining: M6 long-term soak (24 h / 7 day), and confirming
the hub never drops the fake plug over time.

## 2026-11-06: multi-hop proven with the sniffer, antenna-dependent

After the keep-alive panic was fixed (see git history), the C6 was reset, removed from the app and
re-joined. At the far spot (~15 m, several walls, plug between C6 and hub) the uplink route would not
form on the on-board ceramic antenna. The C6 faintly heard the hub's broadcasts and recorded a dead
direct route (`coord_nh=0x0000`) instead of relaying via the plug, so ping 0x0000 was 0/N with
continuous `NO_ROUTE_AVAILABLE`. The hub still reached the C6 (source-routed downlink and ZDO
address/bind requests arrived), so the link was one-way (asymmetric) rather than blocked by hub
policy.

Fitting an external U.FL antenna (`CONFIG_WB_ANTENNA_EXTERNAL=y`, GPIO14 high) fixed it. Captured on
the Wiser channel (16) with the second C6 running `sniffer/` and decoded with
`tools/sniff_route_decode.py` (MAC and NWK headers are in the clear; command payloads are not):

* Uplink C6→hub: `C6 →64→ plug →91→ hub`, 0 direct C6→hub frames.
* Downlink hub→C6: `hub →(source route)→ plug →90→ C6`, 0 direct hub→C6 frames.
* Stability: 25-min hub-ping soak, 17/17 cycles with 3/3 replies, ~45-60 ms, 0 drops.

Conclusion at the time (superseded, see the next section): multi-hop hub↔plug↔C6 works and is
stable with the external antenna, and the Wiser hub source-routes to a third-party router. The limit
was antenna/link asymmetry causing a bad route choice on the ceramic antenna, not a closed-ecosystem
block or a firmware bug.

## 2026-11-06 afternoon: uplink decay, routing loop, live-network incident

### Uplink decay

After the clean 25-min soak, the C6's route to the hub aged out. `routes` showed
`0x0000 0x0000 DISCOVERING MTO=no`, ping 0x0000 was 0/N, with continuous `NLME NO_ROUTE_AVAILABLE`,
while the hub's source-routed downlink still arrived.

The Wiser hub is a concentrator. Routers reach it via a many-to-one route learned from its MTORR
broadcasts, and the hub sends those rarely in a quiet network. The external antenna only helped for
a while. esp-zigbee has no API to install a route, so `hub via plug` cannot be pinned. `pinroute`
(route-cost policy `use_route_for_neighbour`, optionally MAC-filtering the hub) and a boot-time pin
only choose among existing routes, and ping stayed at 0/N. In a 20-min capture the C6 recovered once
after a unicast NWK command from the plug (probably a route reply), not after an MTORR.

### Plug↔C6 routing loop

The captures were decrypted using the network key exported from our own C6 (opt-in
`CONFIG_WB_NETKEY_EXPORT` build; key kept out of logs). The C6 relays the hub's MTORR correctly at
path cost 12–13, while the plug rates its own hub link at the worst cost (7). About 0.5 s after an
MTORR the plug sometimes took the C6 as its many-to-one next hop (its first misrouted frames were
MANY_TO_ONE_ROUTE_FAILURE reports), and the C6 sent the frames back. They bounced between the two
until the radius expired. The loop also happened with the pin disabled, so the pin did not cause it.
The hub does answer the C6's route requests (route reply from the hub in ~250 ms).

### Live-network incident

At 12:22 the plug's branch was healthy (no route requests). By ~16:00 the C6 (~600 per 20 min), the
plug and all six TRVs were flooding route requests for the hub, and several TRVs showed offline in
the Wiser app. Actions taken:

* 16:31: C6 silenced (held in the ROM bootloader, radio off).
* Plug restarted. It relayed for ~1 min, then stuck again.
* ~16:54: hub restarted.
* ~17:10: C6 removed from the Wiser app.
* Plug restarted again.

Root cause found at the plug (~17:40, sniffer beside the hub): a one-way link. The hub heard the plug
(87 % of plug→hub frames acknowledged) and answered it (145 route replies + 108 MTORRs in 2 min,
~1,700 source-routed frames for the plug's TRVs), but the plug almost never heard the hub. So the
plug kept flooding route requests and its TRVs were cut off. The link was already rated cost 7 in
the healthy 12:22 capture. Interference near the plug is suspected (channel 16 = 2430 MHz, which
overlaps Wi-Fi ch 3–6). All TRVs were reporting again by ~17:45.

Causation is not established. The plug stayed stuck while the C6 was silent, but the C6's afternoon
of rejoins, reboots and route-request flooding, and the loop, came first. The C6 was kept off the
live network after this.

### Status after the incident

The hub accepts the C6 (with a recognised identity), relays to it and uses it as a TRV parent. But a
C6 at the edge of range cannot hold a reliable uplink, and it must not sit behind a marginal router
where it can form a loop. Next steps, on a test network or with a healthy plug↔hub link:

* Place the C6 one hop from the hub.
* Consider an auto-rejoin watchdog (risk: frequent rejoins).
* Re-run the loop test with a strong plug↔hub link.

## 2026-11-06 evening to 2026-11-07: rejoined beside the plug, 24 h+ run

After the incident the C6 was rejoined (~18:18) with the plug identity and placed next to the plug
instead of at the far, edge-of-range spot. From `logs/post_join.log`, `c6_at_plugspot*.log` and
`children_check.log`:

* `HEALTH … state=JOINED … verdict=JOINED_COORDINATOR_REACHABLE`. `ping 0x0000` returned 3/3
  (27–33 ms), then 5/5 (37–46 ms). The hub immediately read On/Off and metering, treating the C6 as a
  plug.
* By 18:50, two sleepy TRVs (0x6922, 0x38e4) had chosen the C6 as their parent.

It was then moved to a phone-charger supply (no serial access). On 2026-11-07 the owner reported over
24 h with the C6 and all TRVs online in the Wiser app, and no repeat of the afternoon's
route-request flooding. No HEALTH log covers the period. Opening the USB-JTAG port would reboot the
board, so the next measured check should be one `health` + `multihop` read at the end of a soak, or a
passive sniffer capture.

## 2026-11-07: live capture with two WiserBridge routers

On the evening of 2026-11-06 the board that had been the sniffer was reflashed with the router
firmware: the SuperMini build (`sdkconfig.supermini`, no RF switch, LED on GPIO15) with the Heiman
plug profile. It joined as a second router. An ESP32-C6-Zero became the new sniffer.

A 10-minute ch-16 capture (8,115 frames, sniffer beside the XIAO) was decoded with
`tools/sniff_device_summary.py`. Each node was identified by the IEEE source address in its NWK
frames.

| Short | Device | Role / next hop to hub | Originated | Relayed | MAC retx | Own NWK broadcasts |
|---|---|---|---|---|---|---|
| 0x0000 | Wiser hub | coordinator | 637* | — | 1 % | 16 link status, 20 MTORR |
| 0x2c3c | C6 #1, XIAO (IEEE 98:a3:16:ff:fe:bb:7a:38) | router, hub direct 191/191 | 231 | 654 | 3 % | 40 link status, 0 RREQ |
| 0x578f | C6 #2, ex-sniffer (Espressif OUI 54:32:04) | router, hub direct | 229 | 522 | <1 % | 40 link status, 0 RREQ |
| 0x7798 | Heiman plug | router, hub direct | 121 | 172 | 0 % | 34 link status, 0 RREQ |
| 0x917f, 0x38e4, 0x6922, 0x7183 | TRVs | end devices, parent C6 #1 | 104–114 each | — | 1–5 % | — |
| 0xe230 | end device | parent C6 #1 (2 frames) | 2 | — | — | — |
| 0x9e36 | end device | parent plug | 80 | — | 0 % | — |
| 7 others | end devices | hub direct | 53–110 each | — | 0–4 % | — |

\* The hub was heard at −96 dBm, so its counts are undercounts.

* C6 #1 is the busiest router on the network, with 654 frames relayed in 10 min. Its uplink goes
  directly to the hub. The hub's downlink to it, and the hub's source-routed frames to C6 #1's TRVs,
  go via C6 #2: hub → C6 #2 → C6 #1 → TRV.
* The route-request flooding has not recurred. No router originated a command broadcast with
  radius > 1, and each sent link status about every 15 s. Compare C6 #1's ~600 RREQs per 20 min on
  the afternoon of 2026-11-06.
* C6 #1 has not rejoined since 2026-11-06 ~18:18. It still has short address 0x2c3c, the address it
  got at that join, which partly verifies the owner-reported 24 h run.
* The real plug is no longer the uplink for either C6. Both routers link to the hub directly, which
  fits the owner's report that the route moved from via-plug to direct once that was the better path.
