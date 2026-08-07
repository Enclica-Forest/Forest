# Forest OS - Networking Stack

Welcome to the networking chapter of Forest OS! This page walks through the entire
TCP/IP stack — from the wire all the way up to the socket API that userspace apps
use.

---

## Layered Architecture

```
┌─────────────────────────────────────────────────┐
│               Userspace Applications            │
├─────────────────────────────────────────────────┤
│  socket / bind / connect / send / recv  (libc)  │
├─────────────────────────────────────────────────┤
│           System Call Interface (int 0x80)       │
├─────────────────────────────────────────────────┤
│  Socket Layer  ──  TCP  ──  UDP  ──  ICMP       │
├─────────────────────────────────────────────────┤
│              Internet Layer  (IPv4)              │
├─────────────────────────────────────────────────┤
│         ARP  ──  Routing Table                   │
├─────────────────────────────────────────────────┤
│           Network Device Layer (netdev)          │
├─────────────────────────────────────────────────┤
│   e1000  │  RTL8139  │  NE2000  │  virtio-net   │
├─────────────────────────────────────────────────┤
│              Physical NIC Hardware               │
└─────────────────────────────────────────────────┘
```

Packets flow **down** on transmit and **up** on receive.

---

## 1. Network Device Drivers

All drivers live under `fern/src/networking/` and share a common device
abstraction in `netdev.h`. Each driver fills in a `netdev_t` struct with
function pointers for `init`, `send`, `irq_handler`, and `get_mac`.

The kernel scans the PCI bus at boot. When a known vendor/device ID is found,
the matching probe function fills in callbacks and the device is registered.
The first successfully initialized device becomes the **active NIC** — all
outbound traffic goes through it.

| Driver | PCI IDs | Bus Type | Notes |
|--------|---------|----------|-------|
| **e1000** | 8086:100E | MMIO | Intel Gigabit. EEPROM-based MAC, 32 RX / 8 TX descriptors. Great for QEMU. |
| **RTL8139** | 10EC:8139 | IO Port | Realtek Fast Ethernet. 4 TX descriptors, 8KB RX ring buffer. |
| **NE2000** | 10EC:8029 | IO Port | Novell/Realtek. Simple DMA ring, 64-byte buffers. Legacy but reliable. |
| **virtio-net** | VirtIO MMIO | MMIO | AArch64/RISC-V QEMU virt. Split virtqueues, FDT-based discovery. |

The **e1000** driver handles device reset, EEPROM-based MAC reading, RX/TX
descriptor ring setup, and interrupt enablement. The **virtio-net** driver is
designed for non-x86 platforms and discovers the device through the FDT
(Flattened Device Tree).

### Packet Flow

**Transmit:** Higher layer → `network_send_packet()` → active device `send()`
→ driver copies frame into TX descriptor → notifies hardware.

**Receive:** Hardware IRQ → `netdev_handle_irq()` → driver `irq_handler()` →
reads RX ring → `network_receive_packet()` → Ethernet dispatch.

---

## 2. Network Device Management

The `netdev` layer (`netdev.c`) manages a **singly-linked list** of all
registered network devices.

### Key Operations

- **`netdev_register()`** — adds a device to the global list
- **`netdev_unregister()`** — removes it
- **`netdev_find_by_name(name)`** — lookup by interface name (e.g., "e1000")
- **`netdev_find_by_type(type)`** — lookup by driver type enum
- **`netdev_list_all()`** — debug: prints all registered devices

When the first device is registered, it becomes the **active device**. Its
MAC address is propagated to the network layer via `network_set_mac_address()`,
and its send function is registered as the global send callback.

Each device tracks per-interface statistics in a `net_stats_t` struct
(rx/tx packets, bytes, errors, drops). These are queryable via
`net_get_if_stats()` and also accessible through the `netinfo` syscall.

---

## 3. Packet Buffers

Forest OS uses **contiguous stack-allocated buffers** rather than skbuffs.
Key constants from `network.h`:

| Constant | Value | Purpose |
|----------|-------|---------|
| `NETWORK_MAX_PACKET_SIZE` | 1518 | Max Ethernet frame |
| `ETH_MTU` | 1500 | Maximum Transmission Unit |
| `ETH_HLEN` | 14 | Ethernet header size |

