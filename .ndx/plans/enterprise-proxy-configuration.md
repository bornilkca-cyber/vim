# Enterprise and Proxy Configuration

## Overview

Vim starts the server with `job_start(..., "--stdio")` and currently exposes
no endpoint or proxy configuration.  Add only configuration transport that
the pinned language-server schema accepts, without exposing credentials in
messages, logs, or modelines.

**Verified state (2026-08-22):** `copilot_init_params()` has only editor
metadata in `initializationOptions`; `COPILOT_PROTOCOL.md` records no
Enterprise or proxy fields.

## Approach / Steps

1. Re-derive the exact server version's supported Enterprise endpoint and
   proxy configuration through vendor documentation or schema-validation
   probes.  Record accepted names, scope, and precedence in
   `COPILOT_PROTOCOL.md`; do not guess initialization fields.
2. Prefer standard environment variables for proxy transport when the server
   supports them.  Add narrowly named Vim options only for values that cannot
   be supplied safely by the environment, mark credential-bearing string
   options `P_SECURE`, and reject embedded newlines/control characters.
3. Construct a per-job environment dictionary and pass it only to the server
   process.  Do not mutate Vim's process environment, command line, debug log,
   or `:set` output with secrets.
4. Add mock-server modes that record non-secret endpoint/proxy inputs and
   reject unknown initialize fields.  Test default inheritance, explicit
   override, option reset, and no-secret logging.
5. Document the supported server versions, precedence, authentication limits,
   and a configuration example that uses environment variables for secrets.

## Risks

- **A guessed field silently has no effect.**
  - Mitigation: make protocol capture/schema validation a prerequisite to
    client implementation and pin the supported server version.
  - Verification: mock and real-server probes prove the value is received and
    changes the expected request route.
- **Proxy credentials leak through options or diagnostics.**
  - Mitigation: prefer inherited environment, use `P_SECURE`, redact all
    displayed values, and never serialize job environment in debug output.
  - Verification: inspect `:messages`, `:copilot log`, `:set`, and failure
    paths using a credential-shaped test value.
- **Enterprise routing breaks public GitHub sign-in.**
  - Mitigation: keep defaults unchanged and make endpoint configuration scoped
    to each new server process.
  - Verification: mock both default and Enterprise initialization in one Vim
    session after stop/start.

## Timeline

1. Discover and document the vendor-supported contract.
2. Implement secure job configuration and mock coverage.
3. Validate against a reachable approved endpoint and document support.

## Priority

**P1.** Enterprise and network-constrained deployments need this before they
can use the client, but the server contract must be verified first.