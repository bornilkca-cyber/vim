# Workspace Context

## Overview

Vim sends document open/change notifications only when a buffer is used and
does not establish workspace folders for project context.  Add an opt-in,
bounded workspace model that tells the server where the project is without
silently uploading an entire checkout.

**Verified state (2026-08-22):** `copilot_sync_buf()` synchronizes one buffer
at a time; the captured server handshake advertises workspace-folder support.

## Approach / Steps

1. Validate the current server's workspace-folder initialize and change
   notification schema with the mock and schema probes.  Define workspace root
   discovery from the current file, user option, or project markers, with the
   explicit option winning.
2. Add a disabled-by-default workspace option and initialize/reinitialize the
   server with a canonical file URI/name for the selected root.  Send only the
   root metadata initially; retain on-demand `didOpen`/`didChange` for file
   contents.
3. Offer an explicit workspace refresh command that computes added/removed
   folders and sends the supported change notification.  Do not add a global
   recursive scanner or OS watcher until a measured server requirement proves
   it necessary.
4. Enforce path canonicalization, symlink policy, maximum folder count, and
   exclusion of remote/unnamed buffers.  Make context scope visible in status
   and document exactly when contents leave Vim.
5. Test root selection, option precedence, initialization payload, refresh,
   stop/start, path escaping, and a large-tree fixture proving no eager file
   reads occur.  Measure startup latency before considering indexing.

## Risks

- **Workspace context leaks private files.**
  - Mitigation: opt in, initially transmit only folder metadata, clearly state
    on-demand document synchronization, and expose the active root.
  - Verification: mock capture asserts startup has no file contents and tests
    reject paths outside the selected root.
- **Large repositories make startup slow.**
  - Mitigation: avoid recursive indexing/watchers in the first version and set
    hard bounds before any later discovery feature.
  - Verification: time initialization with a large fixture and assert no
    directory traversal happens.
- **Root changes create stale server context.**
  - Mitigation: use the server's documented workspace-change notification or
    stop/reinitialize when unsupported.
  - Verification: switch projects in one session and verify the mock receives
    the right remove/add or fresh initialize sequence.

## Timeline

1. Verify protocol and root-selection policy.
2. Implement opt-in metadata and explicit refresh.
3. Measure, test privacy boundaries, and document behavior.

## Priority

**P2.** Project awareness improves answers, but privacy and performance require
an intentionally limited first release.