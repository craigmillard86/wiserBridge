# WA104 identity investigation and packet sniffing

**Only do this if generic commissioning fails or the hub expels the router (outcome B′/C).**
Do not start here: a plain router must be tried first ([commissioning.md](commissioning.md)).

Rules: the WA104 stays unmodified and paired to the hub; no fingerprint value is guessed; the WA104's
IEEE address, install code and any key are never copied into this project.

## What we want to learn

Manufacturer string, model identifier, node-descriptor manufacturer code, endpoint numbers, profile
IDs, device IDs, input/output clusters, Basic cluster attributes, proprietary (0xFC00–0xFFFF)
clusters, and node capabilities (logical type, MAC capability flags, server mask / stack compliance
revision). Template: [`fingerprints/wa104.template.json`](../fingerprints/wa104.template.json)
(schema: `fingerprints/fingerprint.schema.json`, enforced by `tools/wb_fingerprint.py`).

## Methods, ranked by intrusiveness

### 1. Interview over the air from the joined C6 (non-intrusive, preferred)

Works if the C6 has joined the Wiser network (outcome A or B). The WA104 answers standard ZDO and ZCL
requests from any network member; nothing is changed on it.

```
wb> neighbors                      # find the WA104's short address (a ZR)
wb> interview 0x1a2b               # its short address
...
FINGERPRINT-JSON {...}
```

Then on the PC (with the console log saved via `wb_monitor.py --log`):

```powershell
py tools\fingerprint_from_log.py logs\interview.log --label wa104 --out fingerprints\wa104.captured.json
py tools\fingerprint_check.py fingerprints\wa104.captured.json
```

The interview also reports the node descriptor's **stack compliance revision** — `interview 0x0000`
does the same for the hub. ≥21 means R21+/Zigbee 3.0; lower suggests a legacy trust centre (relevant
to the `tclk` setting).

Limits: manufacturer-specific attributes/commands are not discoverable this way; some devices answer
only part of the Basic cluster.

### 2. Public sources (non-intrusive)

Search, and record what you find with links in the fingerprint's `evidence` list (method
`public-database`):

* Zigbee2MQTT supported-devices list and `zigbee-herdsman-converters` (search "Drayton", "Wiser",
  "Schneider", "WA104").
