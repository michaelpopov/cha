# Block 5: Basic Notes screen

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 5; plan step 10. Next: [block 6](notes-block6.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **step 10**. Connect the approved navigation, metadata tree,
create/open workflow, editor, loading/errors, and shared Notes prompt. The user
can type, save, leave, and reopen notes through the real native stack. This is
a development build; block 6 has not yet established normal-close safety.
Organization beyond create, formatting commands, prompt insertion, import,
links, and Find/Replace are delivered by later blocks.

## Prerequisites and reading

Complete blocks 1–4 and inspect their handoffs and code. Require explicit
approval for this block's visible scope: navigation, tree/create controls,
editor/composer layout, states, labels, keyboard behavior, resizing, the save
error, and the Retry / Stay / Discard dialog. Read
plan step 10, design sections 3, 4.1–4.2, and 13, and PRD sections 3, 4.1–4.2,
4.7, and 5. Carry forward the recorded composer/Direct tools decisions; obtain
approval if the proposal changes.

## Main code paths

Use `App.tsx`, `state/view.ts`, and `Sidebar.tsx`, with new
`components/NotesScreen.tsx` and `components/NotesTree.tsx`. Reuse
`PromptInput.tsx`, `notes/editor.ts`, `notes/useNoteSave.ts`, native Notes
client methods, existing dialog conventions, and `styles/app.css`.

## Implementation sequence

### 1. Separate browsing from the displayed panel

Add `mainView: 'notes'`, `sidebarTab: 'chat' | 'notes'`, and
`activeNoteId: NoteId | null`. Keep the active Chat conversation independently.
Switching sidebar tabs changes browsing only. A primary note/session action
changes the main panel through the shared leave guard; moving tree focus does
not open a note. Do not add a Notes URL or persist a sidebar preference.

Keep the current-vault Notes tree across sidebar tab changes. Dispose editor,
tree, dialogs, import origins, and Notes prompt instance on database context
replacement, even when the replacement contains the same numeric note ID.
Use epoch and editor/request identity for stale work; clear drafts only after
a successful user vault switch, not every maintenance epoch update.

### 2. Load and render metadata

Load `notes.list` lazily on first Notes browsing. Derive an ID map and child
arrays from its single metadata listing for `syncDataLoaderFeature` and
`hotkeysCoreFeature`. Use a synthetic `root` for null parent and decimal string
IDs for stored rows. Identify folders by kind, including empty folders.

Sort each child group by folders first, `localeCompare(name)` next, and exact
lexical comparison when locale comparison ties. Uniqueness remains the core's
exact case-sensitive rule. Preserve Headless Tree refs/events through composed
`getContainerProps()`/`getProps()`; add no selection store or drag/drop feature.

Render tree ARIA, one-based levels, and folder expansion; never give a note
`aria-expanded`. Up/Down move focus, Left/Right move/collapse/expand, Enter
activates. Mark the open note independently with active-row styling and
`aria-current`. Failed listing retains old rows with Error/Retry; it must not
look like an empty tree. Reject stale listing responses.

### 3. Create notes and folders

Use the approved name/destination dialog. Initial parent is the focused folder,
the focused note's parent, or Top level. The editable native select lists Top
level and full folder paths, distinguishing duplicate leaf names. Bind the
dialog/request to its captured vault context.

Creating a note resolves the old editor before `notes.create`; Stay creates
nothing. Create a folder without forcing a document leave/save. Serialize
mutation and refresh, then reload metadata and rebuild. Expand destination
ancestors and focus/reveal the row with the tree's own focus API. Open a new
note through the normal load path. On committed-create/refresh failure, retry
the listing without repeating creation; mark retained metadata stale before
allowing another mutation. Keep errors in the dialog and avoid optimistic rows.

### 4. Open exactly one editor safely

Capture note/context identity, resolve any old editor, then call `notes.get`.
Loading disables editor commands and submission. Reject obsolete replies and
initialization failures; show Retry opening without an editable blank fallback.
A valid already-open note just receives focus, retaining history.

Initialize schema content checks, save serials, and end-of-document selection:
end of the last text block or a gap cursor after a final image. Connect accepted
changes to autosave and leave resolution **before enabling editing**. Opening
itself changes neither history nor save serials. Derive the active full path
for `Send to <note path>` accessible name and keyboard-accessible tooltip.

Render the approved persistent save error with Retry, and the approved Retry /
Stay / Discard dialog for block 4's decision state. Use the existing dialog
conventions. Default focus and Escape mean Stay.

### 5. Mount the shared composer and approved layout

Mount Notes `PromptInput` with `key={activeNoteId}`, using App's single Notes
draft. Key changes clean up old voice/submission work without clearing that
draft. Preserve the Chat draft for return to the same conversation and reset
it only on successful conversation change. With no open note, retain the Notes
draft and disable submission.

The prompt insertion handler is completed in block 7. Until then do not enable
an action that can claim success, clear a draft, or send a chat request without
an accepted note insertion. Reuse the typed-entry, voice, and transliteration
contract; typed input must not depend on voice configuration or a live session.

Preserve the composer surface, resize behavior, microphone, circular Send, and
transliteration positions. Omit Notes-inapplicable forum/From/To/target context,
speaker, and session-picture controls. Add no panel title, destination legend,
Clear button, helper prose, or chat history. Keep both document and prompt
available in short windows, allow container scrolling, clamp prompt resizing,
and wrap toolbar controls in order at narrow widths. Use the current sidebar
toggle, not another mobile layout.

## Verification

Extend existing App/view/component tests for browsing-only tab switches,
independent main panel/Chat state, tree order/keyboard/ARIA, root and nested
creation, canceled leave before creation, loading failures, stale replies,
refresh retry, and reactivation without resetting history. Verify no-note draft
retention, disabled submission, same-ID vault replacement, and late voice work.
Test ordinary editing, five-second saves, error Retry, save-before-note/session/
Settings navigation, the rendered Retry / Stay / Discard dialog with its focus
and Escape behavior, and retained drafts. Run targeted suites, frontend check,
and both frontend builds from plan section 5.

## Operator checkpoint

Build the native development app/local platform package. Provide its revision,
OS/browser versions, disposable vault setup, and numbered instructions. Require
the operator to report each result:

1. Browse Chat/Notes tabs; verify the main panel and drafts stay unchanged.
2. Create a root note, folder, subfolder, and nested note; verify order, keyboard
   focus, and the chosen destination.
3. Edit text and leave for another note, the same Chat conversation, and Settings;
   reopen and verify saved text and each panel's retained draft.
4. Test empty, loading, opening-error, and saving-error states, and Retry,
   Stay, and Discard after a failed save on leave. Use the failure-induction
   methods recorded in block 1 and confirm that each one works. A failed load
   must not become an editable empty note.
5. Resize the composer and test narrow/short windows, scrolling, focus, and
   accessible controls. No action or prompt may disappear under the document.

Do not count ordinary close here as verified; use block 6's real save/close
checks before relying on this editable build for normal use.

## Completion and handoff

Automated checks and the operator's basic-screen results pass on the required
platforms. Record approved scope, lifecycle connections, build revision,
observed layout, and any pending Windows items. Hand off the real editable
screen and reproduction steps to block 6.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
