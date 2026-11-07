# Multi-hop verification, milestones and reliability tests

Target topology: **Hub (0x0000) → WA104 → ESP32-C6 → TRV**.

The tests below separate five claims that are easy to confuse:

1. **Joined** — the C6 is a member of the Wiser network.
2. **Connected through an intermediate router** — the C6's path to the hub goes via the WA104.
3. **Traffic delivered across multiple hops** — something actually travelled hub ↔ C6 via the relay.
4. **A sleepy TRV uses the C6 as its parent.**
5. *(not proof of anything)* A TRV is **physically close** to the C6.

Never infer (2)–(4) from distance or from the TRV "working better". Each claim needs the evidence
listed below.

Legend: 🤖 automated (no hardware) · 🧪 bench (C6 only) · 🏠 needs the real Wiser system.

---

## Milestones

| | Milestone | Type | Pass criteria | Status |
|---|---|---|---|---|
| M1 | Firmware builds | 🤖🧪 | Builds for esp32c6 with no errors/warnings; boot banner + CLI work on the XIAO | **Passed 2026-11-05** (see verification-report.md) |
| M2 | Generic Zigbee commissioning | 🏠 | `join` attempted against the Wiser hub; outcome A/B/B′/C recorded with verdict + log; no network ever formed | **Done 2026-11-05**: generic identity joined then REMOVED ~10 s (outcome B′, 3×); joined and RETAINED only with a recognised model identity (Heiman plug). See docs/wiser-public-sources.md |
| M3 | Network persistence | 🏠 | After `reboot` and after a power-cycle: `START: network membership restored from NVRAM`, same PAN/short/channel, `JOINED_*` verdict, no re-commissioning | open |
| M4 | Multi-hop routing | 🏠 | Test 4 below passes (relay next hop = WA104/plug, hub not a direct neighbour, ping to hub succeeds) — ideally confirmed by sniffer | **Passed 2026-11-06.** 2026-11-05: `ping 0x0000` 10/10 via the plug, verdict `JOINED_MULTIHOP_VERIFIED`. 2026-11-06, sniffer-confirmed with an external U.FL antenna: hub↔C6 relayed entirely via plug 0x7798 in BOTH directions (0 direct frames either way); 25-min soak 17/17 cycles 3/3 replies, ~45-60 ms. The C6 stayed routed via the plug for a sustained period until the stack moved to a direct route to the hub when that became the better path (normal route selection). **Caveat at the far spot:** afterwards the uplink sometimes decayed (`NO_ROUTE_AVAILABLE`, ping 0/N) while downlink still worked: the hub is a concentrator and the C6's many-to-one route ages out between MTORRs; the antenna only helped transiently and `pinroute` did not help. A plug↔C6 routing loop and a live-network incident followed — see docs/wiser-public-sources.md (2026-11-06 afternoon). |
| M5 | Wiser TRV support | 🏠 | ≥1 TRV listed by `children` (sleepy) with matching IEEE, TRV keeps reporting in the app, no other device disrupted | **Demonstrated 2026-11-05**: 5 sleepy TRVs became children of the C6 after the plug was power-cycled; relayed to hub (JOINED_MULTIHOP_VERIFIED, ping 7/8). See wiser-public-sources.md |
| M6 | Reliability | 🏠 | Restart tests pass; `wb_health_report.py` all PASS for 24 h, then 7 days | **24 h passed (owner-reported) 2026-11-07**: rejoined 2026-11-06 ~18:18 beside the plug (ping 0x0000 3/3, 5/5; 2 sleepy TRV children by 18:50) and ran >24 h on a phone-charger supply with the C6 and all TRVs online in the Wiser app. No serial HEALTH log for the period (USB-JTAG attach reboots the board). 7-day soak open |

> **Antenna note.** The Seeed XIAO ESP32-C6 has an on-board ceramic antenna and a U.FL connector. At
> the edge of the hub's range, an external U.FL antenna (`CONFIG_WB_ANTENNA_EXTERNAL=y`, menuconfig →
> Board → "Use external U.FL antenna") stopped the C6 from recording a dead *direct* route to the hub,
> and the relayed route then worked for ~25 min. It is **not a fix**: the uplink still decayed once the
> many-to-one route aged out. The most reliable placement found so far is within one hop of the hub.
> The default stays ceramic so boards with no external antenna don't select a disconnected connector.

---

## Test 1 — Joined (M2) 🏠

Follow [commissioning.md](commissioning.md). Evidence: `JOIN SUCCESS`, `network` shows PAN/EPID/
channel/short address, verdict `JOINED_*`. Wait 10 minutes: no `REMOVED from network` event.

## Test 2 — Placement for a genuine multi-hop test 🏠

A multi-hop test is only meaningful if the C6 **cannot** hear the hub directly.

1. Move the C6 (powered from a USB charger, laptop optional) to a spot between the WA104 and the
   distant TRVs.
2. Give it 2–3 minutes (link status is exchanged every 15 s; neighbours age out after a few misses).
3. `neighbors`:
   * The hub (`0x0000`, type ZC) should **not** be listed, or only with a very poor LQI.
   * The WA104 should be listed as a ZR with a decent LQI. Identify it by IEEE: match it against the
     WA104's label/app details if available, or run `interview <short>` and check its Basic cluster
     manufacturer/model strings.