Protocol headers are `__attribute__((packed))` structs cast over the buffer:
`eth_header_t` (14B), `ip_header_t` (20B), `tcp_header_t` (20B),
`udp_header_t` (8B), `icmp_packet_t` (8B).

---

## 4. ARP Protocol

ARP (Address Resolution Protocol) maps IP addresses to MAC addresses on the
local network. Forest OS maintains a **32-entry ARP cache**.

### Cache Structure

Each entry is an `arp_entry_t` containing the IP (4 bytes), MAC (6 bytes),
and a valid flag. The cache is a fixed-size array — no dynamic allocation.

### Operations

- **`arp_resolve(ip, mac)`** — cache lookup. Called by IP before every send.
  Returns 1 if found, 0 if the MAC is unknown.
- **`arp_send_request(ip)`** — broadcasts an ARP request to FF:FF:FF:FF:FF:FF
  asking "who has this IP?"
- **`arp_send_reply(ip, mac)`** — sends a unicast reply to a specific MAC.
- **`arp_handle_packet()`** — processes incoming ARP:
  - **Request:** If someone asks for our IP, we learn their MAC, cache it,
    and send a reply.
  - **Reply:** We learn the sender's MAC and add it to the cache.

### Cache Eviction

When the cache is full, the first invalid slot is used. If all slots are
valid, the first slot is overwritten (simple FIFO replacement). There is no
timer-based expiry yet — entries persist until replaced.

---

## 5. Internet Layer (IPv4)

**Receiving:** `ip_handle_packet()` validates the packet (IPv4, checksum,
destination match), extracts the payload, and dispatches by protocol number
via a 256-entry handler table:
- Protocol 1 → ICMP, Protocol 6 → TCP, Protocol 17 → UDP

**Sending:** `ip_send_packet()` looks up the routing table for the next hop,
resolves the destination MAC via ARP (deferring if unknown), builds the full
Ethernet + IP frame, and sends it.

IP checksum uses standard ones-complement. Pseudo-header checksum for TCP/UDP
is in `ip_pseudo_checksum()`.

---

## 6. ICMP (Ping)

ICMP handles the "are you there?" protocol. Forest OS supports:

- **Echo Request (type 8):** Sent via `icmp_send_echo_request()`. Includes
  an ID, sequence number, and optional payload. Checksum covers the full
  ICMP packet.

- **Echo Reply (type 0):** When we receive an echo request, we automatically
  build a reply with the same ID, seq, and payload — that's what makes
  `ping` work.

- **Callback:** `icmp_set_echo_callback()` registers a function to be called
  when an echo reply arrives, receiving the source IP, sequence, and data.

The ICMP type constants `ICMP_DEST_UNREACHABLE` (3) and `ICMP_TIME_EXCEEDED`
(11) are defined for future use.

---

## 7. UDP

UDP is the simple, connectionless transport protocol. It's used internally
by DHCP and DNS, and available for userspace apps too.

The layer maintains **32 UDP sockets**. Each socket has a port number, a
receive callback, and a bound flag.

- **`udp_bind(port, callback)`** — binds to a port. When a UDP packet
  arrives for that port, the callback fires with source IP, port, data,
  and length.
- **`udp_send(dst_ip, dst_port, data, len)`** — sends a datagram. Builds
  the UDP header (ports, length, pseudo-header checksum) and passes to
  `ip_send_packet()`.
- **`udp_sendto()`** — full version with explicit source IP and port.

Reception: `udp_handle_packet()` scans the socket array for a matching port
and invokes the callback.

---

## 8. TCP

Supports up to **16 simultaneous connections** with full state machine
(CLOSED, LISTEN, SYN_SENT, SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1/2,
CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT).

### Connection Establishment

**Active (client):** `tcp_connect()` allocates a connection slot, generates
an initial sequence number, and sends a SYN packet. When SYN+ACK arrives,
the state moves to ESTABLISHED and the connect callback fires.

