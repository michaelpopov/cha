# Block W: Conditional Windows completion

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block W; Windows portions of plan steps 2, 9, 11, 13, and 14.
Next: [block 9](notes-block9.md). Skip this block in Windows-available mode.

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Use this block **only in Windows-not-available mode**, on a Windows host after
block 8 and before block 9. In Windows-available mode, the work belongs to its
earlier owning blocks and this block is skipped. Implement/verify only the
Windows portions of plan **steps 2, 9, 11, 13, and 14**. Reach the same verified
behavior as macOS blocks 1–8; start no other feature work here.

## Prerequisites and reading

Complete blocks 1–8's macOS/shared scope and collect every pending Windows item
from their handoffs. Read the full plan/split, the referenced steps, design
sections 7–9/11–12/15, and all relevant PRD acceptance scenarios. Inspect shared
implementation and macOS results rather than treating summaries as proof of
Windows parity. Require existing approved visuals; re-present any Windows
proposal that changes the approved UI before implementing it.

Do not start this block without a Windows executor/operator able to build the
actual host, run WebView2, exercise native dialogs/clipboard/system browser, and
report results. An unavailable host is pending verification, not success.

## Main code paths

Use `packaging/windows/main.cpp`, existing native script/frontend close helpers,
`tests/native/windows/webview2.spec.ts`, `playwright.config.ts`, and `run.ps1`.
Shared editor, import, search, save, and leave code remains owned by the earlier
blocks. Change it only to fix a demonstrated parity issue and rerun affected
macOS checks; do not fork platform-specific frontend implementations.

## Implementation sequence

### 1. Resolve the WebView2 dependency/CSP gate

Use block 1's exact installed packages and lockfile. Build both frontend bundles
and the real Windows host; record OS/WebView2 versions and pin evidence. Run the
early packaged editor check using actual CSP and bundled CSS with injection
disabled. Confirm whitespace, highlighting, selection/decorations, tree keyboard
behavior, representative paste, and atomic/history/insertion/search assumptions.

An incompatible dependency set or missing editor hook is a blocker for both
platform readiness. Report and resolve it with the smallest shared change,
retaining notices and exact tested pins. Never relax CSP or call the macOS
experiment Windows evidence.

### 2. Implement the Windows close host path

Use block 6's trusted shell message contract/attempt IDs. Exit, WM_CLOSE, and
page close enter `request_close()`. No runtime or no page connection uses direct
shutdown without timer/dialog; a connected but unresponsive page uses fallback.
Keep `closing_` false, runtime/bridge/window alive, and current title during
negotiation. Start one native 15-second wait; repeated requests preserve it.

Ready/Close anyway permits `begin_shutdown()`, bridge detachment, `Closing CHA…`,
and the separate existing ten-second runtime shutdown deadline. Cancelled clears
only the current wait. Timeout invalidates/notifies the page before native
Cancel close / Close anyway; no timer runs during the decision. Fatal-error and
smoke-test paths enter shutdown directly rather than presenting a save decision.
Ignore stale/untrusted connection/attempt messages and old-page replies.

Verify the shared App behavior against this host: one deferred attempt during
leave/deletion; no competing flush; same remaining host timer; decision cancels
wait; Retry sends a fresh close request before saving; Discard settles/disposes
before requesting close; aborts clear only matching deferred work and cannot
resume after late completion. Do not add a second page timer/coordinator.

### 3. Verify deletion and history navigation on Windows

Exercise affected/unrelated subtree deletion, waiting for an admitted save,
pending delete, success/failure restoring/disposing editor, and deferred close
resumption. Hung deletion/save reaches native fallback; duplicate Exit does not
extend the timer; late completion after Cancel close cannot close the app.

Check Back/Forward through Alt+Left and mouse Back, paired history events, Stay
restoring the captured address, ignored competing history requests, and no Notes
route. Confirm navigation and deletion share one operation slot and use current
metadata ancestry after failed refresh.

### 4. Complete native web-link opening

Allow validated parsed HTTP/HTTPS on Windows in both main/new-window interception;
launch the saved destination in the system browser while preserving the editor.
Retain explicit HTTP and bare-domain HTTP policy. Other schemes never reach
OS APIs. Secondary click opens link actions without launching. Test keyboard
Edit/Remove, invalid/canceled edits, retained text/marks, Undo/Redo, save/reopen,
and no unwanted relinking.

### 5. Verify actual clipboard, authoring, and search parity

Run the shared import/search test cases in WebView2: all four image types,
clipboard image files, real Word/web/internal content, table text, supported
highlighting, file-drop no-op even with alternate formats, empty paste over a
selection, mixed invalid/oversized slice rejection, and offline reopen. Confirm
1 MiB raw picture and 12 MiB compact-document boundaries and whole Undo/Redo.

Verify literal/regex/case/word search, empty replacement, literal `$1`, unsupported
RE2, mark/hard-break boundaries, selected insertion target, scoped shortcuts,
original-set Replace All, and near-limit atomic rejection. Search must not save
or disturb typing history. Include real IME and prompt lifecycle behavior at
Windows checkpoints, keeping provider-dependent dictation separate.

### 6. Extend and actually run the Windows harness

Add meaningful cases to the existing harness/specs and **runner's selected test
names**. The current runner uses a grep expression; a test file that is never
selected does not establish coverage. Use actual Notes screen/disposable vault
helpers and controlled failure/delay mechanisms, without a separate framework.
Record automated evidence separately from packaged manual results.

## Verification and operator checkpoint

Use the Windows versions of plan section 5's C++/frontend checks and run:

```powershell
.\tests\native\windows\run.ps1
```

Build required host targets, runtime/assets, and vault-preparation helper first;
record any environment overrides and which cases the runner executed. Package
locally from the repository root with:

```sh
make package-windows VERSION=0.0.0-notes-dev
```

Provide revision, exact pins, Windows/WebView2 versions, disposable vault setup,
and numbered steps with expected results. Use the block 1 failure-induction
methods, and confirm that each one works on Windows. Require operator
results for Exit/window close, save decisions/hung Retry, navigation/deletion
close waits, no-runtime/no-connection, stale/duplicate/late replies, fatal/smoke
paths, title timing, CSP, actual clipboard, system-browser links, keyboard/IME,
short windows, search, and near-limit behavior. Reproduce failures, fix, and
repeat affected steps. Do not claim cross-platform acceptance from DOM tests.

## Completion and handoff

Every Windows item from blocks 1–8 has actual passing evidence and no unresolved
parity gate. List completed source steps, Windows/shared files changed, selected
harness cases, operator results/build versions, and any shared fixes retested
on macOS. If any pending item remains, this block is incomplete and block 9
must not start. Block 9 still runs the full PRD acceptance on both hosts.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
