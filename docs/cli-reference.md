# Serial command reference

Console: USB-C (USB-Serial/JTAG), prompt `wb>`. All commands are read-only unless marked.
Anything the SDK cannot provide is reported as unavailable — never invented.

| Command | Purpose |
|---|---|
| `help` | List commands |
| `status` | State, uptime, network, link counts, **verdict** + advice, heap |
| `network` | Joined/factory-new, role, local IEEE, short address, PAN ID, extended PAN ID, channel, NWK update ID, outgoing NWK frame counter, max children, link-status period, scan channel set, TX power |
| `neighbors` | Local neighbour table: short, IEEE, type (ZC/ZR/ZED), relationship, rx-on-when-idle, LQI, RSSI, outgoing cost, age, depth |
| `routes` | Routing table (dest, next hop, state, many-to-one, expiry, direct/VIA) and route records |
| `children` | Children of this router: sleepy flag, LQI/RSSI, configured timeout, aging counter, count vs. limit |
| `diagnostics` | Join counters by failure class, last BDB status, scan summary, ping result, leave/announce/association counters, NWK status indications, documented SDK limitations |
| `events` | Last 64 timestamped events (joins, leaves, children, permit-join, NWK status, scans, pings) |
| `multihop` | Evidence summary: hub as direct neighbour?, route entry to 0x0000 and its next hop (+ link quality), last ping, children, caveats |
| `health` | One machine-readable `HEALTH key=value …` line (also printed every 300 s) |
| `scan [force]` | Active beacon scan of the join channel mask. **Refused while joined** (it would take the radio off-channel); `force` overrides on a bench network only |
| `join` | *(changes state)* Start joining. Refused if already joined — on a joined router it would open the Wiser network |
| `ping <addr> [n]` | Send ZDO Node_Desc_req `n` times (default 3), print round-trip time. `ping 0x0000` = hub. Harmless/read-only |
| `lqi <addr>` | ZDO Mgmt_Lqi_req: read a *remote* router's neighbour table (e.g. hub `0x0000` or the WA104). Marks this repeater if listed |
| `interview <addr>` | ZDO Node_Desc → Active_EP → Simple_Desc for each endpoint → read Basic cluster attributes; prints a `FINGERPRINT-JSON` line |
| `profile [show\|pending\|set k v\|save\|defaults]` | *(changes config)* Identity/compatibility profile. Keys: `manufacturer`, `model`, `date_code`, `endpoint`, `profile_id`, `device_id`, `device_version`, `manufacturer_code`, `identify`, `max_children`, `power_source`, `in_clusters`, `out_clusters`, `source`. Applies after `save` + `reboot` |
| `epid [<EPID>\|clear\|show]` | *(changes config)* Only join this extended PAN ID |
| `channels [11,15\|0xMASK\|default]` | *(changes config)* Channels searched when joining (after reboot) |
| `tclk [on\|off\|default]` | *(changes config)* Require Trust Center link-key exchange (on = Zigbee 3.0). After reboot |
| `reset` / `reset confirm <code>` | *(destructive)* Leave the network and clear Zigbee state. Requires the one-time code printed by `reset` within 30 s |
| `reboot` | Restart; keeps network membership |
| `version` | Firmware/IDF version |
| `log <tag\|*> <level>` | Runtime log level (`none/error/warn/info/debug`) |

Addresses can be given as short addresses (`0x1a2b`) or IEEE addresses (`00:12:4b:…`, resolved via
the stack's address table).

## HEALTH line fields

`up` uptime · `state` · `short` · `pan` · `ch` · `nbr` neighbours · `rtr` router/hub neighbours ·
`child` children · `sleepy` sleepy children · `routes` active routes · `coord_nh` next hop to the hub
(0xffff = no entry) · `heap`/`minheap` · `rejoins` · `leaves` · `nlme` NWK status indications ·
`noLinks` no-active-links events · `verdict`.

## Things the public SDK does not expose (stated, not faked)

* Count of frames relayed for other devices, MAC retries, CCA failures.
* Per-child poll timestamps or buffered-frame counts.
* Network/link keys (deliberately never read).
