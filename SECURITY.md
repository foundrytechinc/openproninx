# Security status and reporting

## Current status

**FNU/PRONINX Core 0.1.0-dev is not production-ready and
must not be exposed to an untrusted network or used for confidential data.**

The current code predates the Core security architecture. It has no secure
boot verification, cryptographic package verification, network authentication,
privilege-separated services, multi-user access control, encrypted storage, or
supported incident-response process. QEMU use is recommended only for
development evaluation.

## Target security baseline

The first supported server release will require a signature-verified UEFI boot
chain, signed service and update bundles, SSH public-key authentication with
operator/administrator roles, append-only administrative audit records, and
rollback-safe A/B updates. TPM 2.0 integration is an optional hardware root
of trust; systems without it require local operator unlock for encrypted data.

No feature is considered available until it is implemented, reviewed, tested,
and listed in a supported-release security guide.

## Reporting

Until Foundry Tech Inc. publishes a dedicated security contact and disclosure
policy, do not report vulnerabilities in public issue trackers. Use the
company's established private security channel and include a reproducible
proof of concept, affected revision, and impact assessment.
