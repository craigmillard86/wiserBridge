# Wi-Fi extension feasibility (research branch — not implemented)

Proposal: `TRV → C6 A → Wi-Fi → C6 B → Wiser Hub`, preserving the original Wiser Zigbee network.

**Conclusion: not technically justified with the available SDKs. Nothing is implemented or
advertised.** The reliable way to extend the network is more native Zigbee routers. Reasons below.

## 1. IEEE 802.15.4 MAC acknowledgements

An acknowledged 802.15.4 frame must be ACKed by the *receiving radio* within macAckWaitDuration —
54 symbols ≈ **864 µs** on 2.4 GHz O-QPSK — otherwise the sender retries (default 3 times) and
reports a failure upward. Wi-Fi round trips are typically 2–20 ms with spikes over 100 ms. A
transparent MAC-level tunnel would have to ACK locally on behalf of a device on the far side, which
makes the ACK meaningless (the sender believes the next hop has the frame when it has not), breaking
retry and link-failure detection that Zigbee routing relies on.

## 2. Zigbee network-layer routing

Zigbee routes between NWK neighbours connected by radio links. Link costs come from link-status
broadcasts (radius 1, every 15 s) and neighbour entries age out after missed link statuses. A tunnel
would either be invisible (then the stack has no idea the far side exists) or must appear as a
**virtual link** between two genuine routers. The correct design is therefore: C6 A and C6 B are
both real routers on the same network, and their NWK layers exchange frames over Wi-Fi as if they
were radio neighbours, with a link cost for that link. That requires hooks inside the NWK layer
(inject/extract frames, report link quality, take part in link status). **esp-zigbee-sdk v1.x
ships ZBOSS as a closed library and exposes no such hooks**; v2.x does not document them either.
Without them, this cannot be built correctly.

## 3. Parent/child relationships

Sleepy TRVs poll their parent with MAC Data Requests and expect the parent to answer immediately
(frame-pending bit in the ACK, then the buffered frame). The parent must therefore be a local radio
with the child's buffered frames — C6 A must be a complete router itself. A tunnel cannot be the
parent.

## 4. Frame counters and encryption

Zigbee PRO applies NWK security per hop: each transmitting router secures the frame with its own
outgoing frame counter and address. Receivers drop frames whose counter is not newer than the last
seen from that sender. Forwarding raw secured frames into a different radio neighbourhood looks like
replay, and duplicated delivery (radio + tunnel) is rejected. A NWK-level virtual link (section 2)
would re-secure frames normally — again needing internal hooks. Bridging decrypted frames would
expose the network key on the Wi-Fi path.

## 5. Wi-Fi latency

Even with a NWK-level virtual link, Wi-Fi latency and jitter (and access-point power-save) are
far above radio-link behaviour. Route discovery and APS retries can tolerate tens of milliseconds,
but bursts of 100 ms+ stalls or AP reboots would look like link failures and trigger route repair
across the mesh.

## 6. ESP32-C6 Wi-Fi / 802.15.4 coexistence

The ESP32-C6 has one 2.4 GHz RF front end shared by Wi-Fi and 802.15.4, arbitrated by time-division
coexistence. While Wi-Fi is active the 802.15.4 receiver is unavailable part of the time, so a
router misses frames, beacons and children's polls. Espressif's own Zigbee gateway reference design
uses two chips (802.15.4 on an ESP32-H2 radio co-processor, Wi-Fi on a separate SoC) for this reason.
A router serving sleepy TRVs should keep its radio dedicated to 802.15.4 — which is why the main
firmware never starts Wi-Fi.

## 7. Security and reliability

The Wi-Fi leg would need mutual authentication and encryption, adds two devices and the home access
point as single points of failure, and puts heating control behind the Wi-Fi network's health.

## Practical alternatives

1. Native routers along the path: this project's C6 (once Wiser acceptance is verified), more WA104
   extenders, or other Wiser mains devices that route.
2. Better placement of the existing WA104 / C6 (mid-way, line of sight through fewer walls).
3. For a separate building: a separate Wiser system rather than tunnelling one mesh.

## If someone pursues it anyway

Treat it as research on a **bench network**, never the live heating network: two ESP32-H2/C6 boards
as real routers, an open or source-available Zigbee stack with NWK-layer access, a virtual-link
implementation with link-status participation, and sniffer-verified behaviour on both sides.
