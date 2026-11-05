# Verification report — v0.1.0 (2026-11-05)

## Environment actually used

* Windows PC + WSL2; Docker Desktop with WSL integration; image `espressif/idf:v5.5.6`.
* esp-zigbee-lib **1.6.8**, esp-zboss-lib **1.6.4** (resolved by the component manager).
* Hardware on the bench: Seeed XIAO ESP32-C6 on **COM8** — esptool: *ESP32-C6FH4 (QFN32) rev v0.2,
  4 MB flash, USB-Serial/JTAG*, 802.15.4 MAC `98:a3:16:ff:fe:bb:7a:38`.
* Flashing/serial from WSL via Windows Python 3.12 + esptool 5.1.0 + pyserial.

## Implemented

* Router firmware (`main/`): ZR role only; generic HA Range Extender endpoint (Basic + Identify,
  configurable identity/clusters/endpoint/device ID/manufacturer code/child limit via Kconfig and
  NVS-persisted `profile`); explicit-only joining with steering window, EPID filter, channel mask,
  TCLK option; refusal to steer while joined; no formation path; persistent membership
  (`zb_storage`); confirmed network reset (BDB local reset when joined, NVRAM erase when not);
  BOOT button join/reset; status LED; XIAO RF-switch/antenna control; isolation watchdog.
* Diagnostics: boot banner (firmware, chip, reset reason, IDF, zigbee-lib, role, endpoint,
  identity, child limit, channels, EPID filter, TCLK, TX power, antenna); all ZDO/BDB/NWK signals
  handled and logged; event log; counters; join-outcome classifier; neighbour/route/route-record/
  child tables; `scan`, `ping`, remote `lqi`, `interview` (fingerprint JSON), `multihop`; periodic
  `HEALTH` line. Keys are never read.
* Serial CLI: `status network neighbors routes children diagnostics events multihop health join scan
  ping lqi interview profile epid channels tclk reset reboot version log help`.
* Separate passive sniffer firmware (`sniffer/`) + `tools/sniff_to_pcap.py` (pcap, NOFCS or TAP).
* Tools: `build.sh`, `wb_monitor.py`, `wb_health_report.py`, `fingerprint_from_log.py`,
  `fingerprint_check.py`, `z2m_extract.py`, `wb_fingerprint.py`.
* Fingerprint template (all unknown) + schema. Documentation set in `docs/`.

## Compiled successfully

| Target | Result |
|---|---|
| Router firmware, esp32c6, IDF v5.5.6 | **0 errors, 0 warnings**; app 0xa4910 bytes (66 % of the 1.875 MB partition free) |
| Sniffer firmware, esp32c6, IDF v5.5.6 | **0 errors, 0 warnings**; app 0x39c00 bytes |
| Release images | `release/wiserbridge-{router,sniffer}-0.1.0-xiao-esp32c6.bin` + `SHA256SUMS.txt` |
| Host C unit tests (gcc, `-Wall -Wextra -Werror`) | builds |

SDK API usage was checked against the actual esp-zigbee-lib 1.6.8 headers; `_Static_assert`s pin the
enum values mirrored in the host-testable code.

## Tested automatically (no hardware) — `tests/run_all.sh`

* `main/diag_logic.c`: **75/75** checks — address parsing/formatting, channel-mask rendering,
  status strings, and every branch of the join/routing verdict classifier (including "relay route
  alone is not proof").
* Python tools: **11/11** tests — fingerprint validation (templates refused, IEEE/keys rejected,
  evidence required), interview-log → fingerprint → CLI round trip, Zigbee2MQTT database
  conversion, pcap writer (global/record headers, timestamps, TAP TLV layout), health-log parser.

## Tested on the bench (real XIAO ESP32-C6, no Wiser involvement)

| Check | Result |
|---|---|
| Flash + boot | OK; banner complete and correct; `START: factory-new, not commissioned` |
| No accidental network activity | No auto-join; never formed a network |
| `status`/`network`/`diagnostics`/`children`/`neighbors`/`routes`/`multihop`/`health`/`epid`/`tclk`/`channels` | Correct output when not joined |
| `scan` | OK — 4 PANs seen (ch 11/16/20/24, all permit-join closed); summary feeds the verdict |
| `join` (no network open) | Steering ran, failed with BDB `NO_NETWORK`, retry scheduled — failure path works. The board was rebooted to close the window |
| `ping`/`lqi`/`interview` when not joined | Refused with reason (no fake data) |
| `profile set` validation | Rejects endpoint 0 and duplicate Basic cluster; accepts valid edits |
| `reset` safeguard | Wrong code refused; correct one-time code → NVRAM erased → restart factory-new |
| Production-config signal | Explained in log ("normal for this firmware") |
| Sniffer firmware | 11 frames captured in 20 s on ch 11; pcap decodes (MAC headers; NWK headers readable, payloads encrypted) |
| Serial monitor attaches without resetting the board | Fixed and verified |

## Requires the real Wiser system (not yet done)

M2 commissioning with the hub · M3 persistence while joined (restore from NVRAM, local reset with
Leave) · M4 multi-hop through the WA104 (`multihop`, `ping 0x0000`, `lqi`) · M5 a TRV choosing the C6
as parent · M6 hub/WA104/C6 restarts, route loss, 24 h and 7-day soaks · `interview` of the WA104/hub ·
identify/LED behaviour when the hub sends Identify.

## Unverified Wiser assumptions

1. The Wiser hub permits a non-Drayton Zigbee 3.0 router to join (not known).
2. The hub keeps an unrecognised router in the network rather than sending Leave (not known).
3. The hub's trust centre is R21+ and completes TCLK exchange with this stack (not known; `tclk off`
   exists as a fallback, and `interview 0x0000` will report the hub's stack compliance revision).
4. Wiser TRVs will accept a non-Drayton router as parent when they rejoin (not known).
5. Joining does not require install codes or an allow-list (not known).
6. Which of the four observed PANs is the Wiser network (not known — identify by pairing-mode scan).
7. No WA104 identity values are known; the fingerprint template is entirely null.

## Next physical test (do this first)

**M2 — generic commissioning attempt**, following [commissioning.md](commissioning.md):

1. Power the C6 near the hub or WA104, attach `py tools\wb_monitor.py --port COM8 --duration 0 --log logs\m2.log`.
2. `scan` (baseline) → put the hub into pairing mode in the Wiser app → `scan` again → note the PAN
   that turned `OPEN` → `epid <its EPID>`.
3. `join`, wait for the result (≤ 3 min), then close pairing in the app.
4. `status`, `diagnostics`, `network`, `events`; check the app for a new device; wait 10 minutes and
   run `status` again (detects outcome B′).
5. If joined: `interview 0x0000` (hub stack revision) and `neighbors` (is the WA104 visible?).
6. Keep `logs\m2.log`. That log answers the project's central question.
