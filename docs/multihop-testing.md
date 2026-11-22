# Multi-hop tests and milestones

Target topology: Hub (0x0000) → WA104 → ESP32-C6 → TRV.

The tests prove four separate things: the C6 joined; its route to the hub goes through the WA104;
traffic actually crossed that relay; and a sleepy TRV uses the C6 as its parent. A TRV being close to
the C6, or working better, proves none of these.

Test types: automated (no hardware), bench (C6 only), live (needs the real Wiser system).

## Milestones

| | Milestone | Type | Pass criteria | Status |
|---|---|---|---|---|
| M1 | Firmware builds | Automated, bench | Builds for esp32c6 with no errors or warnings; boot banner and CLI work on the XIAO | Passed 2026-11-05 ([verification-report.md](verification-report.md)) |
| M2 | Commissioning | Live | `join` tried against the Wiser hub; outcome and verdict recorded with log; no network ever formed | Done 2026-11-05. Generic identity joined, then was removed after ~10 s (3 times). Kept only with a recognised identity (Heiman plug). See [wiser-public-sources.md](wiser-public-sources.md) |
| M3 | Network persistence | Live | After `reboot` and after a power-cycle: `START: network membership restored from NVRAM`, same PAN/short address/channel, `JOINED_*` verdict, no re-commissioning | Open |
| M4 | Multi-hop routing | Live | Test 4 passes: next hop is the WA104 or plug, hub is not a direct neighbour, ping to hub succeeds. Sniffer confirmation preferred | Passed 2026-11-06. See note below |
| M5 | TRV support | Live | At least one TRV listed by `children` (sleepy) with matching IEEE; it keeps reporting in the app; no other device disrupted | Demonstrated 2026-11-05: 5 sleepy TRVs became children of the C6 after the plug was power-cycled; relayed to hub (`JOINED_MULTIHOP_VERIFIED`, ping 7/8). See [wiser-public-sources.md](wiser-public-sources.md) |
| M6 | Reliability | Live | Restart tests pass; `wb_health_report.py` all PASS for 24 h, then 7 days | 24 h passed 2026-11-07 (owner-reported). See note below. 7-day soak open |

**M4 detail.** On 2026-11-05, `ping 0x0000` got 10/10 via the plug, verdict
`JOINED_MULTIHOP_VERIFIED`. On 2026-11-06 a sniffer confirmed it with an external U.FL antenna:
hub ↔ C6 traffic went through plug 0x7798 in both directions, with 0 direct frames either way. A
25-min soak passed 17/17 cycles with 3/3 replies each, at ~45-60 ms. The C6 stayed routed via the
plug until the stack switched to a direct route to the hub when that became the better path (normal
route selection).

At the far spot the uplink later decayed (`NO_ROUTE_AVAILABLE`, ping 0/N) while the downlink still
worked. The hub is a concentrator, and the C6's many-to-one route ages out between MTORRs. The
antenna helped only for a while and `pinroute` did not help. A plug ↔ C6 routing loop and a
live-network incident followed. See [wiser-public-sources.md](wiser-public-sources.md) (2026-11-06
afternoon).

**M6 detail.** The C6 rejoined beside the plug on 2026-11-06 at about 18:18 (ping 0x0000 3/3 and
5/5; 2 sleepy TRV children by 18:50). It then ran for more than 24 h on a phone-charger supply, with
the C6 and all TRVs online in the Wiser app. There is no serial HEALTH log for this period, because
attaching USB-JTAG reboots the board. A 10-min sniffer capture on 2026-11-07 confirmed it was still
joined: same short address 0x2c3c (no rejoin), 4 TRV children, 654 frames relayed, 3 % retransmits,
0 route requests. A second C6 (0x578f) joined as a router on the evening of 2026-11-06 and carries
the hub → C6 #1 downlink.

