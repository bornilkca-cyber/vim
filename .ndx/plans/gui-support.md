# GUI Support

## Overview

The Copilot client renders inline suggestions through the `CopilotSuggestion`
text property in `src/copilot.c`, and writes chat output to a normal buffer.
Neither path has been exercised with a GUI build.  Make the feature work and
remain testable in supported gvim variants without changing terminal behavior.

**Verified state (2026-08-22):** `copilot_sugg_show()` uses
`prop_add_common()` and `redraw_after_callback()`, with no `FEAT_GUI` branch;
the focused suite only uses a terminal for automatic-suggestion coverage.

## Approach / Steps

1. Build the supported GUI variants with `+copilot`, `+job`, and `+popup` and
   run the focused suite with each GUI executable.  Record feature availability
   and exclude only configurations that cannot compile `FEAT_COPILOT`.
2. Add a GUI-capable screendump test alongside `test_copilot.vim` that starts
   the mock server, enters Insert mode, waits for the ghost text, accepts it,
   and opens a streamed chat turn.  Assert both display state and buffer
   contents, using the existing terminal test as the behavioral baseline.
3. Exercise GTK, Motif, and Windows GUI backends where CI/build infrastructure
   supports them.  If rendering differs, fix the smallest shared redraw or
   text-property path; introduce backend-specific code only for a demonstrated
   backend limitation.
4. Check focus changes, resizing, multibyte text, selections, and popup/menu
   overlap manually in each available GUI.  Preserve the existing `NonText`
   highlight and avoid changing user colors or layout without an explicit
   option.
5. Document the supported GUI coverage and any known backend restriction in
   `runtime/doc/copilot.txt`, including its `Last change:` header, only after
   the matrix establishes the actual support boundary.

## Risks

- **A redraw workaround regresses terminal rendering.**
  - Mitigation: keep shared rendering unchanged until a GUI failure is
    reproduced, and run the current inline-completion and auto-suggest tests
    with the terminal executable after every fix.
  - Verification: focused `test_copilot.res` passes for both terminal Vim and
    the affected GUI executable.
- **Backend/font width differences misplace multibyte ghost text.**
  - Mitigation: use text-property byte positions already computed by
    `copilot_byte_col()`; add a multibyte screendump fixture rather than
    calculating display columns in a new GUI path.
  - Verification: compare a UTF-8 fixture containing combining, wide, and
    emoji characters before and after accepting a suggestion.
- **GUI automation is unavailable in normal CI.**
  - Mitigation: make the GUI test conditional on a GUI executable/display,
    retain the deterministic mock server, and document a local Xvfb/Windows
    validation command for release checks.
  - Verification: skipped environments report the missing capability; an Xvfb
    job runs the same test without network access.

## Timeline

1. Establish the GUI build/test matrix and add the shared screendump fixture.
2. Reproduce and repair any backend-specific defect.
3. Run all available GUI variants, document the resulting support statement,
   and add CI coverage where a display is available.

## Priority

**P1.** The code has a plausible cross-platform rendering path, but GUI users
need evidence that inline and chat interaction is reliable before the feature
can be represented as supported.