**Passive (server):** `tcp_listen()` creates a listen socket. When SYN
arrives, `tcp_accept()` allocates a new connection in SYN_RECEIVED state
and sends SYN+ACK. When ACK arrives, the connection is ESTABLISHED.

### Data Transfer

`tcp_send()` builds PSH+ACK segments with the data payload. The checksum
uses the pseudo-header (IP src/dst, protocol number, TCP length) computed
in `ip_pseudo_checksum()`. Sequence numbers advance by the data length.
Window size is advertised at 8192 bytes.

### Connection Teardown

`tcp_close()` sends FIN+ACK and transitions to FIN_WAIT_1. When the remote
FIN arrives, an ACK is sent and the connection closes. Callbacks are
invoked for cleanup.

### Reliability

Retry counter (max 3) and timeout (5s) are defined but the full
retransmission logic is a TODO. Sequence numbers and ACKs provide the
foundation for future reliability enhancements.

---

## 9. DHCP Client

DHCP automatically configures the network interface with an IP address,
gateway, netmask, and DNS server.

### The DHCP Exchange

1. **DISCOVER** — `dhcp_discover()` broadcasts a DHCP Discover packet. Built
   as a raw UDP packet (bypassing normal IP send) because the interface has
   no IP yet. Includes the DHCP magic cookie and message type option.

2. **OFFER** — When a server responds with an Offer, the client stores the
   offered IP and sends a Request.

3. **REQUEST** — `dhcp_request()` formally requests the offered IP, including
   the requested IP option.

4. **ACK** — `dhcp_handle_ack()` parses DHCP options to extract:
   - Subnet mask (option 1)
   - Router/gateway (option 3)
   - DNS server (option 6)

   These are applied globally via `network_set_ip_address()`,
   `network_set_gateway()`, `network_set_subnet_mask()`, and
   `network_set_dns_server()`.

A callback via `dhcp_set_callback()` notifies when configuration completes,
receiving the success status, IP, gateway, netmask, and DNS server.

---

## 10. DNS Resolver

DNS translates human-readable domain names into IP addresses over UDP
port 53.

### How It Works

1. **`dns_query(name, dns_server, callback)`** — initiates a lookup. Only
   one query can be in flight at a time.

2. The query is sent over UDP. The DNS packet includes a header with a
   unique query ID, recursion desired flag, and 1 question. The query name
   is encoded in DNS wire format: `dns_encode_name()` converts
   "www.example.com" into `\x03www\x07example\x03com\x00`.

3. **Response parsing:** `dns_handle_response()` skips questions, handles
   name compression (pointer offsets with 0xC0 marker), and extracts A-record
   IP addresses from answer RRs.

4. The callback receives the resolved IP addresses.

### Limitations

Only A-record (IPv4) queries are supported. No caching, no retry/timeout,
and only one concurrent query. These are on the roadmap for future work.

---

## 11. Routing

The routing table determines where packets go. It's a fixed-size table of
**32 entries**. Each `net_route_t` has a destination network, subnet mask,
gateway (next hop), interface name, and metric.

### Default Routes

At boot, two routes are initialized:
- **Loopback:** 127.0.0.0/8 via "lo"
- **Default:** 0.0.0.0/0 via "lo" (updated when a NIC comes up)

### Route Lookup

`route_lookup(dest_ip)` performs **longest prefix match**: it scans all
entries, checks `(dest_ip & mask) == (route_dest & mask)`, and selects the
entry with the most specific (longest) mask.

### Management

- `route_add(dest, mask, gateway, ifname, metric)` — adds or updates a route
- `route_del(dest, mask)` — removes a route
- `route_dump(out, max)` — dumps the table for userspace inspection

These are also exposed via the `net_route_*` wrappers in `net.h`.

---

## 12. Socket API

### Libc (userspace)

Standard POSIX functions defined in `libs/libc/src/syscalls.c`:
`socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`,
`recv()`, `sendto()`, `recvfrom()`, `close()`.

Each triggers `int 0x80` syscall to the kernel. The libc layer handles
architecture differences (32-bit vs 64-bit register conventions) and
translates kernel error codes to errno.

### Kernel Side

