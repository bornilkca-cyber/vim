# Server Platform Support

## Overview

The repository bundles a Linux x64 language server only.  `copilot_server_path()`
already prefers the secure `'copilotcommand'` option, so users can run a native
server on macOS, arm64, and Windows.  Establish and document a supported
distribution policy rather than adding unverified automatic downloads.

**Verified state (2026-08-22):** `'copilotcommand'` is `P_SECURE`, tests use it
to launch the mock server, and the fallback server path is
`runtime/copilot/copilot-language-server`.

## Approach / Steps

1. Obtain the server vendor's supported platform/architecture matrix and
   redistribution terms.  Decide whether each binary is shipped, fetched by a
   separately approved installer, or supplied by the user through
   `'copilotcommand'`.
2. Add platform selection only for binaries that may legally be distributed:
   choose a runtime path from the compiled OS/architecture, reject unsupported
   combinations with an actionable error, and preserve the explicit option as
   the highest-priority override.
3. Package each shipped executable under a platform-specific runtime path,
   preserve executable mode, and teach install/uninstall targets to include
   exactly the selected artifact.  Never run a downloaded executable during
   build or installation.
4. Extend the mock-server test to assert option precedence and missing-server
   diagnostics.  Run start/status/chat smoke tests on every available native
   runner and cross-build only where the produced binary can be inspected.
5. Update `runtime/doc/copilot.txt` and `README.md` with the actual matrix,
   the `'copilotcommand'` escape hatch, and the absence of automatic downloads.

## Risks

- **Bundling proprietary binaries violates licensing or platform policy.**
  - Mitigation: require written redistribution approval and checksums before
    adding any artifact; otherwise keep the user-supplied command path.
  - Verification: review package manifests and license notices per artifact.
- **A new selector overrides a user command or runs the wrong architecture.**
  - Mitigation: retain current explicit-option precedence and validate the
    selected executable before `job_start()`.
  - Verification: mock tests cover explicit command, bundled path, and a
    missing/unsupported target diagnostic.
- **Binary packaging creates large or non-reproducible releases.**
  - Mitigation: pin artifact versions/digests and incorporate them in the
    reproducible-package policy.
  - Verification: inspect release/package contents and compare repeat builds.

## Timeline

1. Confirm licensing and choose the distribution policy.
2. Implement selection/packaging and run native smoke tests.
3. Publish the verified matrix and release checks.

## Priority

**P1.** Non-Linux users can already supply a server, but first-class support
requires an explicit, legally sound delivery policy.