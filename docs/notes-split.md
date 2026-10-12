# Notes: sequential implementation blocks

Status: proposed execution grouping; implementation and verification remain
pending.

Use **nine blocks**, executed in order, plus block W for Windows-only work when
a Windows host is not available during the earlier blocks. Each block produces
a result that can be reviewed, tested, and committed before the next block
uses it. The boundaries follow integration and regression risks: storage,
Chat extraction, the editor core, the screen, and native lifecycle changes
are in separate blocks.

The intended executor is Grok 4.7 with a 500K context window. The four Notes
documents together are about 60K tokens, so context size does not limit a
block; review and verification effort do. The context size is not a target
for the amount of code changed in one block.

This document groups the [implementation plan](notes-plan.md); it does not
replace its steps or completion checks. Complete all work in each referenced
plan step, including checks not repeated in this summary.

## Document precedence

- The [PRD](notes-prd.md) defines required behavior and acceptance.
- For implementation, the [plan](notes-plan.md) wins, then the
  [detailed design](notes-design.md), then the [architecture](notes-arch.md).
- Read the architecture only as background. The design and plan supersede
  parts of it, for example the close Retry ordering, empty-slice paste
  handling, and the transaction guard.
- Report a conflict that these rules do not resolve. Do not choose silently.

## Block overview

| Block | Plan steps | Result | Operator checkpoint |
| --- | --- | --- | --- |
| 1. Baseline and editor feasibility | 1, 2 | Known baseline, UI approval scope and pending gates, tested dependencies, and editor/tree feasibility | UI review; packaged CSP check |
| 2. Storage and native API | 4, 5 | Notes persistence and native calls, with compatibility and admission tests | None |
| 3. Shared prompt | 6 | Reusable prompt and App-owned drafts, with Chat behavior preserved | None |
| 4. Editor and save foundations | 7, 8 | Atomic editing, history, autosave, leave coordination, and history-event handling | None |
| 5. Basic Notes screen | 10 | Real editable Notes screen with navigation, tree browsing, create, and open | Short manual check in the app |
| 6. Native close | 3, 9 | Selected macOS termination path and verified close negotiation | Packaged macOS close checks |
| 7. Organization and authoring | 11, 12 | Rename, move, delete, formatting, and prompt insertion | Deletion-close checks |
| 8. Content import and search | 13, 14 | Pictures, clipboard content, links, Find, and Replace | Real clipboard content and link opening |
| W. Windows work (conditional) | Windows parts of 2, 9, 11, 13, 14 | Windows behavior at the level macOS reached in blocks 1–8 | Packaged Windows checks |
| 9. Vault integration and release verification | 15, 16 | Verified vault behavior and complete packaged acceptance | All 23 PRD scenarios on both platforms |

Step 3 runs in block 6, directly before step 9, instead of at its position in
the plan's delivery order. Only step 9 uses its result, its only dependency
(step 1) is met, and the plan specifies the fallback if the experiment fails.

## Execution rules

Run one block at a time, starting from the committed result of the previous
blocks. A block is a task, review, and commit boundary. Use smaller internal
changes when useful, but finish and verify the block before moving on. Do not
introduce temporary production abstractions or a separate test framework to
make the boundaries work.

At the start of each block, read:

- the repository instructions in `AGENTS.md`;
- this document and the full plan;
- the PRD and design sections that the block's plan steps reference;
- the previous block's handoff.

Inspect the actual code, the previous block's commit, and the test results
rather than assuming the earlier implementation matches its summary. Run the
relevant existing checks in each block; block 9 does not replace verification
during development.

Intermediate editable builds are development builds. Do not release an
editable Notes build until its save, navigation, deletion, and native close
checks pass on both platforms. A completed block is not release approval, and
no block authorizes publishing a build. Use the platform package targets in
plan section 5, never the generic `make package` target.

Record progress, decisions, approvals, and verification in the handoff. Do
not tick plan checkboxes or edit files in `docs/` without a separate explicit
request.

