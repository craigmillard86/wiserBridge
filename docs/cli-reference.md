# Serial command reference

The console is on USB-C (USB-Serial/JTAG) with the prompt `wb>`. Commands are read-only unless
marked. Values the SDK cannot provide are reported as unavailable.

Addresses can be short (`0x1a2b`) or IEEE (`00:12:4b:...`, resolved through the stack's address
table).

## Read-only

| Command | Shows |
|---|---|
| `help` | Command list |
| `status` | State, uptime, network, link counts, verdict and advice, heap |
| `network` | Join state, role, IEEE and short address, PAN ID, extended PAN ID, channel, NWK update ID, outgoing frame counter, max children, link-status period, scan channels, TX power |
| `neighbors` | Neighbour table: short, IEEE, type (ZC/ZR/ZED), relationship, rx-on-when-idle, LQI, RSSI, outgoing cost, age, depth |
| `routes` | Routing table (destination, next hop, state, many-to-one, expiry, direct or via) and route records |
| `children` | Children: sleepy flag, LQI/RSSI, timeout, aging counter, count against the limit |
| `diagnostics` | Join failure counters, last BDB status, scan summary, ping result, leave/announce/association counters, NWK status indications, SDK limitations |
| `events` | Last 64 timestamped events |
| `multihop` | Whether the hub is a direct neighbour, route and next hop to 0x0000, last ping, children |
| `health` | One `HEALTH key=value ...` line (also printed every 300 s) |
| `scan [force]` | Beacon scan of the join channels. Refused while joined because it leaves the channel; `force` overrides, on a bench network only |
| `ping <addr> [n]` | Sends ZDO Node_Desc_req `n` times (default 3) and prints round-trip times. `ping 0x0000` = hub |
| `lqi <addr>` | Reads a remote router's neighbour table (Mgmt_Lqi_req), for example the hub or WA104 |
| `interview <addr>` | Reads node, endpoint and Basic cluster descriptors and prints a `FINGERPRINT-JSON` line |
| `apslog [on\|off]` | Logs each ZDO/ZCL request sent to this node (tagged `APS-IN` / `WB_APSIN`) |
| `version` | Firmware and IDF version |
| `log <tag\|*> <level>` | Sets log level: `none`, `error`, `warn`, `info`, `debug` |

## Changes state or settings

| Command | Effect |
|---|---|
| `join` | Starts joining. Refused when already joined, because it would open the Wiser network |
| `profile [show\|pending\|set k v\|save\|defaults]` | Identity profile. Applies after `save` and `reboot`. Keys: `manufacturer`, `model`, `date_code`, `endpoint`, `profile_id`, `device_id`, `device_version`, `manufacturer_code`, `identify`, `max_children`, `power_source`, `in_clusters`, `out_clusters`, `source` |
| `epid [<EPID>\|clear\|show]` | Join only this extended PAN ID |
| `channels [11,15\|0xMASK\|default]` | Channels searched when joining. Applies after reboot |
| `tclk [on\|off\|default]` | Require Trust Center link-key exchange (`on` = Zigbee 3.0). Applies after reboot |
| `relay [on\|off\|pin <n\|default>\|invert <on\|off\|default>]` | On/Off output driven by the plug On/Off cluster. `on`/`off` override locally. Pin settings are saved and apply at once |
| `reboot` | Restart. Keeps network membership |
| `reset`, then `reset confirm <code>` | Destructive: leaves the network and clears Zigbee state. Confirm with the printed code within 30 s |

To restore the default plug identity: `profile defaults`, `profile save`, `reboot`.

## Experimental and diagnostic

| Command | Effect |
|---|---|
| `pinroute [on\|hard\|off\|status]` | Experimental. `on` sets route-cost policy `use_route_for_neighbour` (prefer a relayed route over a direct link to the hub); `hard` also MAC-filters the hub's direct frames. Uses test-grade SDK APIs. Tested and did not help: it picks among existing routes and cannot create a missing one. `CONFIG_WB_PIN_HUB_ROUTE` applies `on` at every join (default off) |
| `netkey`, then `netkey confirm <code>` | Only with `CONFIG_WB_NETKEY_EXPORT=y` (off by default). Prints the network key once, to decrypt your own sniffer captures. Never enable it in a build you distribute |

## HEALTH line fields

`up` uptime, `state`, `short` address, `pan`, `ch` channel, `nbr` neighbours, `rtr` router and hub
neighbours, `child` children, `sleepy` sleepy children, `routes` active routes, `coord_nh` next hop
to the hub (0xffff = none), `heap`/`minheap`, `rejoins`, `leaves`, `nlme` NWK status indications,
`noLinks` no-active-links events, `verdict`.

## Not available from the SDK

- Frames relayed for other devices, MAC retries, CCA failures.
- Per-child poll times and buffered-frame counts.
- Network and link keys (except through `netkey` in a `CONFIG_WB_NETKEY_EXPORT=y` build).
