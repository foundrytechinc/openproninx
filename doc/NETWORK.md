# OpenProninx Networking Subsystem

## 1. Overview & Architecture

OpenProninx integrates a modular network stack built upon the PRONINX driver framework, an adapter layer (`kern/net/adapter.c`), and a vendored `lwIP` (v2.2.1) protocol engine.

```
+-------------------------------------------------------------+
|                     Userspace Applications                  |
|    (ping, netinfo, netconfig, resolve, tcpecho, udpecho)    |
+-------------------------------------------------------------+
                              |
                     System Call Boundary
                              |
+-------------------------------------------------------------+
|                Kernel Protocol Stack (lwIP)                 |
|            DHCP, DNS, ARP, IPv4, ICMP, UDP, TCP             |
+-------------------------------------------------------------+
                              |
                     Adapter Layer (adapter.c)
                              |
+-------------------------------------------------------------+
|                 OpenProninx Driver Framework                |
|               (driver.h / driver.c / pci.c)                 |
+-------------------------------------------------------------+
                 /                           \
+---------------------------------+  +------------------------+
|   Intel E1000 Gigabit Driver    |  |  VirtIO-Net PCI Driver |
|       (em0 - e1000.c)           |  |   (vtnet0 - virtio_net)|
+---------------------------------+  +------------------------+
                 \                           /
+-------------------------------------------------------------+
|                   Underlying Hardware / QEMU                |
+-------------------------------------------------------------+
```

---

## 2. Supported Network Interface Cards (NICs)

### Intel E1000 Gigabit Ethernet (`em0`) - Primary Driver
- **Supported Chips:** Intel 82540EM, 82545EM, 82543GC, 82574L, 82541EI, 82541ER.
- **Features:** 
  - Hardware ring buffer DMA (64-entry TX and 64-entry RX rings).
  - Hardware checksumming and CRC striping (`SECRC`).
  - Automatic link negotiation (`SLU`, `ASDE`, `FD`).
  - Dual MAC acquisition (RAL/RAH register read with EEPROM fallback).
- **QEMU invocation:** `-netdev user,id=proninx-net0 -device e1000,netdev=proninx-net0`.

### VirtIO-Net PCI (`vtnet0`) - Secondary / Virtualized Driver
- **Supported Architecture:** Legacy VirtIO PCI Transport (`0x1af4:0x1000`).
- **Features:** Virtqueue split ring buffer (Available, Used, Descriptor rings), dynamic TX reclamation, RX buffer refilling.
- **QEMU invocation:** `-netdev user,id=proninx-net0 -device virtio-net-pci,netdev=proninx-net0`.

---

## 3. Userspace Networking Tools

| Command | Description |
| :--- | :--- |
| `netinfo` | Displays active network interfaces, MAC addresses, IPv4 address, Gateway, DNS server, DHCP state, and frame statistics. |
| `netconfig` | Manually configures static IPv4 address, netmask, gateway, and DNS, or restarts DHCP client. |
| `ping <ip>` | Sends ICMP Echo requests and reports round-trip time in milliseconds. |
| `resolve <host>` | Resolves domain names to IPv4 addresses using DNS. |
| `tcpecho <port>` | Starts a TCP echo server. |
| `udpecho <port>` | Starts a UDP echo server. |