### Operator checkpoints

Some checks need a person: visual approval, interaction with a packaged app,
real clipboard content from other applications, IME, dictation, and the
system browser. The executor never reports such a check as passed on its own.

At an operator checkpoint, the executor:

1. Finishes the automated work and checks of the block.
2. Builds the required local package or development app.
3. Writes numbered steps with the expected result of each step, the build
   revision, and how to cause each failure or delay that a step needs.
4. Stops and waits for the operator's results.

The operator runs the steps and reports the result of each one. The executor
records the results, fixes failures, and asks the operator to repeat the
failed steps. The block is complete only when the operator results pass. A
check that nobody ran is pending, never passed. An unavailable environment is
never a successful test result.

### UI approval

Block 1 presents all remaining visual proposals and records exactly what the
operator approved and what remains pending. Carry forward the approvals already
recorded in design sections 1 and 3. Pending UI approvals do not prevent
completion of block 1 once its technical checks and proposal review are
complete; they gate the blocks that implement the affected visible scope.

Blocks 2–4 have no visible changes and can proceed while approval is pending.
Blocks 5, 7, 8, and 9 implement visible changes; each starts only when its
visible scope has approval. If a proposal changes later, stop at that change
and obtain approval before implementing it. The temporary packaged
feasibility build in step 2 needs no UI approval.

### Windows work

In block 1, record whether a Windows host is available to the executor and
operator for blocks 2–8. Use one of these modes for the whole run:

- **Windows available:** do each step's Windows work in its own block,
  together with the macOS work. Block W is not used.
- **Windows not available:** do the macOS work in blocks 1–8, then do block W
  on a Windows host before block 9. Blocks 1–8 are complete when their macOS
  work and checks pass. Each handoff lists its pending Windows items.

Do not write Windows-only code that cannot be built and tested in the current
block. Shared frontend code is not Windows-only and stays in its own block.
The plan's platform rule still applies: pending Windows close work does not
block steps 11–15 on macOS. Block 9 starts only after the Windows work is
complete in either mode.

### End of a block

1. The executor finishes the block, including its operator checkpoint.
2. The operator reviews the change.
3. The operator gives an explicit commit command. The executor never commits
   without that command and never pushes.
4. The commit message contains the handoff. The next block reads it with
   `git log -1` and starts from that commit.

If the operator decides not to commit, the handoff stays in the task
response, and the operator passes it to the next block.

## Block 1. Establish the baseline and resolve editor feasibility

**Scope: steps 1–2.** Read the requirements, preserve unrelated work, run the
baseline checks, and record existing failures. Record the Windows mode. Use
the plan's mapping of the 23 PRD scenarios as the acceptance checklist.

Prepare all remaining visual proposals from design sections 3 and 14:
navigation and tree controls, row menus and dialogs, the find bar, link and
picture controls, the inactive selection marker, errors, replacement
confirmations, narrow and short window behavior, keyboard access, and the
native timeout dialog. Present them at an operator checkpoint and record
exactly what is approved.

Verify the dependency set and the editor, history, prompt insertion, search,
and tree behaviors that later steps rely on. Where possible, write the
feasibility code as the production editor module and its tests, so block 4
continues it instead of rewriting it. Run the temporary packaged CSP check at
an operator checkpoint. In Windows-not-available mode, the WebView2 parts of
step 2 move to block W; if WebView2 then rejects the dependency set, report
it as a blocker.

**Completion:** dependency and integration choices have test evidence; all
remaining visual proposals have been presented and their approved or pending
status is recorded; baseline failures, the Windows mode, and remaining
environment gates are explicit. Pending UI approvals gate the affected visible
work. Resolve a failed feasibility assumption before building the dependent
feature.

## Block 2. Implement storage and the native API

**Scope: steps 4–5.** Complete storage before exposing the native calls. Add
the Notes schema and operations through the existing database transaction
and locking paths, then add Application admission, DTOs, and native RPCs.
Keep document JSON opaque in the core and enforce the specified size limit.
Use the existing transport. Do not add a Notes HTTP API, an event stream, or
a migration system.

