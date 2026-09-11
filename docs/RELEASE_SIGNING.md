# Release signing handoff

Release signing is intentionally external to the repository. Private keys must
never be placed in source, build profiles, packages, CI logs, or gateway data
bundles.

The OTA verifier authenticates this canonical message:

```text
<version>\n<platform>\n<archive-sha256>\n
```

The release pipeline should:

1. Build and test the target artifact.
2. Compute the archive SHA-256.
3. Sign the canonical message with the release key in a protected signing
   service or offline workstation.
4. Publish the archive, SHA-256, signature, platform, and version together in
   the update manifest.
5. Store only the public key in the gateway's managed data directory.
6. Test a valid signature, wrong-platform signature, wrong-version signature,
   and modified-archive checksum before offering the update.

When `update.require_signature` is enabled, a missing or invalid signature is
always rejected. The `1.2.0` local test package currently has checksum and
metadata validation coverage; live signature and OTA restart/rollback tests
remain pending until the signing key and managed target fixture are available.
