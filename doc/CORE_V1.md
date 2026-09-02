# FNU/PRONINX Core v1 delivery baseline

## Product boundary

Core v1 is a single, hardened x86-64 server node for internal networks. It
boots a Foundry-signed image, runs Foundry-signed native services, and is
operated through local serial console or SSH public keys. It is not a Linux
distribution, hypervisor, container runtime, cluster manager, router, or data
centre control plane.

## Required delivery sequence

1. Replace the legacy boot path with a UEFI Secure Boot chain. Keep release
   signing keys offline; separate root and release keys; reject unsigned or
   revoked boot components.
2. Add VirtIO block and network drivers for the QEMU reference platform, then
   PCI/NVMe support for one certified x86-64 server model. Do not certify
   unspecified hardware.
3. Add IPv4, ARP, DHCP/static addressing, TCP, DNS, TLS and an SSH daemon.
   Use an audited C cryptographic library; no in-house cryptography.
4. Add named service identities, an operator/administrator authorization
   model, a signed service-bundle format, and a service manager that starts
   only trusted bundles.
5. Add encrypted data storage. Seal the data key to measured boot when TPM
   2.0 is present; otherwise require local console unlock.
6. Add signed offline update bundles with A/B system slots, health confirmation
   and automatic rollback.

## Implemented offline foundation

Core 0.1.0-dev now starts a local `fnusvc` supervisor from a minimal `fnu-init`
PID 1. Its service registry starts and restarts `fnu-health` and PSH, which is
the standard local administration shell. PSH retains its engineering workflow
and adds node status, service status, health observation, and health-service
lifecycle commands. If `fnusvc` exits, PID 1 reboots instead of leaving an
orphaned service tree beside a new one. The `procinfo` system call returns a
bounded process snapshot, so `top` shows active service processes. None of
these components authenticates users, verifies signatures, or opens a network
listener.

`fnusvc` publishes its current local service snapshot through the volatile
`/fnusvc.status` kernel state device; the administration console exposes it
through `services`. The command queue and health snapshot use the same
non-persistent mechanism, so immutable UFS2 roots are never modified. A
service that exits more than three times in one supervisor lifetime is marked
`failed` and is not restarted automatically. This prevents a tight restart
loop, but is not yet a durable audit trail, atomic publication protocol, or
production remediation policy.

The offline control plane now has a bounded `waitpid(pid, WNOHANG)` kernel ABI.
`fnusvc` polls its local command file and supports start, stop and restart for
registered services; PSH queues only its fixed health-service command set.
This is a development-only local IPC mechanism, with no authentication,
durability or concurrent-writer guarantees.

## Release gate

A candidate passes only when CI reproducibly builds it; QEMU/OVMF rejects
tampered boot, service and update artifacts; an SSH key can administer a
networked service node; role checks and audit records are verified; interrupted
updates roll back; and the certified NVMe system completes the same smoke
tests. Independent security review and an SBOM/CVE process are mandatory
before production use.