* Zigbee device compatibility repositories (e.g. blakadder's), ZHA quirks, deCONZ device database.
* The Zigbee Alliance/CSA certified-product database (certificates sometimes list device type and
  stack).
* Zigbee OTA indexes (e.g. the Koenkk `zigbee-OTA` index): an OTA image header contains a
  manufacturer code and image type. Only use what is actually published there.

At the time of writing nothing in this project has been confirmed from these sources — treat every
entry as unknown until you have a reference.

### 3. Separate coordinator / Zigbee2MQTT (intrusive — last resort)

A Zigbee2MQTT (or ZHA) coordinator performs a full interview, stored in `data/database.db`:

```powershell
py tools\z2m_extract.py path\to\database.db --list
py tools\z2m_extract.py path\to\database.db --ieee 0x<wa104 ieee> --label wa104 --out fingerprints\wa104.captured.json
```

**But:** this needs the WA104 to *leave the Wiser network and join the other coordinator*, which
means factory-resetting it and later re-pairing it to Wiser — exactly what this project avoids.
Only consider it if you accept that disruption (or have a second, spare WA104). The Sonoff Zigbee
Bridge is only usable as that coordinator if its model supports Zigbee2MQTT/ZHA (the classic
ZBBridge needs reflashing) — identify it first.

### 4. Packet capture (passive)

See *Sniffer* below. Without the network key, a capture reveals MAC/NWK headers and beacons
(stack profile, protocol version, router/end-device capacity, depth, extended PAN ID, update ID) and
unencrypted MAC commands such as Association Request/Response — but **not** ZDO/ZCL content (that
is inside the encrypted NWK payload). It is the right tool for *why* a join failed and for
proving multi-hop relaying, not for reading the WA104's clusters.

## From fingerprint to firmware profile (only after real evidence)

```powershell
py tools\fingerprint_check.py fingerprints\wa104.captured.json --emit-cli
```

This refuses templates and incomplete files. It prints `profile set …` commands; paste them into the
console, check `profile pending`, then `profile save`, `reboot`, and re-commission (`reset` → `join`)
so the hub interviews the new identity. Understand what this does: the C6 then reports the captured
manufacturer/model strings and manufacturer code, and exposes the same cluster IDs **with no
behaviour behind them** (any request for a proprietary attribute gets "unsupported"). The unique IEEE
address is never copied. Whether presenting another product's identity is appropriate is your
decision; it is not done by default and the generic profile should always be tried first.

## Sniffer

### Separate firmware

`sniffer/` is a receive-only build: promiscuous mode, never transmits (no ACKs, beacons or joins).
While it is flashed the board is not a router. Verified on the bench (2026-11-05): 11 frames in 20 s
on channel 11, converted to a pcap that decodes as IEEE 802.15.4/Zigbee.

Simultaneous router + sniffer on one C6 is **not supported**: the radio can only be in one mode and
the SDK offers no verified way to mirror frames while operating as a router. Use a second
802.15.4-capable board for live multi-hop proof.

### Steps

1. Flash: `py -m esptool --chip esp32c6 -p COM8 write-flash 0x0 release\wiserbridge-sniffer-0.1.0-xiao-esp32c6.bin`
2. Find the Wiser channel: from the router's `network` output (when it was joined), or the channel of
   the PAN that turned `OPEN` during pairing in a `scan`.
3. Capture to a file:
   ```powershell
   py tools\sniff_to_pcap.py --port COM8 --channel 11 --out captures\wiser.pcap
   ```
   or live into Wireshark — from **cmd.exe** (PowerShell 5 corrupts binary pipes):
   ```
   py tools\sniff_to_pcap.py --port COM8 --channel 11 --stdout | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
   ```
   `--linktype tap` adds channel/RSSI/LQI to each frame (IEEE 802.15.4 TAP).
4. In Wireshark the frames decode as *IEEE 802.15.4 (no FCS)* → *ZigBee Network Layer*. Useful
   display filters:
   * `wpan.frame_type == 0x0` beacons; `wpan.cmd == 0x01` association request;
     `wpan.cmd == 0x02` association response (check its status field)
   * `wpan.cmd == 0x04` data requests (sleepy children polling their parent: the `wpan.dst16` is the
     parent)
   * `zbee_nwk.cmd.id == 0x08` link status; `zbee_nwk.cmd.id == 0x01/0x02` route request/reply
   * relayed traffic: `wpan.src16 != zbee_nwk.src` or `wpan.dst16 != zbee_nwk.dst`

### Identifying commissioning events

During a join you would expect: beacon request → beacons → association request → association
response (status) → (encrypted) Transport-Key from the trust centre via the parent → device announce →
(Zigbee 3.0) TCLK request/verify. A failed association shows a non-success status; a missing
Transport-Key after a successful association points at the trust centre refusing the device.

### What remains encrypted

Everything above the NWK header: APS, ZDO and ZCL payloads are encrypted with the network key.
**This project does not bypass Zigbee security.** Decryption requires the network key in
Wireshark (Preferences → Protocols → ZigBee → Security Keys). Legitimate ways to have it:

* If a device joins **your own** network using the standard well-known Zigbee 3.0 default link key
  (no install code), the Transport-Key carrying the network key is encrypted with that well-known
  key; Wireshark can decrypt a join captured in full. Whether the Wiser hub uses that path or install
  codes is unknown.
* Vendor-provided diagnostic access.

Do not attempt to extract keys from the hub or the WA104 hardware. The C6 holds the network key
after joining (the SDK has `esp_zb_secur_primary_network_key_get()`), but this firmware deliberately
never reads or prints it; adding such a feature is an owner's decision with obvious security
implications and is not part of this project.
