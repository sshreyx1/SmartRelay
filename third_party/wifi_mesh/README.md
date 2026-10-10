# WiFi Mesh — how to use it (CSC09 SmartRelay)

Owner: Goutham. This folder is the WiFi half of the radio layer. It implements
Mridula's `mesh_radio.h`, so your fragmentation code can call WiFi and LoRa the
same way.

```
 Your fragmentation code
        │  wifi_mesh.send / recv / poll / tx_busy / last_heard ...
 wifi_mesh.c      flooding mesh: duplicates, hop limit, relaying, heartbeat
        │
 wst_link.c       Pico <-> WST over UART0 (GP16/GP17), framed + CRC
        │
 WST (ESP-01S)    custom ESP-NOW firmware (wst_bridge.ino) — already flashed on all 5
```

---

## 1. Files

| File | What it is | You need it? |
|---|---|---|
| `mesh_radio.h` | Shared radio interface (Mridula) | **Yes** |
| `wifi_mesh.h` / `wifi_mesh.c` | The WiFi mesh. Exposes `wifi_mesh` (a `mesh_radio_t`) | **Yes** |
| `wifi_mesh_config.h` | WST MAC table, demo neighbour layout, timings | **Yes** |
| `wst_link.h` / `wst_link.c` | UART driver for the WST | **Yes** |
| `bridge.c` | Node A program: mesh ↔ laptop over USB | Yes, if you build the node A firmware |
| `bridge_client.py` | Laptop side of the bridge; `import` it in the backend | Yes, for the backend |
| `CMakeLists.txt`, `pico_sdk_import.cmake` | Build files (examples of how to link everything) | Reference |
| `mesh_test.c`, `hello_test.c` | My test programs | No |
| `mesh_sniffer.py`, `wst_monitor.py` | My laptop debug tools | Optional |
| `test/sim_mesh.c` | 5-node PC simulation of the mesh | No |
| `*.uf2` | Pre-built test/bridge programs | No |

---

## 2. Adding it to your build

```cmake
add_executable(your_node
    your_main.c
    your_fragmentation.c
    wifi_mesh.c
    wst_link.c
)
target_link_libraries(your_node pico_stdlib pico_rand hardware_uart hardware_irq)
pico_enable_stdio_usb(your_node 1)
pico_enable_stdio_uart(your_node 0)      # UART0 belongs to the WST
```

Set `set(PICO_BOARD pico_w)` **before** `include(pico_sdk_import.cmake)`.

**Pins used:** UART0 on **GP16 (TX) / GP17 (RX)** — the Maker Pi Pico ESP-01 socket. Don't use these for anything else.

---

## 3. Using it in code

```c
#include "wifi_mesh.h"

int main(void) {
    stdio_init_all();
    wifi_mesh_init(0);          // 0 = work out node ID from the WST's MAC (WST-1 = A ... WST-5 = E)
                                // or pass 1-5 to force it. Returns false if no WST / unknown MAC.
    const mesh_radio_t *radio = &wifi_mesh;

    while (true) {
        radio->poll();          // call EVERY loop, every few ms — this is what actually sends/receives

        // send (only queues; never blocks)
        if (!radio->tx_busy())
            radio->send(1, frag, frag_len);     // to node A (the bridge)

        // receive
        uint8_t src, buf[245];
        int n = radio->recv(&src, buf, sizeof buf);
        if (n >= 0) { /* packet of n bytes from node src */ }
    }
}
```

### Rules and numbers

| Thing | Value / behaviour |
|---|---|
| Max payload per `send()` | **245 bytes** (`radio->max_payload()`) |
| Node IDs | 1–5 = A–E. `MESH_BROADCAST` (255) = everyone. Node A (1) is the bridge |
| Delivery | **Best effort.** No ACKs, no retries, no ordering. Your layer does retransmission |
| Corrupt packets | Dropped, never delivered |
| `send()` returns false | Queue full (8), length 0 or >245, bad dst, or dst = yourself |
| `recv()` | Returns length ≥ 0, `MESH_RECV_NONE` (-1), or `MESH_RECV_BUF_TOO_SMALL` (-2, packet discarded) |
| `recv()` `src` | The node that **created** the packet, not the last relay |
| Pacing | Send the next fragment only when `!radio->tx_busy()`. That gives the best throughput without overflowing queues |
| `last_heard(1, &t)` | Time (ms since boot) node A was last heard. Node A sends a heartbeat every 3 s, so if `now - t > ~6000`, WiFi can't reach the bridge → use LoRa for this transfer |
| `set_drop_rate(0.2f)` | Demo only: this node ignores 20% of what it receives |
| Don't block | Anything that blocks for long (big SD reads, `sleep_ms`) delays `poll()` and the mesh stalls. Keep each loop iteration short |