4. `lqi 0x0000` — the hub's own neighbour table. **This repeater should not appear** (or only with
   very poor LQI). If the hub lists the C6 with good LQI, the C6 is still in direct range: move it.

## Test 3 — Connected through the WA104 🏠

```
wb> ping 0x0000 5
wb> multihop
wb> routes
wb> lqi <WA104 short>
```

Pass:
* `multihop` → *Hub as direct neighbour: no*; *Routing entry to hub: next hop 0xWWWW* where 0xWWWW
  is the WA104's short address, state ACTIVE → *"RELAYED via another router (multi-hop)"*.
* `lqi <WA104>` lists this repeater (`<- THIS REPEATER`) — the WA104 sees the C6 as a neighbour.

## Test 4 — Traffic delivered across multiple hops (M4) 🏠

* `ping 0x0000 10` gets ≥9/10 replies **while** Test 2/3 conditions hold (hub not a neighbour, next
  hop = WA104). A Node_Desc_req from the C6 can only reach the hub and come back through a relay
  in that situation. Verdict becomes `JOINED_MULTIHOP_VERIFIED`.
* Repeat 3× over an hour (`routes` may change as the stack re-evaluates; record each).
* **Independent proof (recommended):** with a *second* 802.15.4 radio running `sniffer/` on the Wiser
  channel near the C6, capture while pinging. In Wireshark (IEEE 802.15.4 without FCS) look for:
  * C6 → WA104: `wpan.src16 == <C6>` and `wpan.dst16 == <WA104>` with `zbee_nwk.dst == 0x0000`
  * WA104 → C6: `wpan.src16 == <WA104>` and `wpan.dst16 == <C6>` with `zbee_nwk.src == 0x0000`
  The NWK header is readable without the network key; only the payload is encrypted.

## Test 5 — A sleepy TRV uses the C6 as its parent (M5) 🏠

A TRV only picks a new parent when it **rejoins** (after losing its parent or when it is re-paired).
Do not unpair TRVs. Options, least to most intrusive:

1. **Wait.** A struggling distant TRV may rejoin by itself. Leave the C6 running with logging and
   check `events`/`children` daily.
2. **Parent-loss test (planned, daytime):** if a distant TRV's current parent is the WA104, briefly
   powering the WA104 off makes children of the WA104 look for a new parent. This affects every
   device behind the WA104 for a few minutes — do it only when heating is not needed, and power it
   back on within ~10 minutes.

Evidence that a TRV chose the C6 (all three are expected):
* `events` shows `ASSOCIATED child xx:xx:…` (MAC association with this router) and/or
  `ANNCE 0x.... … CHILD-OF-US`.
* `children` lists the device with `sleepy YES` and relation `child`.
* The TRV keeps reporting temperature in the Wiser app over the following hours.

**Proximity is not evidence.** A TRV next to the C6 can still be the child of the WA104.
`children` is the authoritative local view; the sniffer view is the independent one (data requests
from the TRV's short address sent to `wpan.dst16 == <C6>`).

## Test 6 — Restarts and recovery (M3, M6) 🏠

Run each with `wb_monitor.py --log` attached and record the outcome.

| Test | Procedure | Expected |
|---|---|---|
| ESP32 restart | `reboot`; later unplug/replug power | Banner → `network membership restored from NVRAM`; same PAN/short; router neighbours back within ~1–2 min; children rejoin/poll again (`children`) |
| Hub restart | Restart the Wiser hub (app or power) | C6 stays JOINED; possibly `NO ACTIVE LINKS LEFT` / isolation warning while the hub is down if it was the only link; links recover; `ping 0x0000` OK afterwards |
| WA104 restart | Power-cycle the WA104 | If WA104 was the next hop: route errors (`NWK-STATUS …`) then the stack re-routes or waits; `multihop` shows the next hop again after recovery |
| Temporary route loss | Power the C6 off for 10 min, then on | Its children move to other parents while it is off (that is the stack's job); C6 restores from NVRAM; children may or may not move back (normal) |
| Network reconnection | Move the C6 out of range of all routers for 20 min, then back | `ISOLATED` warning after 15 min; LED double-blink; recovers when back in range without re-commissioning |
| Multiple TRVs | Over days, track `children` | Count ≤ limit; no `NO_INDIRECT_CAPACITY` / `INDIRECT_TRANSACTION_EXPIRY` storms in `diagnostics` |

## Test 7 — Long-running stability (M6) 🏠

```powershell
mkdir logs
py tools\wb_monitor.py --port COM8 --duration 0 --log logs\soak-24h.log --every 600 --cmd health --cmd-delay 5
```

(Leave it running; it does not reset the board when it attaches.) Afterwards:

```powershell
py tools\wb_health_report.py logs\soak-24h.log --min-hours 24
py tools\wb_health_report.py logs\soak-7d.log --min-hours 168
```

Pass = all checks `PASS`: no PANIC/WATCHDOG/BROWNOUT resets, joined in every sample, never removed by
the network, heap not trending down. Also check physically: every TRV kept reporting in the Wiser app
for the whole period and heating behaved normally.
