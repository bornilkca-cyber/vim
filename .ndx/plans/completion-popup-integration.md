# Completion Popup Integration

## Overview

Copilot currently requests `textDocument/inlineCompletion` and displays the
first result as a text property.  It does not participate in Vim's insert-mode
completion machinery.  Add an opt-in integration that respects existing
completion sources and never blocks typing on a network response.

**Verified state (2026-08-22):** `copilot_sugg_cb()` retains only the first
inline result and `copilot_ins_idle()` deliberately sends requests
asynchronously; no Copilot code calls Vim's `ins_complete()` path.

## Approach / Steps

1. Define the desired contract with Vim's existing completion owners: whether
   Copilot is a `completefunc` source, an explicit `:copilot complete` command,
   or a core completion-provider hook.  Prototype against the current
   `ins-completion` APIs and select the smallest stable integration point.
2. Add an opt-in option or command; do not set `'completefunc'` or alter
   `'completeopt'` globally.  Preserve virtual-text suggestions when popup
   mode is disabled.
3. Convert inline-completion replies into completion items with insertion text,
   replacement range, display text, and documentation only after confirming
   range semantics.  Track request generation, buffer number, changedtick,
   and cursor position so stale replies cannot populate a later popup.
4. Keep request dispatch asynchronous.  When a popup request cannot be ready
   in time, return no candidates and allow normal Vim completion to continue;
   never wait in `ins_redraw()`.
5. Extend the mock and focused tests for trigger, stale reply, accept, dismiss,
   undo, coexistence with a user completion source, and `completeopt` variants.
   Add screendumps only for UI behavior that buffer assertions cannot prove.

## Risks

- **Copilot delays or takes over ordinary completion.**
  - Mitigation: opt in explicitly, preserve user sources/options, and make all
    server interaction asynchronous with an empty-result fallback.
  - Verification: timed mock test proves Insert-mode input continues while a
    delayed reply arrives.
- **A stale range inserts into the wrong text.**
  - Mitigation: validate buffer, changedtick, cursor, and request ID before
    publishing candidates; discard invalid replies.
  - Verification: edit/move after a request and assert no candidate or change
    appears for the old location.
- **Core completion APIs differ across feature sets.**
  - Mitigation: gate the feature on the exact existing completion capability
    and leave inline completion available everywhere it works today.
  - Verification: compile and run focused tests in minimal supported and huge
    feature configurations.

## Timeline

1. Choose/prove the integration point with a mock prototype.
2. Implement opt-in asynchronous candidates and state validation.
3. Add interaction tests, docs, and feature-set coverage.

## Priority

**P2.** Inline suggestions already cover the primary workflow; popup support
is valuable only when it cooperates with Vim's established completion model.