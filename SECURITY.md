# Security status and reporting

## Current status

**FNU/PRONINX Core 0.1.8-dev must not be exposed to an untrusted network or
used for confidential data.**

The current code predates the Core security architecture. It has no secure
boot verification, cryptographic package verification, network authentication,
privilege-separated services, multi-user access control, encrypted storage, or
supported incident-response process.

## Target security baseline

The first supported release will require a signature-verified UEFI boot chain,
signed service and update bundles, SSH public-key authentication with defined
access roles, append-only audit records, and rollback-safe A/B updates. TPM
2.0 integration is an optional hardware root of trust; systems without it
require local unlock for encrypted data.

No feature is considered available until it is implemented, reviewed, tested,
and listed in a supported-release security guide.

## Reporting

Until Foundry Tech Inc. publishes a dedicated security contact and disclosure
policy, do not report vulnerabilities in public issue trackers. Use the
company's established private security channel and include a reproducible
proof of concept, affected revision, and impact assessment.

## Development image

The image has two accounts, `root` and `admin`. The bcrypt hashes are in
`/etc/master.passwd`, readable by root only. Members of `wheel` may use
`doas`. `login`, `su`, `doas` and `passwd` are setuid programs that check the
hash themselves; the kernel ignores the setuid bit on volumes mounted
`nosuid`. Roles, password ageing and an audit trail are not implemented. The
default passwords are in `README.md`. Change them.

Only root may write the supervisor's FIFO `/var/run/fnusvc.ctl`. `/dev/klog`
holds the last 64 KB of kernel messages and is readable by root and `wheel`.
The messages of the previous boot survive a reboot (in memory, or at the end
of the `fnu-swap` partition) and `dmesg` shows them once. They are for
debugging. They are unsigned and get overwritten.

## sshd

`sshd` listens on port 22 from boot and takes passwords and public keys for
any account, root included. It has no privilege separation and allows six
attempts per connection. Root and password logins cannot be turned off yet.
On a network you do not trust, stop it with `fnusvc` or unplug the cable.