**Antenna.** The XIAO ESP32-C6 has a ceramic antenna and a U.FL connector. At the edge of the hub's
range, an external antenna (`CONFIG_WB_ANTENNA_EXTERNAL=y`, menuconfig → Board → "Use external U.FL
antenna") stopped the C6 recording a dead direct route to the hub, and the relayed route worked for
~25 min. The uplink still decayed once the many-to-one route aged out. The most reliable placement
found so far is within one hop of the hub. The default stays ceramic.

## Test 1: joined (M2)

Follow [commissioning.md](commissioning.md).

Pass: `JOIN SUCCESS`; `network` shows PAN, EPID, channel and short address; verdict `JOINED_*`; no
`REMOVED from network` event after 10 minutes.

## Test 2: placement

A multi-hop test only counts if the C6 cannot hear the hub directly.

1. Power the C6 from a USB charger and place it between the WA104 and the distant TRVs.
2. Wait 2-3 minutes. Link status is exchanged every 15 s, and neighbours age out after a few misses.
3. Run `neighbors`:
   * The hub (`0x0000`, type ZC) should not be listed, or only with very poor LQI.
   * The WA104 should be listed as a ZR with good LQI. Identify it by IEEE, either from its label or
     app details, or with `interview <short>` (check the Basic cluster manufacturer and model).
4. Run `lqi 0x0000` to read the hub's neighbour table. The C6 should not appear, or only with very
   poor LQI. If it appears with good LQI, the C6 is still in direct range: move it.

## Test 3: relayed through the WA104

```
wb> ping 0x0000 5
wb> multihop
wb> routes
wb> lqi <WA104 short>
```

Pass:
* `multihop` shows *Hub as direct neighbour: no* and *Routing entry to hub: next hop 0xWWWW*, where
  0xWWWW is the WA104, state ACTIVE, result *"RELAYED via another router (multi-hop)"*.
* `lqi <WA104>` lists the C6 (`<- THIS REPEATER`).

## Test 4: multi-hop traffic (M4)

1. While the Test 2 and 3 conditions hold, run `ping 0x0000 10`. Pass is 9/10 replies or better. In
   that situation the Node_Desc_req can only reach the hub and return through the relay. The verdict
   becomes `JOINED_MULTIHOP_VERIFIED`.
2. Repeat 3 times over an hour. Record `routes` each time, as the stack may change routes.
3. Optional independent check: run `sniffer/` on a second 802.15.4 radio on the Wiser channel near
   the C6 and capture while pinging. In Wireshark (IEEE 802.15.4 without FCS) look for:
   * C6 → WA104: `wpan.src16 == <C6>` and `wpan.dst16 == <WA104>` with `zbee_nwk.dst == 0x0000`
   * WA104 → C6: `wpan.src16 == <WA104>` and `wpan.dst16 == <C6>` with `zbee_nwk.src == 0x0000`

   The NWK header is readable without the network key. Only the payload is encrypted.

## Test 5: TRV uses the C6 as parent (M5)

A TRV only picks a new parent when it rejoins, after losing its parent or being re-paired. Do not
unpair TRVs. Options, least intrusive first:

1. Wait. A distant TRV may rejoin on its own. Leave the C6 logging and check `events` and `children`
   daily.
2. Parent-loss test (daytime only). If a distant TRV's parent is the WA104, power the WA104 off
   briefly so its children look for a new parent. This affects every device behind the WA104. Do it
   only when heating is not needed, and power it back on within ~10 minutes.

Pass (all three):
* `events` shows `ASSOCIATED child xx:xx:…` and/or `ANNCE 0x.... … CHILD-OF-US`.
* `children` lists the device with `sleepy YES` and relation `child`.
* The TRV keeps reporting temperature in the Wiser app over the following hours.

A TRV next to the C6 can still be the WA104's child. `children` is the local proof. A sniffer gives
independent proof: data requests from the TRV's short address sent to `wpan.dst16 == <C6>`.

## Test 6: restarts and recovery (M3, M6)

Run each with `wb_monitor.py --log` attached and record the result.

| Test | Procedure | Expected |
|---|---|---|
| ESP32 restart | `reboot`; later unplug and replug power | Banner shows `network membership restored from NVRAM`; same PAN and short address; router neighbours back within ~1-2 min; children poll again (`children`) |
| Hub restart | Restart the Wiser hub (app or power) | C6 stays joined; may log `NO ACTIVE LINKS LEFT` or an isolation warning while the hub is down if it was the only link; `ping 0x0000` works afterwards |
| WA104 restart | Power-cycle the WA104 | If it was the next hop: route errors (`NWK-STATUS …`), then the stack re-routes or waits; `multihop` shows a next hop again after recovery |
| Route loss | Power the C6 off for 10 min, then on | Its children move to other parents while it is off; C6 restores from NVRAM; children may or may not move back |
| Reconnection | Move the C6 out of range of all routers for 20 min, then back | `ISOLATED` warning after 15 min; LED double-blinks; recovers in range without re-commissioning |
| Multiple TRVs | Track `children` over days | Count stays within the limit; no `NO_INDIRECT_CAPACITY` or `INDIRECT_TRANSACTION_EXPIRY` storms in `diagnostics` |

## Test 7: long-running stability (M6)

Start logging and leave it running. Attaching over USB restarts the board once (it stays joined), so attach at the start of the run and leave it attached.

```powershell
mkdir logs
py tools\wb_monitor.py --port COM8 --duration 0 --log logs\soak-24h.log --every 600 --cmd health --cmd-delay 5
```

Afterwards, check the log:

```powershell
py tools\wb_health_report.py logs\soak-24h.log --min-hours 24
py tools\wb_health_report.py logs\soak-7d.log --min-hours 168
```

Pass: every check reports `PASS` (no PANIC, WATCHDOG or BROWNOUT resets; joined in every sample;
never removed by the network; heap not trending down). Every TRV must also have kept reporting in
the Wiser app, and heating must have behaved normally.
