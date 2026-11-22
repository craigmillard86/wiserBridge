# Device identity investigation and packet sniffing

This guide was written before we knew how the Wiser hub decides which routers to keep. The answer is
now known (see the [Wiser investigation log](wiser-public-sources.md)): the hub keeps a router that
presents the standard identity of a device it recognises, and removes a generic router about 10 s
after it joins. The firmware default identity is therefore the Wiser smart plug: HEIMAN /
SmartPlug-EF-3.0, node-descriptor manufacturer code 0x120B, device ID 0x0051 (set in
`main/Kconfig.projbuild`).

The methods below are still useful for interviewing other devices (for example the WA104 range
extender) and for diagnosing joins with a sniffer.

Rules: the WA104 stays unmodified and paired to the hub. No fingerprint value is guessed. The WA104's
IEEE address, install code and keys are never copied into this project.

## What to collect

Manufacturer string, model identifier, node-descriptor manufacturer code, endpoint numbers, profile
IDs, device IDs, input/output clusters, Basic cluster attributes, proprietary (0xFC00–0xFFFF)
clusters, and node capabilities (logical type, MAC capability flags, server mask / stack compliance
revision). Template: [`fingerprints/wa104.template.json`](../fingerprints/wa104.template.json)
(schema: `fingerprints/fingerprint.schema.json`, enforced by `tools/wb_fingerprint.py`).

## Methods, least intrusive first

### 1. Interview over the air from the joined C6 (preferred)

This needs the C6 to be joined to the Wiser network. The WA104 answers standard ZDO and ZCL requests
from any network member, and nothing on it is changed.

```
wb> neighbors                      # find the WA104's short address (a ZR)
wb> interview 0x1a2b               # its short address
...
FINGERPRINT-JSON {...}
```

Then on the PC, with the console log saved via `wb_monitor.py --log`:

```powershell
py tools\fingerprint_from_log.py logs\interview.log --label wa104 --out fingerprints\wa104.captured.json
py tools\fingerprint_check.py fingerprints\wa104.captured.json
```

The interview also reports the node descriptor's stack compliance revision. `interview 0x0000` does
the same for the hub. A value of 21 or more means R21+/Zigbee 3.0; a lower value suggests a legacy
trust centre, which matters for the `tclk` setting.

Limits: manufacturer-specific attributes and commands cannot be discovered this way, and some
devices answer only part of the Basic cluster.

### 2. Public sources

Record what you find, with links, in the fingerprint's `evidence` list (method `public-database`).
Places to search:

* The Zigbee2MQTT supported-devices list and `zigbee-herdsman-converters` (search "Drayton",
  "Wiser", "Schneider", "WA104").
