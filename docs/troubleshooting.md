# Troubleshooting

Start with `status`, `diagnostics` and `events` — the verdict and advice usually point at the cause.

## Serial / flashing

| Symptom | Fix |
|---|---|
| No COM port | Use a data cable; check Device Manager for VID_303A/PID_1001 |
| esptool cannot connect | Hold BOOT, tap RESET/re-plug, release BOOT, retry |
| No prompt after opening the port | Press Enter; the REPL prints `wb>` after each command. Use `--reset` to see the boot banner |
| "Your terminal application does not support escape sequences" | Harmless (no line editing in that terminal) |
| Board reboots when a serial tool connects | That tool toggles DTR/RTS. `tools/wb_monitor.py` does not |

## Boot / stack

| Log / verdict | Meaning | Action |
|---|---|---|
| `No production config in zb_fct` | Normal | none |
| `STACK_ERROR`, `START FAILED … retrying` | Stack could not initialise | Check the boot log just before it; if `zb_storage` is suspected corrupt: `erase-flash` and reflash (loses membership) |
| `Endpoint registration failed` | Invalid profile | `profile show`; `profile defaults`; `reboot` |
| Reset reason `BROWNOUT` | Supply dipped | Better USB PSU/cable |
| Reset reason `PANIC`/`WATCHDOG` | Firmware fault | Keep the log (the panic backtrace prints before reboot) and report it |

## Joining

| Verdict | Likely cause | Action |
|---|---|---|
| `PERMIT_JOIN_CLOSED` | Hub not in pairing mode (or it timed out) | Re-open pairing in the app, `scan` to confirm OPEN, `join` |
| `NO_NETWORK_VISIBLE` | Out of range / wrong channel mask / antenna | Move closer; `channels default`; check `CONFIG_WB_ANTENNA_EXTERNAL` matches the hardware |
| `NO_ROUTER_CAPACITY` | Beaconing devices refuse routers | Try a spot near a different router or the hub |
| `SECURITY_TCLK_FAILURE` | Trust Center link-key update failed | `tclk off`, `reboot`, `join`. Record it — this indicates a legacy (pre-R21) trust centre |
| `JOIN_REFUSED_OR_AUTH_FAILED` | Association refused, or the trust centre did not send/accept the key (allow-list, install-code-only policy) | Needs a sniffer capture of the attempt (association response status; presence of Transport-Key) |
| `JOINED_THEN_REMOVED` | Hub admitted then sent Leave | App-level rejection. See protocol-investigation.md for the identity investigation |
| Joined the **wrong** network | No EPID filter and another network was open | `reset` → `reset confirm`, then `epid <Wiser EPID>`, retry |
| `join` → `REFUSED: already joined` | Intended: steering on a joined router opens the network | `reset` first if you really want to re-commission |

## Running

| Symptom | Meaning | Action |
|---|---|---|
| LED double-blink / `ISOLATED for …` | Joined but no router/hub neighbour | Hub/WA104 down or C6 out of range; move it or check power. The stack keeps its state and recovers when links return |
| `NO ACTIVE LINKS LEFT` | All router links aged out | Same as above |
| `NWK-STATUS NO_ROUTE_AVAILABLE` / `…LINK_FAILURE` | Route errors reported by the stack | Occasional ones are normal (devices restarting). Persistent ones → poor placement |
| `NWK-STATUS BAD_FRAME_COUNTER` | Frame counter rejected | Often after a full erase + rejoin while neighbours still hold the old counter; usually clears as entries age. Use `reset` (local reset keeps the counter) instead of `erase-flash` when re-commissioning |
| `NWK-STATUS NO_INDIRECT_CAPACITY` / `INDIRECT_TRANSACTION_EXPIRY` | Buffered frames for sleepy children overflowed/expired | Too many children or a child that stopped polling; check `children` |
| Children = 0 for days | TRVs did not choose the C6 | Normal unless a TRV rejoins; see multihop-testing.md Test 5 |
| `scan` refused | Joined | Intended (scan leaves the channel). Use `scan force` only on a bench network |
| `lqi` returns `NOT_SUPPORTED` | Remote device does not implement Mgmt_Lqi | Valid answer, not an error in this firmware |
| `ping` to a TRV times out | Sleepy device answers only after its next poll | Expected; ping routers/hub instead |

## Network key rotation

If the hub changed the network key while the C6 was off, the C6 cannot decrypt traffic after power-up
(routes fail, isolation warnings). Recovery: `reset` → `reset confirm`, then commission again.
