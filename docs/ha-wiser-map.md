# Wiser Zigbee topology card for Home Assistant

Home Assistant cannot auto-draw this network because the Drayton Wiser hub is its own closed Zigbee
coordinator (not ZHA/Zigbee2MQTT). This card renders the topology **observed by the WiserBridge C6**
(`neighbors`, `children`, and the plug's `lqi` table, 2026-11-05). It is a static snapshot — re-run
those commands and edit the diagram to refresh it.

Add a **Manual** card (or Markdown card) to a dashboard and paste this YAML:

```yaml
type: markdown
content: |
  ## Wiser Zigbee network (observed)
  ```mermaid
  graph TD
    HUB["Heat Hub 0x0000<br/>coordinator"]:::coord
    PLUG["Smart Plug 0x7798<br/>router (Heiman)"]:::router
    C6["WiserBridge C6 0x2c3c<br/>router (our device)"]:::router

    HUB --- PLUG
    C6 -. "route to hub via plug" .-> PLUG
    PLUG --- HUB

    PLUG --> E1["TRV/sensor 0x38e4"]:::ed
    PLUG --> E2["TRV/sensor 0x4988"]:::ed
    PLUG --> E3["TRV/sensor 0x6922"]:::ed
    PLUG --> E4["TRV/sensor 0xe230"]:::ed
    PLUG --> E5["TRV/sensor 0x917f"]:::ed
    PLUG --> E6["TRV/sensor 0x7183"]:::ed
    PLUG --> E7["TRV/sensor 0xcf2b"]:::ed
    PLUG --> E8["TRV/sensor 0x48d3"]:::ed
    PLUG --> E9["TRV/sensor 0x9e36"]:::ed
    PLUG --> E10["TRV/sensor 0x72e8"]:::ed
    PLUG --> E11["TRV/sensor 0x14f1"]:::ed

    classDef coord fill:#b3261e,color:#fff,stroke:#7f1d1a;
    classDef router fill:#1b5e9b,color:#fff,stroke:#0d3d6b;
    classDef ed fill:#2e7d32,color:#fff,stroke:#1b5e20;
  ```
  _Solid = parent/child link. Dashed = the C6's current route to the hub (relayed via the plug).
  The C6 has 0 children: no end device uses it as parent yet._
```

## Notes

* Short addresses (0x….) can change if a device rejoins; the stable id is the IEEE (not shown here on
  purpose). To label a node with a real device, match its IEEE from the C6's `interview`/`lqi` output
  against the Wiser app.
* All 11 end devices are children of the **plug** (0x7798), which is itself a child of the hub. The
  **C6** is a working router (relays toward the hub) but nothing is parented to it, so it carries no
  TRV traffic today.
* To make this live rather than static you would need the network on ZHA/Zigbee2MQTT, which would mean
  moving devices off the Wiser hub — out of scope for this project.