**Completion:** storage, old-vault schema creation, compatibility, and native
admission tests pass. Verify hierarchy operations, identifier behavior,
protected vaults, size boundaries, and stale vault context rejection. The
next blocks can list, create, load, and save Notes through the production
native API without a Notes screen.

## Block 3. Extract the shared prompt without changing Chat behavior

**Scope: step 6.** Implement the full prompt contract in the plan, including
the form and status slots, Chat-supplied Stop mode, synchronous draft reads,
submission controls, and voice lifecycle. Lift the two drafts to App and keep
Chat-specific decisions in Chat.

Keep this extraction separate from editor integration so that a Chat
regression has a small set of possible causes. Check the existing Chat
controls, asynchronous draft preservation, IME, speech, transliteration, and
speaker interaction. Include voice cancellation when `enabled` changes and
the delayed spoken send during generation.

**Completion:** Chat passes the step 6 regression checks with its current
behavior preserved. The shared prompt is ready for Notes to mount by note
identity. Do not add temporary Notes UI just to demonstrate the component.

## Block 4. Build the editor, autosave, and leave coordination

**Scope: steps 7–8.** Build the supported schema and atomic editor acceptance
first, continuing block 1's feasibility code and using the core size fixtures
from block 2. Then connect autosave and the shared leave procedure to the
native API. The acceptance boundary checks total document size only; image
entry checks belong to block 8.

Verify root and appended transactions together, Undo/Redo boundaries,
rejection without lost selection or history, save ordering, late replies,
and Retry/Stay/Discard. Consume empty paste slices without dispatch or fallback
before block 5 enables editing; block 8 adds supported fallbacks. Include the
paired `popstate` and `hashchange` case and the rule that Notes adds no URL route.
Provide the operation slot, its release point, and the decision state that the
close handler uses; block 6 implements the page-side close deferral of step 8
together with native close.

**Completion:** editor and save/leave behavior pass focused tests in the
existing suites. The screen can use these foundations without inventing
another save path.

## Block 5. Connect the basic Notes screen

**Scope: step 10.** Connect approved navigation, sidebar tabs, tree browsing,
create/open, the active editor, and the keyed Notes prompt. Connect accepted
editor changes to autosave and the leave procedure before enabling editing.
Formatting, import, search, organization, and native close remain in later
blocks.

Verify ordinary text edits, saving, switching notes, leaving Notes, empty,
loading, and error states, and accessibility in automated tests. At an
operator checkpoint, the operator creates, edits, leaves, and reopens a note
in the native app, and checks layout, resizing, and a narrow window.

**Completion:** the basic screen works through the actual native stack
without losing either panel's draft. This development build supplies the real
editor for block 6. Normal-close safety is not established yet.

## Block 6. Implement native close

**Scope: step 3, followed by step 9.** This order is required. First run the
macOS termination experiment in a packaged build and select one supported
termination path. Then implement close negotiation, including the page-side
close deferral, against the block 5 screen, using block 4's leave
coordination.

Cover both host entry points, the no-runtime and no-page-connection fast
paths, the response timer, deferred attempts, Retry, and late replies. Set
shutdown state and the `Closing CHA…` title only when negotiation permits
shutdown.

The existing macOS harness does not compile `main.swift`, so the macOS close
checks are packaged manual checks at an operator checkpoint. Include a failed
save followed by a hung Retry, close during a pending navigation flush, and a
save that completes after cancellation. In Windows-available mode, also
extend the Windows harness for the same cases and run the Windows packaged
checks.

**Completion:** the step 9 close checks pass with real unsaved notes on
macOS, and on Windows in Windows-available mode. The selected macOS path and
its experiment results are recorded in the handoff.

## Block 7. Add organization and everyday authoring

