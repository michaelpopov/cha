# Block 1: Baseline, UI approval, and editor feasibility

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 1; plan steps 1–2. Next: [block 2](notes-block2.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Execution order and source mapping

The file names use the split's block names. Run the blocks in the order below.

| Block file | Plan steps | Result |
| --- | --- | --- |
| [notes-block1.md](notes-block1.md) | 1–2 | Baseline, visual approval, editor feasibility |
| [notes-block2.md](notes-block2.md) | 4–5 | Storage and native API |
| [notes-block3.md](notes-block3.md) | 6 | Shared prompt and independent drafts |
| [notes-block4.md](notes-block4.md) | 7–8 | Editor, autosave, leave coordination |
| [notes-block5.md](notes-block5.md) | 10 | Basic Notes screen |
| [notes-block6.md](notes-block6.md) | 3, then 9 | macOS termination selection and native close |
| [notes-block7.md](notes-block7.md) | 11–12 | Organization, formatting, prompt insertion |
| [notes-block8.md](notes-block8.md) | 13–14 | Import, links, Find/Replace |
| [notes-blockW.md](notes-blockW.md) | Windows parts of 2, 9, 11, 13, 14 | Conditional Windows completion |
| [notes-block9.md](notes-block9.md) | 15–16 | Vault integration and final verification |

With Windows available throughout, skip block W because its work is completed
in the earlier owning blocks. Otherwise block W is mandatory before block 9.

## Outcome and scope

Implement plan **steps 1–2**. Establish the current build/test baseline, record
the approved or pending status of all remaining visual proposals, select and
test exact dependencies, and prove the editor/history/insertion/search/tree
behavior needed by later blocks.
No production Notes screen or native-close implementation is delivered here.

## Prerequisites and reading

Read `AGENTS.md`, the full implementation plan and split, PRD sections 3–9,
and design sections 1–3, 7–9, 13–15. Read architecture as background only;
plan/design corrections supersede it. Inspect the actual repository before
assuming a proposed module or dependency exists.

## Main code paths

Inspect `webapp/package.json`, lockfile and `.node-version`, App/Sidebar/
ChatScreen, prompt/voice tests, state/route, native client/bridge, both shell
entry points, existing native harnesses, and database/store ownership. Useful
feasibility code should become `webapp/src/notes/editor.ts` and focused tests
rather than a throwaway production subsystem.

## Implementation sequence

### 1. Record baseline and platform mode

Record starting revision and `git status`; preserve unrelated changes. Use
Node/npm versions declared by the repository. Run the baseline checks below,
recording exact commands, failures, and environment limitations separately from
new failures. Do not repair unrelated problems without identifying their scope.
Use plan section 4's mapping of all **23 PRD scenarios** as the acceptance
checklist; retain the full original scenario text for later execution.

Record one Windows mode for the run:

- Windows available: executor/operator can build and exercise WebView2 during
  blocks 2–8; perform the Windows portions in their owning blocks.
- Windows not available: complete macOS/shared work first, record every pending
  Windows item, then run conditional block W before final verification. Do not
  write Windows-only code that cannot be built/tested in its current block.

### 2. Present the remaining visible design

Carry forward the recorded approval of the Direct tools toolbar and preservation
of the existing composer/control positions. Technical design approval is not
approval of every screen. Prepare visual representations for the remaining
scope from design sections 3 and 14:

- Chat/Notes navigation, tree focus versus open-note marker, create actions,
  destinations, keyboard row menus, and create/rename/move/delete dialogs.
- Editor/prompt layout, no-note/loading/error states, wrapping and independent
  scrolling, narrow/short windows, composer resize bounds, and focus behavior.
- Find/Replace bar, labels/options/results and shortcut scope; picture picker,
  link context/toolbar actions, URL dialog, and actionable import errors.
- View-only inactive caret/text/image selection marker and save/open errors,
  Retry / Stay / Discard, and native Cancel close / Close anyway.
- Download and preliminary Parent merge confirmation, including applicable
  ChaWeb controls, replacement of local notes/folders, and single `.bac` scope.

Use existing CHA styling, spacing, buttons, dialogs, and focus conventions.
Add no panel title/legend, helper narration, or composer redesign. Hide only the
Notes-inapplicable Chat context, target, speaker, and session-picture controls.
Preserve microphone, Send, transliteration, and their placement. Record exactly
what the user approves and any still-pending visuals in the handoff.

Wait for explicit approval before implementing a visible proposal. Backend/
nonvisible tests may proceed while approval is pending; the temporary packaged
feasibility build below needs no production UI approval. Present the complete
proposal set at this block's checkpoint and record each answer. A proposal that
is still pending does not prevent completion of block 1; it is a gate for the
block that implements it. Each later block lists the approvals it needs in its
prerequisites. Re-present changed proposals before later UI work.

### 3. Verify and pin the dependency set

Start with design candidates Tiptap `3.30.3` and Headless Tree `1.7.0`; these
are candidates, not certified pins. Verify published compatible matching
packages, React compatibility, licenses/notices, and both embedded-browser
requirements before adoption. Install exact versions and retain the lockfile.

The candidate direct Tiptap set is core, react, pm, starter-kit, highlight,
image, and find-and-replace; use Headless Tree core/react. Use ProseMirror via
`@tiptap/pm`. If Link or Underline needs a direct import, disable the corresponding
StarterKit extension so each is registered once. Do not install a UI generator,
the supplied search-panel template, its dependencies, or unrelated extensions.

### 4. Prove atomic editor acceptance and history

Use real ProseMirror nodes/selections and pinned Tiptap dispatch. Demonstrate
that a root action plus synchronous appended actions (especially autolinking)
can be applied to one candidate and accepted/rejected before state publication.
Check full compact UTF-8 JSON size, including attributes/escaping. A rejected
action must preserve content, selection, history, and save notification state.
Include IME DOM reconciliation. Accepted formatting/import/insertion/replacement
must produce one isolated Undo event without adjacent typing. Saving must leave
Redo intact.

Prefer supported hooks. If a narrow dispatch adapter is required, preserve
Tiptap notifications and document its upstream version. Do not repair a
published action with Undo, patch private state, add a generic framework, or
claim separate per-transaction filtering proves whole-action rejection.
Resolve a failed assumption before dependent editor implementation.

### 5. Prove insertion, search, and tree integration

Implement/test design section 8's insertion command against actual nodes:
empty paragraphs, block starts/ends, paragraph/heading interiors, nested lists,
cross-block and cross-item selections, image node/gap selections, preserved
marked fragments, literal multiline text, final URLs, and repeated submissions.
The new paragraph ends with the selection before original trailing content;
one Undo restores the whole old structure. No stale DOM Range or custom
selection type is needed. Retain useful production command code for block 7.

Verify headless search literal replacement (including `$1`), empty deletion,
case/whole-word/RE2 behavior, unsupported regex, boundaries, and original-match
Replace All in one history event. Prove required behavior rather than copying
the supplied panel's defaults.

Verify Headless Tree synchronous loading, keyboard focus/activation, rebuild,
empty folders, and composed props using only `syncDataLoaderFeature` and
`hotkeysCoreFeature`. Do not add selection, drag/drop, or inline rename features.

### 6. Exercise packaged CSP and rendering early

Create a temporary packaged editor build with bundled CSS, editor and search
`injectCSS: false`, and the actual shell CSP. Check whitespace, supported
highlighting, selection/decorations, and representative clipboard content.
Do not relax `style-src 'self'` or other CSP to make a test pass. Exercise
WKWebView and WebView2 when available; defer WebView2 to block W otherwise.
A later incompatible WebView2 result is a blocker, not permission to skip it.
Remove disposable scaffolding, retaining useful editor modules/tests.

### 7. Select the failure-induction methods

Later operator checkpoints need failures and delays in packaged builds. Select
and record one method for each condition before block 5, so that no later block
invents its own test hook:

- A save, open, metadata refresh, or delete request that fails.
- A save that completes late, while a close attempt waits or after Cancel close.
- A save, final flush, or delete that does not complete within the 15-second
  native close wait, including a hung Retry.
- A connected page that does not respond to prepare-close.
- Close with no runtime, and close with no page connection.

Inspect the actual code before selecting. Prefer external methods that need no
code change, for example a SQLite lock held by another process on a plaintext
disposable vault, or a JavaScript debugger pause in the page. Record the limits
of each method; for example, the connection's SQLite busy timeout limits how
long a lock can delay a request before it fails.

If a condition needs temporary test instrumentation, use the smallest
nonvisible change within the authorized scope without a separate approval.
Visible changes, such as exposing inspector controls in local development
packages, still require a visual proposal and explicit approval before
implementation. Obtain authorization for work outside the agreed scope.
Record the method and temporary changes in the handoff; block 9 removes them
after the checks that need them. If no usable method exists or required approval
is pending, the check stays pending; it is never reported as passed. Block 5 is
the first block that uses these methods; it confirms that each one works.

## Verification commands

From `webapp/`, after selecting repository Node/npm:

```sh
npm ci
npm run api-types
npm run check
npm run build
npm run build:chaweb
```

Review generated DTO differences; do not accept unexplained baseline drift.
From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

Run focused feasibility tests as they are added. Use `make itest-local` on a
supported non-Windows host when applicable. Keep provider/R2 checks separate
and disposable. Build local packages only with the platform-specific commands
in plan section 5, never the publishing `make package` target.

## Operator checkpoints

Finish automated preparation first, then provide the visual proposal set and
record individual explicit approvals or pending items. Describe the selected
failure-induction methods and temporary instrumentation; request approval only
for visible changes or work outside the authorized scope. For the CSP
experiment, provide packaged
revision, dependency pins, OS/browser versions, numbered steps/expected results,
and how to produce test clipboard content. The operator reports each result;
unrun interaction is pending. Resolve failures and repeat affected checks.
Do not run the macOS termination experiment here; the split assigns it to
block 6 immediately before native-close implementation.

## Completion and handoff

Record baseline failures, Windows mode, approved and pending visible scope,
exact package pins/licenses, editor hook/history/insertion/search/tree
evidence, packaged CSP results, the selected failure-induction methods, and
outstanding environment gates. Dependency/atomic feasibility must be resolved
before broad editor work. Block 1 is complete when all visual proposals were
presented and each approval or pending item is recorded; pending approvals
gate only the blocks that implement them. Hand useful modules/fixtures to
block 4; block 2 is next.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.

COMPLETED