* Zigbee device compatibility repositories (e.g. blakadder's), ZHA quirks, the deCONZ device
  database.
* The Zigbee Alliance/CSA certified-product database. Certificates sometimes list device type and
  stack.
* Zigbee OTA indexes (e.g. the Koenkk `zigbee-OTA` index). An OTA image header contains a
  manufacturer code and image type. Use only what is published there.

What we found is in the [Wiser investigation log](wiser-public-sources.md). None of it is a WA104
value; treat every WA104 field as unknown until you have a reference.

### 3. Separate coordinator / Zigbee2MQTT (last resort)

A Zigbee2MQTT (or ZHA) coordinator performs a full interview and stores it in `data/database.db`:

```powershell
py tools\z2m_extract.py path\to\database.db --list
py tools\z2m_extract.py path\to\database.db --ieee 0x<wa104 ieee> --label wa104 --out fingerprints\wa104.captured.json
```

This requires the WA104 to leave the Wiser network and join the other coordinator. That means
factory-resetting it and re-pairing it to Wiser afterwards, which this project avoids. Only do it if
you accept that disruption or have a spare WA104. The Sonoff Zigbee Bridge can only act as that
coordinator if its model supports Zigbee2MQTT/ZHA (the classic ZBBridge needs reflashing), so
identify it first.

### 4. Packet capture (passive)

See [Sniffer](#sniffer) below. Without the network key, a capture shows MAC/NWK headers, beacons
(stack profile, protocol version, router/end-device capacity, depth, extended PAN ID, update ID) and
unencrypted MAC commands such as Association Request/Response. It does not show ZDO/ZCL content,
which is inside the encrypted NWK payload. Use it to find out why a join failed and to prove
multi-hop relaying, not to read the WA104's clusters.

## From fingerprint to firmware profile

Only do this with a captured fingerprint, not a template.

```powershell
py tools\fingerprint_check.py fingerprints\wa104.captured.json --emit-cli
```

The tool refuses templates and incomplete files. It prints `profile set …` commands. Paste them into
the console, check `profile pending`, then run `profile save`, `reboot`, and re-commission
(`reset` → `join`) so the hub interviews the new identity.

The C6 then reports the captured manufacturer/model strings and manufacturer code, and exposes the
same cluster IDs with no behaviour behind them: a request for a proprietary attribute gets
"unsupported". The unique IEEE address is never copied. Presenting another product's identity is
your decision. The firmware ships with the smart plug identity because a generic identity is removed
by the Wiser hub; to use a different identity, override it in `menuconfig` or with `profile set`.

## Sniffer

### Separate firmware

`sniffer/` is a receive-only build. It runs in promiscuous mode and never transmits (no ACKs,
beacons or joins), so a board running it is not a router. Verified on the bench (2026-11-05): 11
frames in 20 s on channel 11, converted to a pcap that decodes as IEEE 802.15.4/Zigbee.

One C6 cannot be a router and a sniffer at the same time. The radio has one mode, and the SDK has no
verified way to mirror frames while routing. Use a second 802.15.4 board for live multi-hop proof.

### Steps

1. Flash: `py -m esptool --chip esp32c6 -p COM8 write-flash 0x0 release\wiserbridge-sniffer-0.1.0-xiao-esp32c6.bin`
2. Find the Wiser channel: from the router's `network` output while it was joined, or from the PAN
   that turned `OPEN` in a `scan` during pairing.
3. Capture to a file:
   ```powershell
   py tools\sniff_to_pcap.py --port COM8 --channel 11 --out captures\wiser.pcap
   ```
   or live into Wireshark. Run this from cmd.exe, because PowerShell 5 corrupts binary pipes:
   ```
   py tools\sniff_to_pcap.py --port COM8 --channel 11 --stdout | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
   ```
   `--linktype tap` adds channel/RSSI/LQI to each frame (IEEE 802.15.4 TAP).
4. In Wireshark the frames decode as *IEEE 802.15.4 (no FCS)* → *ZigBee Network Layer*. Useful
   display filters:
   * `wpan.frame_type == 0x0` beacons; `wpan.cmd == 0x01` association request;
     `wpan.cmd == 0x02` association response (check its status field)
   * `wpan.cmd == 0x04` data requests (sleepy children polling their parent; `wpan.dst16` is the
     parent)
   * `zbee_nwk.cmd.id == 0x08` link status; `zbee_nwk.cmd.id == 0x01/0x02` route request/reply
   * relayed traffic: `wpan.src16 != zbee_nwk.src` or `wpan.dst16 != zbee_nwk.dst`

### Commissioning events

A join normally shows: beacon request → beacons → association request → association response
(status) → (encrypted) Transport-Key from the trust centre via the parent → device announce →
(Zigbee 3.0) TCLK request/verify. A failed association has a non-success status. A successful
association with no Transport-Key afterwards means the trust centre refused the device.

### What stays encrypted

Everything above the NWK header: APS, ZDO and ZCL payloads are encrypted with the network key. This
project does not bypass Zigbee security. To decrypt, Wireshark needs the network key
(Preferences → Protocols → ZigBee → Security Keys). Legitimate ways to get it:

* If a device joins your own network using the well-known Zigbee 3.0 default link key (no install
  code), the Transport-Key carrying the network key is encrypted with that well-known key, and
  Wireshark can decrypt a join captured in full. Whether the Wiser hub uses that path or install
  codes is unknown.
* Vendor-provided diagnostic access.

Do not try to extract keys from the hub or WA104 hardware. The C6 holds the network key after
joining (the SDK has `esp_zb_secur_primary_network_key_get()`), but the default firmware never reads
or prints it. Exporting it is an owner's decision with obvious security implications.
