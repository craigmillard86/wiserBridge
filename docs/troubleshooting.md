# Troubleshooting

Run `status`, `diagnostics` and `events` first. The verdict in `status` usually names the cause.

## Removed about 10 s after joining (`JOINED_THEN_REMOVED`)

The hub removes devices with a generic identity, such as the old default `DIY-ESP32C6` /
`ZB-ROUTER-C6` (device 0x0008). Current builds use the Wiser smart plug identity (`HEIMAN` /
`SmartPlug-EF-3.0`, device 0x0051).

1. Run `profile show`. If it shows `DIY-ESP32C6` / `ZB-ROUTER-C6`, run `profile defaults`,
   `profile save`, `reboot`. On an older build without the plug defaults, set the fields one by one
   (see [README.md](../README.md)). If you build from source, check `sdkconfig` has the new defaults
   (see [building.md](building.md)).
2. Open pairing in the Wiser app and run `join`.
3. If it is still removed, run `events` and note the seconds between join and Leave. Open an issue
   with the output of `profile show` and `events`.

## Serial and flashing

| Symptom | Fix |
|---|---|
| No COM port | Use a data cable. Look for VID_303A/PID_1001 in Device Manager |
| esptool cannot connect | Hold BOOT, press RESET or replug, release BOOT, retry |
| No prompt | Press Enter. Use `wb_monitor.py --reset` to see the boot banner |
| "Your terminal application does not support escape sequences" | Harmless |
| Board reboots when the port opens | Expected on USB-Serial/JTAG. Network membership is kept |

## Boot and stack

| Log | Action |
|---|---|
| `No production config in zb_fct` | Normal. Ignore |
| `STACK_ERROR`, `START FAILED ... retrying` | Check the log just before it. If `zb_storage` is corrupt, `erase-flash` and reflash (loses membership) |
| `Endpoint registration failed` | Invalid profile. Run `profile show`, then `profile defaults`, `profile save`, `reboot` |
| Reset reason `BROWNOUT` | Use a better USB supply or cable |
| Reset reason `PANIC` or `WATCHDOG` | Firmware fault. Keep the log (it includes the backtrace) and report it |

## Joining

| Verdict | Cause | Action |
|---|---|---|
| `PERMIT_JOIN_CLOSED` | Hub not in pairing mode, or pairing timed out | Open pairing in the app, `scan` to confirm OPEN, `join` |
| `NO_NETWORK_VISIBLE` | Range, channel mask or antenna | Move closer. `channels default`. Check `CONFIG_WB_ANTENNA_EXTERNAL` matches the antenna |
| `NO_ROUTER_CAPACITY` | Nearby devices refuse routers | Try near a different router or the hub |
| `SECURITY_TCLK_FAILURE` | Link-key update failed | `tclk off`, `reboot`, `join`. Note it: this means a legacy (pre-R21) trust centre |
| `JOIN_REFUSED_OR_AUTH_FAILED` | Association refused, or key not sent or accepted (allow-list, install-code-only policy) | Sniff the attempt: check the association response status and whether a Transport-Key was sent |
| `JOINED_THEN_REMOVED` | Hub rejected the identity | See the first section |
| Joined the wrong network | No EPID filter, and another network was open | `reset`, `reset confirm <code>`, `epid <Wiser EPID>`, then join again |
| `REFUSED: already joined` | Intended: `join` on a joined router would open the network | `reset` first if you want to re-commission |

## Running

| Symptom | Cause | Action |
|---|---|---|
| LED double blink, `ISOLATED for ...` | No router or hub neighbour | Hub or WA104 down, or out of range. Move the C6 or check power. It recovers when links return |
| `NO ACTIVE LINKS LEFT` | All router links aged out | As above |
| `NWK-STATUS NO_ROUTE_AVAILABLE` / `...LINK_FAILURE` | Route errors | Occasional ones are normal. Persistent ones mean poor placement |
| `NWK-STATUS BAD_FRAME_COUNTER` | Frame counter rejected | Often after `erase-flash` and rejoin while neighbours hold the old counter; clears as entries age. Re-commission with `reset` (keeps the counter), not `erase-flash` |
| `NWK-STATUS NO_INDIRECT_CAPACITY` / `INDIRECT_TRANSACTION_EXPIRY` | Buffered frames for sleepy children overflowed or expired | Too many children, or one stopped polling. Check `children` |
| Children = 0 for days | No TRV chose the C6 | Normal until a TRV rejoins. See Test 5 in [multihop-testing.md](multihop-testing.md) |
| `scan` refused | Device is joined | Intended. `scan force` on a bench network only |
| `lqi` returns `NOT_SUPPORTED` | Remote device lacks Mgmt_Lqi | Valid answer |
| `ping` to a TRV times out | Sleepy devices answer after their next poll | Ping routers or the hub instead |

## Network key changed while the C6 was off

The C6 cannot decrypt traffic (route failures, isolation warnings). Run `reset`, `reset confirm
<code>`, then commission it again.
