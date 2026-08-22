# Telemetry Controls

## Overview

Telemetry is controlled inside the proprietary language server and the
captured initialize schema does not document a Vim-side setting.  Establish a
truthful, versioned control surface only if the server exposes one; otherwise
document the server-owned configuration and its limits clearly.

**Verified state (2026-08-22):** `copilot_init_params()` sends editor metadata
only, and `COPILOT_PROTOCOL.md` records no accepted telemetry field.

## Approach / Steps

1. Obtain vendor documentation or use schema-validation probes to determine
   whether telemetry can be configured through environment, command arguments,
   persistent server configuration, or initialization options.  Record the
   exact supported server versions and precedence.
2. If a supported non-secret control exists, add a narrowly typed Vim option
   with conservative default semantics and pass it via the documented channel.
   Reject values outside the proven enum; do not invent an endpoint override.
3. If no client control exists, add no ineffective option.  Instead document
   the server's actual configuration location/command, what Vim can and cannot
   influence, and how to inspect the installed server version.
4. Add mock schema tests for the chosen transport and reset/default behavior.
   For an external server setting, use a fixture configuration and verify that
   Vim preserves it rather than overwriting it.
5. Review help/README wording with privacy and compliance requirements in
   mind; never promise that disabling a Vim option disables all GitHub service
   telemetry unless the vendor contract proves it.

## Risks

- **A local option gives users a false privacy guarantee.**
  - Mitigation: require vendor-confirmed semantics and state the scope/version
    explicitly; otherwise document only the real server control.
  - Verification: capture the server's accepted configuration and a controlled
    telemetry-related behavior/log in an approved test environment.
- **A setting is sent in an unsupported protocol field.**
  - Mitigation: make schema-validation evidence a blocking design input and
    fail closed for unknown values.
  - Verification: mock rejects unknown fields; a pinned real server accepts
    the documented payload.
- **Configuration leaks across projects or changes global state.**
  - Mitigation: scope any Vim value to the job it creates and never rewrite the
    server's persistent configuration implicitly.
  - Verification: start two controlled sessions with different values and
    inspect each mock/job environment independently.

## Timeline

1. Verify whether a server-supported control exists.
2. Implement and test it, or document the confirmed server-owned path.
3. Review wording and add regression coverage for the selected contract.

## Priority

**P1.** Privacy/compliance needs are important, but a visible control is only
useful when it truthfully reaches the language server.