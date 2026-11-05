# Commissioning with the Drayton Wiser Heat Hub

**Goal:** find out — with evidence — whether the Wiser hub lets this generic Zigbee router join its
network, and whether it then keeps it. This is the core unverified assumption of the project.

Joining at the *network* level and being *recognised in the Wiser app* are different things:

| Outcome | What it means | How you tell |
|---|---|---|
| **A** Joins and recognised | Hub admitted it and the app lists a new device | `status` = JOINED…, app shows a device |
| **B** Joins, not recognised | Network admission and key transport succeeded, the app does not list it | `status` = JOINED…; nothing new in app; **no** `REMOVED` within ~10 min |
| **B′** Joins, then removed | Admitted, then told to leave (hub rejected it at application level) | verdict `JOINED_THEN_REMOVED`, event `REMOVED from network…` |
| **C** Join fails | Not admitted | verdict is one of the failure classes below |

Outcome **B** may still be useful: Zigbee routing happens in the network layer and does not depend on
the Wiser app knowing what the device is. Whether the Wiser hub and TRVs actually route through an
unrecognised router is exactly what [multihop-testing.md](multihop-testing.md) verifies.

## Safety rules

* Do **not** unpair, reset or re-pair any TRV, the WA104 or the hub for these tests.
* Cancel/close pairing mode in the Wiser app as soon as the test is over, so no stray device joins.
* Prefer a time when a short heating hiccup would not matter (it should not happen, but be prepared).
* The C6 never opens your network itself. It only follows permit-join that the hub broadcasts.

## Before you start

1. Flash the router firmware ([flashing.md](flashing.md)), open the console
   (`py tools\wb_monitor.py --port COM8 --duration 0 --log logs\commissioning.log`).
2. Check the banner: `Zigbee role: ROUTER`, `Auto-join: off`, `Child limit: 20`.
3. Place the C6 within reasonable range of the hub **or** the WA104 for the first join (joining
   through the WA104 is fine: routers re-broadcast the hub's permit-join).

## Step 1 — identify the Wiser network (baseline scan)

With the hub **not** in pairing mode:

```
wb> scan
SCAN RESULT: SUCCESS, 4 network(s)
  PAN    extended PAN ID         ch  permit routerCap edCap
  0x7cc1 c5:a9:1c:6a:12:08:eb:2b 11  closed yes       yes
  ...
```

Several networks may be visible (the development bench saw four: neighbours, the Sonoff bridge, …).
Write them down.

## Step 2 — open the Wiser network

In the Wiser Home app, start adding a new device / enter pairing mode on the hub (the exact menu
wording depends on the app version). This makes the hub broadcast permit-join for a limited time.

Immediately run `scan` again. **The PAN that changed from `closed` to `OPEN` is the Wiser network**
(unless a neighbour happens to be pairing at the same moment — repeat if unsure). Pin it:

```
wb> epid c5:a9:1c:6a:12:08:eb:2b        <- use YOUR open network's EPID
wb> channels 11                          <- its channel (optional; needs 'reboot' to apply)
```

The EPID filter is saved in NVS and makes the next `join` target only that network.

## Step 3 — join

```
wb> join
```

(or short-press BOOT). The LED blinks fast. The firmware runs network steering repeatedly for up to
180 s (`CONFIG_WB_STEERING_WINDOW_S`). Watch the log. On success:

```
I WB_EVENT: JOIN SUCCESS (network steering)
NETWORK
  Joined/commissioned : YES
  Short address       : 0x....
  PAN ID              : 0x....
  Extended PAN ID     : ..
  Channel             : ..
```

Then **close pairing in the Wiser app**.

## Step 4 — classify the outcome

```
wb> status
wb> diagnostics
wb> events
```

`status`/`diagnostics` print a **verdict** and advice:

| Verdict | Meaning | Next action |
|---|---|---|
| `JOINED_*` | Network join succeeded | Check the app (A vs B); wait 10 min for B′; go to multihop-testing.md |
| `JOINED_THEN_REMOVED` | Hub admitted then expelled it | Outcome B′ — capture logs; consider the WA104 identity investigation |
| `PERMIT_JOIN_CLOSED` | Networks visible, none open | Pairing mode was not active/expired; retry Step 2 |
| `NO_NETWORK_VISIBLE` | No beacons at all | Range/antenna/channel mask; move closer |
| `NO_ROUTER_CAPACITY` | Open network refuses routers | Try nearer another router/hub |
| `SECURITY_TCLK_FAILURE` | Association + key OK, link-key exchange failed | Hub may be pre-Zigbee-3.0: `tclk off`, `reboot`, retry; record this |
| `JOIN_REFUSED_OR_AUTH_FAILED` | Open network with capacity, still failed | Association refused or key not delivered (allow-list / install codes?). Needs a sniffer capture to separate |
| `STEERING_FAILED` | Failed, no scan evidence | Run `scan` (not joined) for evidence |
| `STACK_ERROR` | Local stack problem | See troubleshooting.md |

When the join window closes without success, the firmware **automatically runs a scan** so the
verdict is evidence-based.

### Distinguishing the failure layers

| Layer | Evidence available here |
|---|---|
| No available network | scan shows 0 PANs on the channel mask |
| Permit-join restriction | scan shows the Wiser PAN `closed` during your pairing attempt |
| Association / commissioning | PAN open with router capacity, steering still fails → `JOIN_REFUSED_OR_AUTH_FAILED`. The public SDK only returns an overall BDB status (typically `NO_NETWORK`), so it cannot say *which* step failed; a sniffer distinguishes an Association Response with a non-success status from a missing Transport-Key |
| Security / key establishment | `TCLK_EX_FAILURE` |
| Stack / configuration | `STACK_ERROR`, boot errors, `profile show` problems |
| Joined, no routing evidence | `JOINED_NO_ROUTING_EVIDENCE` — wait 1–2 min for link status, then `neighbors`, `ping 0x0000` |

## Step 5 — record the result

Keep `logs\commissioning.log` and note: date, hub firmware version (from the app if shown), the
outcome (A/B/B′/C), the verdict, and whether anything appeared in the app. If you share results,
remove the EPID/IEEE values if you consider them private.

## If you need to start over

```
wb> reset            (prints a one-time code)
wb> reset confirm NNNN
```

When joined this broadcasts a Zigbee Leave, clears the Zigbee state (keeping the frame counter as the
stack requires) and restarts factory-new. If the hub still lists the device in the app afterwards,
remove it there. Holding BOOT for 10 s and releasing does the same.
