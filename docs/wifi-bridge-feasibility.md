# Wi-Fi bridge feasibility (not implemented)

Proposal: `TRV → C6 A → Wi-Fi → C6 B → Wiser Hub`, keeping the existing Wiser Zigbee network.

Conclusion: this cannot be built correctly with the available SDKs, and nothing is implemented. To
extend the network, add native Zigbee routers.

## 1. MAC acknowledgements

An 802.15.4 frame must be ACKed by the receiving radio within macAckWaitDuration: 54 symbols, about
864 µs on 2.4 GHz O-QPSK. Otherwise the sender retries (3 times by default) and reports a failure.
Wi-Fi round trips are 2-20 ms, with spikes over 100 ms. A MAC-level tunnel would have to ACK locally
for a device on the far side. The sender would then believe the next hop has the frame when it does
not, which breaks the retry and link-failure detection that Zigbee routing depends on.

## 2. Network-layer routing

Zigbee routes between NWK neighbours. Link costs come from link-status broadcasts (radius 1, every
15 s), and neighbours age out after missed link statuses. A tunnel would either be invisible to the
stack, or would have to appear as a virtual link between two real routers.

The correct design is for C6 A and C6 B to be real routers on the same network, with their NWK layers
exchanging frames over Wi-Fi as if they were radio neighbours. That needs hooks inside the NWK layer
to inject and extract frames, report link quality and take part in link status. esp-zigbee-sdk v1.x
ships ZBOSS as a closed library with no such hooks, and v2.x does not document any.

## 3. Parent and child

Sleepy TRVs poll their parent with MAC Data Requests and expect an immediate answer (frame-pending
bit in the ACK, then the buffered frame). The parent must be a local radio holding the child's
buffered frames, so C6 A must be a complete router. A tunnel cannot be the parent.

## 4. Frame counters and encryption

Zigbee PRO secures each hop: the transmitting router uses its own outgoing frame counter and address.
Receivers drop frames whose counter is not newer than the last one seen from that sender. Forwarding
raw secured frames into another radio neighbourhood looks like a replay, and frames delivered twice
(radio and tunnel) are rejected. A NWK-level virtual link would re-secure frames normally, but again
needs internal hooks. Bridging decrypted frames would expose the network key on the Wi-Fi path.

## 5. Wi-Fi latency

Route discovery and APS retries tolerate tens of milliseconds. Wi-Fi stalls of 100 ms or more
(jitter, access-point power-save or reboots) would look like link failures and trigger route repair
across the mesh.

## 6. Wi-Fi and 802.15.4 on the ESP32-C6

The ESP32-C6 has one 2.4 GHz RF front end shared by Wi-Fi and 802.15.4, time-shared by the
coexistence arbiter. While Wi-Fi is active, the 802.15.4 receiver is off part of the time, so the
router misses frames, beacons and child polls. Espressif's Zigbee gateway reference design uses two
chips for this reason (802.15.4 on an ESP32-H2 co-processor, Wi-Fi on a separate SoC). A router
serving sleepy TRVs needs its radio for 802.15.4 only, so the main firmware never starts Wi-Fi.

## 7. Security and reliability

The Wi-Fi leg needs mutual authentication and encryption, and makes two extra devices and the home
access point single points of failure for heating.

## Alternatives

1. Native routers along the path: this project's C6, more WA104 extenders, or other mains-powered
   Wiser devices that route.
2. Better placement of the existing WA104 or C6: mid-way, through fewer walls.
3. For a separate building: a separate Wiser system.

## If you try it anyway

Use a bench network, never the live heating network. You would need two ESP32-H2/C6 boards as real
routers, an open or source-available Zigbee stack with NWK-layer access, a virtual-link
implementation that takes part in link status, and sniffer checks on both sides.