**Scope: steps 11–12.** Add rename, move, deletion, and metadata recovery
through the existing Notes operations and shared leave coordination.
Implement close deferral during deletion with step 9's attempt field. Run the
deletion-close checks at an operator checkpoint on each platform that has
completed step 9.

Then connect approved formatting controls and prompt-to-note insertion.
Preserve the specified selection, list, heading, and undo behavior, and clear
only the accepted draft prefix after insertion. Keep prompt content literal
and Notes separate from Chat context.

**Completion:** tree mutations and deletion failure recovery pass, with no
lost editor state or stale navigation. Formatting and successive prompt
submissions pass the plan's structural, selection, and history checks.

## Block 8. Add content import, links, and Find/Replace

**Scope: steps 13–14.** Complete picture picking, clipboard import, and link
handling first. Use the file importer and one shared slice-image check for
paste and drop. Consume all file drops without insertion. Do not add image
checks to the transaction acceptance boundary. Extend native HTTP/HTTPS link
opening on macOS, and also on Windows in Windows-available mode.

Then connect the approved Find/Replace controls to the verified search
extension. Keep replacements within the existing atomic editor and history
paths; do not add a second document index.

At an operator checkpoint, test real Word, web, and internal clipboard
content, clipboard image files, and link opening in the system browser.

**Completion:** supported and rejected imports, empty paste, link opening,
literal and regex search, empty replacement, and Replace All pass their
checks. Imports and replacements respect size limits and undo boundaries.
The planned editing feature set is present on macOS.

## Block W. Complete the Windows work (conditional)

Use this block only in Windows-not-available mode. Run it on a Windows host
after block 8 and before block 9.

**Scope: the Windows parts of steps 2, 9, 11, 13, and 14.** Verify the pinned
dependency set and the packaged CSP check in WebView2. Implement Windows close
negotiation through `request_close()`, keep `closing_` false during
negotiation, and keep the fatal-error and smoke-test paths direct. Extend
Windows link opening to HTTP. Add the Windows harness tests for steps 9, 11,
13, and 14, and include them in the runner's selected test names. Run the
Windows packaged checks at an operator checkpoint.

**Completion:** Windows behavior matches the macOS result of blocks 1–8, and
no Windows item from earlier handoffs remains pending. Do not start other
feature work in this block.

## Block 9. Verify vault integration and complete packaged acceptance

**Scope: steps 15–16.** Finish the vault operation integration and the
approved replacement confirmations, including the applicable ChaWeb
controls. Verify whole-vault copy and backup, configuration-only transfer
exclusions, protected vaults, and successful and failed vault switches using
disposable data.

Apart from step 15, this block adds no feature work. If an item from an
earlier block is still pending, stop and report it; do not absorb it into
this block.

Run the complete PRD acceptance checklist on both packaged platforms at
operator checkpoints. Include the macOS Edit menu Undo, Redo, and Paste
actions, keyboard shortcuts without duplicate execution, native clipboard,
real IME input, and real dictation where configuration permits.

Verify typing, saving, and reopening near the document limit. Perform the
additional many-note vault and shell message parsing measurements only if
the scenario 22 check shows lag. Remove experiment scaffolding and any state,
dependencies, or abstractions no longer needed.

**Completion:** all earlier gates and all 23 PRD scenarios pass on both
platforms, required builds and checks pass, and no platform work remains
unverified. Report the evidence and any remaining failure honestly;
incomplete acceptance means this block is incomplete. This block establishes
release readiness and does not authorize publishing a build.

## Handoff between blocks

Put the handoff in the block's commit message, or in the task response if
the operator does not commit. It contains:

- Completed plan steps and the main files changed.
- Decisions that the next block needs, including the approved UI scope and
  the Windows mode.
- Exact checks run and their results, including existing failures.
- Operator checkpoint results: build revision, steps, and the result of each
  step.
- Unrun checks and outstanding UI or platform gates, including pending
  Windows items.
- The next block and any prerequisite it must resolve first.

Do not create a separate progress database, tracking framework, or document
log for these blocks.
