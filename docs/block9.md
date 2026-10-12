# Block 9: Vault integration and final verification

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 9; plan steps 15–16. This is the final block.

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

This block follows [block W](notes-blockW.md) in Windows-not-available mode,
and block 8 in Windows-available mode.

Implement plan **steps 15–16**. Finish vault integration and approved replacement
copy, then establish complete packaged acceptance on both desktop hosts. Apart
from step 15, this block adds no new feature work. If an earlier block's item
is pending, stop and return it to that owning block; do not absorb it here.
Successful verification establishes readiness, not permission to publish.

## Prerequisites and reading

Complete all earlier technical, approval, operator, and platform gates. Windows
must already be verified through blocks 1–8 in available mode or through block W
in deferred mode. Read plan steps 15–16 and sections 4–6; design sections 14–15;
PRD section 5.1, all 23 scenarios in section 8, and exact limits in section 9.
Inspect earlier commits/test evidence and the actual integrated code.
Require approved Download/Parent merge confirmations including applicable ChaWeb
controls. Do not assume a design document itself supplies approval.

## Main code paths

Use existing vault operations/maintenance/protection/copy tests, App's leave and
switch boundaries, desktop vault controls, and applicable
`webapp/src/chaweb/vaultActions.tsx`. Keep SQL/store/native Notes API unchanged
unless a demonstrated integration bug requires a fix in its owning block.
No Notes transfer subsystem, item restore UI, or ChaWeb Notes editing is needed.

## Implementation sequence

### 1. Verify full-database and configuration-transfer boundaries

Use disposable collections with nested folders, Unicode names, marked text,
images, links, and deleted IDs that expose sequence non-reuse. Include plaintext,
protected, older valid schema-2 vaults without Notes, and individual malformed
note content. Verify:

| Operation | Expected Notes behavior |
| --- | --- |
| New without copy | Empty collection. |
| Whole-vault Copy | All metadata/content/embedded images/links and non-reuse sequence retained. |
| Upload | Whole saved database, including edits saved before leaving for Settings. |
| Download | Remote collection replaces local collection; previous local database replaces `.bac`. |
| Password/protection change | Collection, embedded images, and sequence preserved. |
| Merge | Source Notes excluded and destination Notes intact. |
| Parent merge | Parent preliminary Download replaces parent's collection/backup; active Notes unchanged by configuration merge. |
| Workspace Import/Export | No source Notes transferred or destination Notes replaced/deleted. |
| Session Markdown mirror | No note documents emitted. |

Settings' shared leave guard is the final save boundary before Upload/Copy;
do not add another Notes flush in each button. Keep existing maintenance and
epoch admission. Individual corrupt note JSON must not introduce a new whole-
vault-operation blocker through eager note scanning.

### 2. Apply the approved replacement scope and verify backups

Download/Parent merge confirmation must explicitly describe replacement of
local notes/folders and replacement of the existing single `.bac` previous-
database backup. Apply equivalent approved scope where offered in ChaWeb,
without adding Notes API/editor controls. Use existing dialog/focus patterns.

Test replacements with newer and older valid schema-2 databases. Saved final
edits are in `.bac`; explicitly discarded unsaved edits are absent. A second
successful replacement overwrites the prior backup. Older vault reopen creates
an empty Notes table through existing shared schema creation. For Parent merge,
a later merge failure does not undo a completed parent Download/backup, and
active-vault Notes stay unchanged.

Keep live R2/provider transfers separate from offline tests. Use disposable
configured resources and report credentials/environment-dependent cases that
cannot run. Do not claim an unrun transfer passed from inspecting copy code.

### 3. Verify switch/context isolation end to end

Exercise successful, failed, and canceled vault switches with unsaved/pending
writes. Guard before session cleanup/core switch. Drafts reset only after
success; failed/canceled switches retain them. Notes editor/tree/import/dialog/
voice work from the old database cannot affect the replacement, even when IDs
match. Maintenance epoch changes alone do not imply draft reset. All admitted
old-context operations must fail safely or finish under existing lifecycle rules;
no delayed save writes into another vault or restores replaced data.

### 4. Run complete packaged acceptance

Run every original PRD scenario in full on both macOS and Windows. The table
below identifies where evidence originated; it does not replace scenario text
or permit skipping cross-platform repetition. Record each result with build
revision, exact package pins, OS/browser version, test data, and failure/delay
reproduction. Operator-dependent scenarios require operator reports.

