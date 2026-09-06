# Security status and reporting

## Current status

**FNU/PRONINX Core 0.1.5-dev must not be exposed to an untrusted network or
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

## Development-image account and IPC limits

The image initializes local `root`, `admin`, and `user` records. These are
development-image accounts, not secure provisioning defaults. The account
implementation uses salted SHA-256 hashes and supports at most 16 accounts,
UID/GID values, wheel membership, `chmod`, `chown`, and local `doas`
authentication. It does not provide modern password hardening, roles, remote
authentication, or a complete access-control model.

The supervisor exposes `/fnusvc.command`, `/fnusvc.status`, and `/.fnuhealth`
as volatile kernel-state devices. They are local control/observation
interfaces, not authenticated IPC or audit storage; their contents reset at
reboot.
