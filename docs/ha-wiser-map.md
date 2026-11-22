# Wiser topology card for Home Assistant

Home Assistant cannot draw this network itself, because the Wiser hub is a closed Zigbee coordinator
(not ZHA or Zigbee2MQTT). This card draws the topology the C6 observed on 2026-11-05 from
`neighbors`, `children` and the plug's `lqi` table. It is a static snapshot. To refresh it, re-run
those commands and edit the diagram.

> The topology has changed since this snapshot: there are now two C6 routers, and four TRVs use C6
> 0x2c3c as their parent. See [test-results.md](test-results.md).

Add a Manual (or Markdown) card to a dashboard and paste this YAML:

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

* Short addresses change when a device rejoins. The IEEE address is the stable ID and is left out
  of the diagram. To label a node, match its IEEE from the C6's `interview` or `lqi` output against
  the Wiser app.
* At snapshot time, all 11 end devices were children of the plug (0x7798), which is a child of the
  hub. The C6 relayed toward the hub but had no children, so it carried no TRV traffic.
* A live diagram would need the network on ZHA or Zigbee2MQTT, which means moving devices off the
  Wiser hub. That is out of scope.