| PRD | Acceptance focus | Main blocks |
| --- | --- | --- |
| 1 | Create, format, normal close/reopen through both entry points | 2, 4–7 |
| 2 | Organization, sorting, persistence | 2, 5, 7 |
| 3 | Browsing-only tabs, separate panels/drafts/targets | 3–5, 7 |
| 4 | Find modes, literal/empty replacement, complete Undo/Redo | 1, 4, 8 |
| 5 | Literal prompt insertion, structure, voice/transliteration, history | 1, 3–5, 7–8 |
| 6 | Vault isolation/empty new vault | 2, 4–5, 9 |
| 7 | Confirmed deletion, cancel/failure preservation | 2, 4, 7 |
| 8 | Opening/saving errors and Retry/Stay/Discard | 2, 4–7 |
| 9 | Continuous-typing five-second save start | 4 |
| 10 | Same save rules for every edit; intact Redo | 4, 7–8 |
| 11 | Save-before-screen navigation/no late writes | 4–6 |
| 12 | Copy/Upload/Download and single previous backup | 2, 4, 9 |
| 13 | Native unresponsive close fallback/separate shutdown limit | 6 |
| 14 | Mixed paste/table text/unsupported-only no-op | 4, 8 |
| 15 | Four image formats, offline/clipboard/full copy | 2, 4, 8, 9 |
| 16 | Allowed links and blocked schemes in system browser | 4, 8 |
| 17 | Names/Unicode/exact cross-kind duplicates | 2, 5, 7 |
| 18 | Invalid moves preserve hierarchy | 2, 7 |
| 19 | Configuration transfer/mirror exclude Notes | 9 |
| 20 | Parent Download/backup/later merge failure | 9 |
| 21 | Exact picture boundary/whole mixed rejection | 4, 8 |
| 22 | Exact whole-document limit and responsiveness | 1–2, 4, 7–8, 9 |
| 23 | Link editing/removal/keyboard/persistence/history | 4, 8 |

Conditional block W supplies Windows platform evidence for the earlier owners
when needed. Reuse data/reproduction, not a prior platform's passing status.

Include keyboard-only controls, actual clipboard images, real IME/languages,
window/composer resizing and short layouts, and dictation where configured.
Provider checks remain identified separately from offline Notes acceptance.
Repeat block 6's production close and block 7's deletion-close regression cases.
DOM tests cannot certify native clipboard, CSP, AppKit termination, or launch.

On packaged macOS with the editor focused and multiple history entries, click
Edit ▸ Undo, Redo, and Paste. Undo/Redo must move exactly one editor step,
including native `historyUndo`/`historyRedo` input events. Paste must use the
same validation and whole-action history. Repeat Command+Z, Command+Shift+Z,
and Command+V; keyboard/native handling must execute each once, not twice.

### 5. Measure the near-limit scenario before optimizing

Profile PRD scenario 22 using valid pictures and multilingual text near
**12,582,912 compact UTF-8 JSON bytes**. Record typing/IME responsiveness,
exact whole-document acceptance cost, paste, Replace All, search navigation,
save-start timing, memory, and reopening on both platforms. Include continuous
typing during a slow write; confirm one in-flight save, due-save behavior, and
first-uncovered-edit deadline. Do not call the deadline a measured guarantee
if synchronous work prevents timely request starts.

Only if this scenario shows lag, diagnose the affected path with many-large-
note vault open/switch/maintenance timings and/or shell UI-thread parsing of
large save envelopes. These design-section-15.4 measurements are conditional
diagnostics under the plan, not extra unconditional release gates. Optimize only
a measured bottleneck, preserve exact acceptance and shared fixtures, and rerun
related platform checks. Add no speculative worker/cache/size estimate.

### 6. Finish checks, packages, and cleanup

Run affected frontend/C++/bridge/runtime/native suites, then normal full checks
once after integration. Review final diff for Chat regressions, accidental
ChaWeb Notes work, altered configuration transfer, and unrelated documentation.
Remove experiment scaffolding, duplicate state, unused dependencies/abstractions,
and tests/contracts for abandoned behavior. If block 1 introduced temporary
failure-induction instrumentation, remove it after the checks that need it,
rebuild both packages, and repeat their smoke tests. Check licenses, notices,
bundled CSS, and package smoke tests. No cleanup weakens a required safety path.

## Verification commands and environments

Use repository Node/npm. From `webapp/`:

```sh
npm ci
npm run api-types
npm run check
npm run build
npm run build:chaweb
```

Review generated schema with DTO source, retaining exact dependency lockfile.
From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

Use `make itest-local` on supported non-Windows hosts for local integration.
Run existing native harnesses with required assets/runtime/vault helpers;
ensure Windows test names are included in its runner. Keep macOS bridge harness
results separate from production `main.swift` manual close evidence.

Build local packages on their corresponding hosts:

```sh
make package-macos VERSION=0.0.0-notes-dev
```

```sh
make package-windows VERSION=0.0.0-notes-dev
```

Never use generic `make package`: it publishes to R2 and is outside this work.
Use intended revision/version when preparing a particular acceptance build.

## Operator checkpoints and completion

Provide numbered steps and expected result for every original scenario on each
packaged platform, including the block 1 failure-induction method for each
failure or stall, and required test data. The operator reports every result; fix/repeat failures. Record all earlier
gates and all 23 per-platform scenario outcomes, commands, package/smoke results,
measurements, and provider/platform limitations honestly. An unrun gate remains
pending and this block remains incomplete.

Completion requires plan section 6 in full: approved visuals, tested dependency/
macOS strategy, complete two-platform acceptance, critical regressions intact,
vault/backup compatibility, usable near-limit editing/save timing, required
checks/packages, and final KISS cleanup. Report release readiness only after
these pass. Publishing/deployment still requires separate authorization.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
