# Multi-file Agent Edits

## Overview

Agent tools currently permit approved shell commands and file reads.  Add a
reviewable multi-file edit proposal flow that keeps the user in control and
cannot leave files partially modified after an error or denial.

**Verified state (2026-08-22):** `copilot_register_tools()` registers only
`vim_run_shell` and `vim_read_file`; `copilot_tool_run()` confirms each call
with a default-No dialog.

## Approach / Steps

1. Capture the language-server client-tool schema for structured edits before
   registering a new tool.  Prefer a single `vim_apply_workspace_edit` request
   containing URI/range/text changes over unrestricted `vim_write_file`.
2. Parse and validate every edit before asking for consent: require file URIs,
   reject duplicate/overlapping ranges, convert UTF-16 positions with the
   existing helpers, and show a concise per-file diff/summary in the dialog.
3. Apply accepted edits as one transaction: verify all target buffers/files
   and changedticks first, stage untouched-file writes safely, modify loaded
   buffers through Vim APIs, and roll back staged changes on any failure.
   Never write buffers with unsaved conflicting edits without a second clear
   confirmation.
4. Return per-edit success/failure to the server and preserve the existing
   default-No, per-request consent model.  Scope file access to the workspace
   by default, with a documented explicit override for external files.
5. Add mock scenarios for accepted multi-buffer edits, decline, invalid range,
   overlapping changes, changed buffer, write failure, undo, and server
   response.  Update help with consent, scope, conflict, and recovery rules.

## Risks

- **An agent changes files the user did not intend.**
  - Mitigation: require a default-No review showing exact paths/diffs and
    enforce workspace scope unless explicitly expanded.
  - Verification: mock external-path and denied-edit cases prove no mutation.
- **Partial failures corrupt a multi-file change.**
  - Mitigation: validate/stage all edits before mutation and provide rollback
    for on-disk writes; do not treat a shell command as an edit transaction.
  - Verification: inject a failing second file and compare every buffer/file
    with its pre-request content.
- **Concurrent edits invalidate server ranges.**
  - Mitigation: bind proposals to document versions/changedticks and reject
    stale requests with a retryable result.
  - Verification: modify a target after proposal creation and assert no write.

## Timeline

1. Verify server schema and define the structured edit contract.
2. Implement parse, preview, transaction, and response handling.
3. Add failure/undo coverage and user documentation.

## Priority

**P1.** This is the key missing agent capability, but it must meet a stronger
consent and atomicity bar than the existing read/run tools.