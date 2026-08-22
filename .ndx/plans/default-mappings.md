# Default Mappings

**Implementation status: completed.** An opt-in runtime plugin provides
conflict-aware Insert-mode mappings: <Tab> accepts a visible suggestion and
otherwise retains its normal behavior, while CTRL-] dismisses a suggestion.
The focused Copilot suite verifies opt-in setup, Tab fallback, and preservation
of an existing user mapping.

## Overview

Users currently map `:copilot accept` and `:copilot dismiss` themselves.
Provide an opt-in, conflict-aware mapping layer rather than claiming `<Tab>`
globally in every Vim session.

**Verified state (2026-08-22):** the README and help give mapping examples,
but no runtime Copilot plugin defines a default mapping.

## Approach / Steps

1. Decide the public opt-in surface: a boolean option or a small runtime plugin
loaded only by `packadd`/explicit setup.  Preserve the default of no mapping.
2. Implement Insert-mode expressions that accept only when a Copilot suggestion
is pending; otherwise return the original key unchanged.  Supply a separate,
non-conflicting dismiss mapping and do not map `<Esc>` by default.
3. Detect a pre-existing buffer-local or global mapping before installation;
skip it with an explanatory message rather than replacing user, plugin, or
terminal behavior.  Provide an explicit unmap/disable path.
4. Add Vim-script tests for opt-in/opt-out, no-suggestion fallback, accept,
dismiss, pre-existing mapping preservation, buffer-local precedence, and
reload idempotency.
5. Document installation, key choices, conflict rules, and the existing manual
mapping pattern in `runtime/doc/copilot.txt` and README.

## Risks

- **`<Tab>` breaks indentation, snippets, or completion plugins.**
  - Mitigation: keep the feature opt-in and pass the exact key through unless
    a live Copilot suggestion exists.
  - Verification: tests assert the fallback mapping result and preserve a
    user-defined `<Tab>` mapping.
- **Mappings are duplicated or removed incorrectly on reload.**
  - Mitigation: track only mappings installed by the feature and make setup
    idempotent.
  - Verification: source the runtime file twice, then disable it and inspect
    `maparg()` metadata.
- **A mapping accepts stale ghost text.**
  - Mitigation: reuse `copilot_accept()` state checks and keep stale suggestion
    cleanup in existing Insert-mode lifecycle paths.
  - Verification: leave Insert mode or edit after a reply, then assert the
    mapping falls through and no text changes.

## Timeline

1. Select the opt-in API and key set.
2. Implement conflict-aware mappings with focused tests.
3. Document and validate alongside common completion configurations.

## Priority

**P2.** It reduces setup friction, but must not disrupt Vim's heavily customized
Insert-mode key behavior.