# Joining the Wiser network

The hub only keeps devices it recognises. A generic router identity joins and is then removed by
the hub about 10 s later (verdict `JOINED_THEN_REMOVED`). The hub keeps the Wiser smart plug
identity (HEIMAN / SmartPlug-EF-3.0, manufacturer code 0x120B, device ID 0x0051), so the firmware
uses that identity by default.

## Safety rules

* Do not unpair, reset or re-pair any TRV, the WA104 or the hub.
* Close pairing mode in the Wiser app as soon as the join is done, so no other device joins.
* The C6 never opens the network itself. It only follows the permit-join that the hub broadcasts.

## Before you start

1. Flash the firmware ([flashing.md](flashing.md)) and open the console:
   `py tools\wb_monitor.py --port COM8 --duration 0 --log logs\commissioning.log`
2. Check the boot banner shows `Zigbee role: ROUTER`, `Auto-join: off` and `Child limit: 20`.
3. Place the C6 near the hub or the WA104 for the first join. Joining through the WA104 works,
   because routers re-broadcast the hub's permit-join.

## Step 1: check the identity

```
wb> profile show
```

Check that it shows:

```
  manufacturer      : "HEIMAN"
  model             : "SmartPlug-EF-3.0"
  device_id         : 0x0051
  manufacturer_code : 0x120b
```

If it shows a generic identity (for example `DIY-ESP32C6` / `ZB-ROUTER-C6`), the board has an old
profile saved in NVS. Restore the defaults:

```
wb> profile defaults
wb> profile save
wb> reboot
```

Run `profile show` again to confirm. If the board had already joined with the old identity, reset it
first (see [Starting over](#starting-over)), then join again.

## Step 2: find the Wiser network

With the hub not in pairing mode, scan:

```
wb> scan
SCAN RESULT: SUCCESS, 4 network(s)
  PAN    extended PAN ID         ch  permit routerCap edCap
  0x7cc1 c5:a9:1c:6a:12:08:eb:2b 11  closed yes       yes
  ...
```

Note every network listed. Neighbours and other Zigbee bridges may appear.

Now start adding a new device in the Wiser Home app. This puts the hub in pairing mode for a limited
time. Run `scan` again straight away. The network that changed from `closed` to `OPEN` is the Wiser
network. If a neighbour could be pairing at the same time, repeat the check.

## Step 3: select the network and join

Set the EPID filter to the Wiser network's extended PAN ID. It is saved in NVS, so `join` only
targets that network.

```
wb> epid c5:a9:1c:6a:12:08:eb:2b        <- use your network's EPID
wb> channels 11                          <- optional; its channel, applies after 'reboot'
wb> join
```

You can also short-press BOOT instead of typing `join`. The LED blinks fast while the firmware
retries network steering for up to 180 s (`CONFIG_WB_STEERING_WINDOW_S`). On success the log shows:

```
I WB_EVENT: JOIN SUCCESS (network steering)
NETWORK
  Joined/commissioned : YES
  Short address       : 0x....
  PAN ID              : 0x....
  Extended PAN ID     : ..
  Channel             : ..
```

Close pairing in the Wiser app.

## Step 4: read the verdict

```
wb> status
wb> diagnostics
wb> events
```

`status` and `diagnostics` print a verdict. If the join window closes without success, the firmware
runs a scan automatically so the verdict is based on what it saw.

| Verdict | Meaning | What to do |
|---|---|---|
| `JOINED_*` | Joined | Wait 10 min and check `events` has no `REMOVED from network`. Then see [multihop-testing.md](multihop-testing.md) |
| `JOINED_THEN_REMOVED` | Hub admitted the C6, then told it to leave | Identity not accepted. Go back to Step 1 |
| `PERMIT_JOIN_CLOSED` | Networks visible, none open | Pairing mode was off or expired. Repeat Step 2 |
| `NO_NETWORK_VISIBLE` | No beacons | Move closer; check antenna and channel mask |
| `NO_ROUTER_CAPACITY` | Open network refuses routers | Move nearer the hub or another router |
| `SECURITY_TCLK_FAILURE` | Association and key OK, link-key exchange failed | Hub may be pre-Zigbee 3.0. Run `tclk off`, `reboot`, retry, and record the result |
| `JOIN_REFUSED_OR_AUTH_FAILED` | Open network with capacity, join still failed | Association refused or key not delivered. Needs a sniffer capture to tell which |
| `STEERING_FAILED` | Failed, no scan evidence | Run `scan` while not joined |
| `STACK_ERROR` | Local stack problem | See [troubleshooting.md](troubleshooting.md) |

Being listed in the Wiser app is separate from joining. Routing works at the network layer either
way.

### Locating a failure

| Layer | Evidence |
|---|---|
| No network | `scan` shows no PANs on the channel mask |
| Permit-join | `scan` shows the Wiser PAN `closed` during your pairing attempt |
| Association or key transport | PAN open with router capacity, steering still fails: `JOIN_REFUSED_OR_AUTH_FAILED`. The SDK only returns an overall status (usually `NO_NETWORK`). A sniffer shows whether the Association Response failed or the Transport-Key never arrived |
| Link-key exchange | `TCLK_EX_FAILURE` |
| Stack or configuration | `STACK_ERROR`, boot errors, problems in `profile show` |
| Joined, no routing yet | `JOINED_NO_ROUTING_EVIDENCE`. Wait 1-2 min for link status, then run `neighbors` and `ping 0x0000` |

## Step 5: record the result

Keep `logs\commissioning.log`. Note the date, hub firmware version, verdict, and whether the device
appeared in the app. Remove EPID and IEEE values before sharing logs if you want them private.

## Starting over

```
wb> reset            (prints a one-time code)
wb> reset confirm NNNN
```

If joined, this broadcasts a Zigbee Leave, clears the Zigbee state (the frame counter is kept, as
the stack requires) and restarts factory-new. Holding BOOT for 10 s and releasing does the same. If
the app still lists the device afterwards, remove it there.