### Over-the-air header (added automatically, 5 bytes)

`[src][dst][pkt_id lo][pkt_id hi][hops]` + your payload. Same layout on LoRa.
Each packet floods across the mesh: duplicates are dropped (30 s window), hop
limit 4, relays wait a random 5–30 ms.

### Throughput (5-node simulation, 2 hops, 240-byte packets)

About **45 packets/s ≈ 11 KB/s**. A 20 KB image ≈ 85 fragments ≈ 2 s on WiFi
(before retransmissions). 20% random packet loss still gave 96% delivery
thanks to flooding's multiple paths.

---

## 4. Demo layout (topology filter)

In a room every node hears every other node, so there'd never be a hop. The
mesh therefore ignores packets from non-neighbours (by WST MAC) to recreate the
proposal layout:

```
   B ─── C
   │ ╲ ╱ │ ╲
   │  ╳  │  E        A: B C D     B: A C D     C: A B D E
   │ ╱ ╲ │ ╱         D: A B C E   E: C D
   A ─── D
```

E reaches A in 2 hops (via C or D). Turn it off with
`wifi_mesh_set_topology_filter(false)`. Change it in `wifi_mesh_config.h`.

**WST ↔ node:** WST-1 = A, WST-2 = B, WST-3 = C, WST-4 = D, WST-5 = E.
The node with WST-1 must be the bridge.

---

## 5. The bridge (node A ↔ laptop)

`bridge.c` runs on node A. It forwards every packet delivered to A to the
laptop, and lets the laptop send packets into the mesh. It talks to radios
through `mesh_radio_t`, so adding LoRa is 3 marked lines in `bridge.c`.

**USB text protocol** (one line each; data as hex):

| Direction | Line | Meaning |
|---|---|---|
| Pico → laptop | `RX <r> <src> <hex>` | packet from node `src` on radio `r` (`w` / `l`) |
| | `OK <r>` / `ERR <r> <reason>` | reply to a TX line (`busy`, `badlength`, ...) |
| | `ST <r> tx=.. rx=.. ... seen=B:3,C:-,D:12,E:-` | every 5 s; `seen` = seconds since heard, `-` = never |
| | `# text` | log, ignore |
| laptop → Pico | `TX <r> <dst> <hex>` | send into the mesh (dst 1–5 or 255) |
| | `DROP <r> <percent>` | demo packet loss on node A |
| | `STATS` | print status lines now |

**From Python (backend):**

```python
from bridge_client import Bridge

br = Bridge("/dev/cu.usbmodem14401")         # node A's USB port
for ev in br.events():                        # yields None when idle
    if ev and ev["type"] == "rx":
        handle(ev["radio"], ev["src"], ev["data"])   # data = bytes (one fragment)
    elif ev and ev["type"] == "stats":
        update_dashboard(ev["seen"])                 # {'B': 3, 'C': None, ...}
br.send("w", 3, b"resend 3,7,9")              # e.g. a NACK back to node C
```

Note: the bridge only outputs while a program has the port open (DTR), so
nothing is printed into the void and the mesh never stalls on USB.

---

## 6. Tested so far

- All 5 WSTs flashed and tested pairwise over ESP-NOW (0 errors).
- Mesh on hardware (1 Pico + laptop playing other nodes): delivery, broadcast,
  relaying, heartbeat, bridge-loss detection, drop-rate — all working.
- Bridge both ways (mesh → laptop, laptop → mesh) — working.
- Not yet: real 3–5 Pico multi-hop (needs everyone's boards together).

Questions → Goutham.