The kernel implements: `net_socket_create()`, `net_bind()`, `net_connect()`,
`net_send()`, `net_recv()`, `net_ioctl()`. Supports SOCK_STREAM (TCP),
SOCK_DGRAM (UDP), and SOCK_RAW (future).

Socket options (`SO_REUSEADDR`, `SO_KEEPALIVE`, `TCP_NODELAY`, etc.) are
defined in `net.h` but not all are implemented yet.

### Address Structures

Standard `sockaddr_in_t` with `sin_family` (AF_INET), `sin_port` (network
byte order), and `sin_addr`. Byte order helpers: `htons()`, `ntohs()`,
`htonl()`, `ntohl()`.

### Interface Configuration

`net_ioctl()` supports: `SIOCGIFADDR` (get IP), `SIOCGIFCONF` (list
interfaces), `SIOCGIFHWADDR` (get MAC), `SIOCGIFFLAGS` (get flags),
`SIOCADDRT` / `SIOCDELRT` (route management).

---

## 13. Initialization

```
network_init()
  ├── netdev_init()     # PCI scan, probe drivers, register active NIC
  ├── route_init()      # Loopback + default routes
  ├── arp_init()        # Clear ARP cache
  ├── ip_init()         # Register ICMP/TCP/UDP handlers
  ├── tcp_init()        # Clear connection table
  ├── udp_init()        # Clear socket table
  ├── dhcp_init()       # Reset DHCP state
  └── dns_init()        # Reset DNS state
```

---

## 14. Build System

`fern/build/features/networking.mk` provides per-protocol/per-driver toggles:

`ENABLE_NETWORKING` (master), `ENABLE_TCP`, `ENABLE_UDP`, `ENABLE_ARP`,
`ENABLE_ICMP`, `ENABLE_DHCP`, `ENABLE_DNS`, `ENABLE_DRIVER_E1000`,
`ENABLE_DRIVER_RTL8139`, `ENABLE_DRIVER_NE2000`.

Set to `no` to exclude that subsystem from the build.

---

## 15. Key Source Files

| File | Description |
|------|-------------|
| `network.c/h` | Core: checksums, endianness, global state, Ethernet dispatch |
| `netdev.c/h` | Device registration, PCI probing, IRQ dispatch |
| `ip.c/h` | IP send/receive, protocol handler table |
| `arp.c/h` | ARP cache, request/reply |
| `tcp.c/h` | TCP state machine, connection management |
| `udp.c/h` | UDP bind/send/receive |
| `icmp.c/h` | ICMP echo request/reply |
| `dhcp.c/h` | DHCP discover/offer/request/ack |
| `dns.c/h` | DNS query encoding/decoding |
| `route.c/h` | Routing table, longest prefix match |
| `e1000.c/h` | Intel e1000 driver |
| `rtl8139.c/h` | Realtek RTL8139 driver |
| `ne2000.c/h` | NE2000 driver |
| `virtio_net.c/h` | VirtIO MMIO driver (AArch64/RISC-V) |
| `net.h` | Network types, socket API, NIC driver interface |

---

## 16. Limitations and Future Work

**Working:** PCI discovery, Ethernet, ARP, IP, ICMP, UDP, TCP connections,
DHCP, DNS, routing, socket API.

**Planned:** TCP retransmission/reliability, congestion control, sliding window,
IP fragmentation, IPv6, ARP cache expiry, checksum validation on receive,
DNS caching, socket options (SO_REUSEADDR), non-blocking I/O/select().

---

## Quick Reference

```c
network_init();                          // Init stack
dhcp_discover();                         // Get IP via DHCP
icmp_send_echo_request(ip, id, seq, d, l); // Ping
dns_query("example.com", dns_ip, cb);    // Resolve hostname

tcp_connection_t* c = tcp_connect(ip, port); // TCP connect
tcp_send(c, data, len);                      // Send data
tcp_listen(port);                             // Listen
tcp_connection_t* cl = tcp_accept(ls);        // Accept

udp_bind(port, cb);                     // UDP bind
udp_send(ip, port, data, len);          // UDP send

route_add(dest, mask, gw, if, metric);  // Add route
route_lookup(dest, &next, ifname);      // Lookup route
```

---

*Happy networking on Forest OS!*
