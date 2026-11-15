# Networking

The stack is lwIP 2.2.1 under `kern/net/lwip/`, the interface layer in
`kern/net/if.c` and the drivers below it. DNS and ping live in userspace.

## Drivers

### Intel PRO/1000 (`em0`)

`kern/net/e1000.c` sits on Intel's shared code in `third_party/e1000/`,
taken from FreeBSD as is. It knows the same 189 device ids as FreeBSD's `em`
and `igb`: the 8254x, 82571 to 82583, the ICH and PCH parts up to Meteor Lake,
and the 82575, 82576, 82580, I350, I210 and I211. The T520's 82579LM works.
QEMU's `e1000`, `e1000e` and `igb` all attach.

### Realtek (`re0`)

`kern/net/re.c` is written after OpenBSD's `re.c`. It handles the RTL8139C+,
RTL8101 to 8106, RTL8168B to 8168H, RTL8169 and RTL8401 to 8411. It was
written for the RTL8168 in the Y520. QEMU's `rtl8139` uses the 8139C+ path.

### VirtIO (`vtnet0`)

Legacy VirtIO PCI (`0x1af4:0x1000`). QEMU: `-device virtio-net-pci`.

## lwIP glue

`kern/net/lwip/lwip.c` connects lwIP to the kernel. lwIP is built `NO_SYS`,
so every call into it (interrupts, timers, sockets) takes one spinlock,
`netlock`, much like `splnet`. Loopback frames go through a netisr thread.

Each TCP connection has a receive ring. The window shrinks when the reader
stops reading. Buffers start at `net.inet.tcp.recvspace` (64 KB) and
`net.inet.tcp.sendspace` (32 KB) and grow up to 2 MB with window scaling,
like FreeBSD's autotuning. `sendbuf_auto` and `recvbuf_auto` turn that off.

RTT is measured per segment in nanoseconds, since lwIP's 500 ms ticks are
too coarse for a LAN. Initial sequence numbers follow RFC 6528 (SipHash over
the 4-tuple). `close(2)` sends FIN. `shutdown(SHUT_RD)` stays local because
lwIP would answer later data with RST.

Do not define `LWIP_DEBUG` at all; lwIP tests it with `#ifdef`.

## Sockets

`kern/socket.c` provides the BSD socket calls. A socket is a file descriptor,
so `read`, `write`, `close`, `dup`, `fcntl` and `select` work on it. Numbers
and structures are in `doc/ABI.md`.

Raw ICMP sockets (`FNU_SOCKET_RAW`, `FNU_PROTO_ICMP`) deliver datagrams with
the IP header. Only root can open one. `ping` is setuid root and drops
privileges after opening it.

## Names

DNS servers come from the DHCP lease or `netconfig` and show up in
`net.inet.dns.servers`. `fnu_resolve()` in libfnu accepts a dotted address,
then tries `/etc/hosts`, then asks the servers for an A record over UDP.

## Tools

| command | what it does |
| :--- | :--- |
| `netinfo` | interfaces, addresses, gateway, DNS, DHCP state, counters |
| `netconfig` | static address, netmask, gateway and DNS, or DHCP again |
| `ping [-c count] [-i wait] host` | ICMP echo |
| `resolve name ...` | look up names |
| `netecho [-u] port [address message]` | TCP or UDP echo server, or send a message to one |
| `ssh [-p port] [-i key] [-l user] [-t] [-T] [user@]host [command]` | SSH client |
| `sshd [-p port] [-d]` | SSH server, started by the supervisor |
| `ssh-keygen [-t ed25519] [-f file] [-C comment] [-l]` | make or fingerprint a key |

## SSH

`user/ssh/` speaks SSH-2 with `curve25519-sha256`, `ssh-ed25519` and
`chacha20-poly1305@openssh.com`, plus `kex-strict`. The crypto is OpenBSD's.
It talks to OpenSSH 10. Host keys go to `~/.ssh/known_hosts`,
`~/.ssh/id_ed25519` is tried before a password, and `sshd` reads
`~/.ssh/authorized_keys`. The host key is created on first start. There is
no agent forwarding and no `scp` yet.
