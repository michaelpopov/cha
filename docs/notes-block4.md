# Block 4: Editor, autosave, and leave coordination

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 4; plan steps 7–8. Next: [block 5](notes-block5.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **steps 7–8**. Deliver the supported editor model, atomic
transaction acceptance, history boundaries, save scheduling, and the shared
leave procedure. Block 5 must be able to mount these foundations without
introducing another save path. This block does not expose a Notes screen.

## Prerequisites and reading

Complete blocks 1–3. Inspect their changes and test results: continue block 1's
editor feasibility implementation, use block 2's native API and byte-boundary
fixtures, and preserve block 3's prompt contract. Read plan steps 7–8 and design
sections 7–11.1; use PRD sections 4.4–4.7 and 5.2 for behavior.

## Main code paths

- `webapp/src/notes/editor.ts`: schema, acceptance, selection, history, and
  accepted-change notifications.
- `webapp/src/notes/useNoteSave.ts`: the one in-flight save and flush behavior.
- `webapp/src/components/App.tsx`, `webapp/src/useLiveSession.ts`, and
  `webapp/src/state/route.ts`: guard operation entry before side effects.
- App leave state: the Retry / Stay / Discard decision as state and callbacks
  that tests drive. Add no dialog markup here; block 5 renders the approved
  dialog after its approval.
- `webapp/src/styles/app.css`: bundled editor styles needed by the foundation;
  do not expose unapproved visible changes.

## Implementation sequence

### 1. Promote the tested editor configuration

Support doc, paragraph, text, hard break, headings 1–6, nested bullet/ordered
lists with start values, block images, bold, italic, underline, single-color
highlight, and HTTP/HTTPS links. Disable unsupported nodes, arbitrary styles,
Markdown conversion of literal prompt text, and automatic trailing nodes.
Register Link and Underline once and use ProseMirror through `@tiptap/pm`.
Bundle CSS with editor/search CSS injection disabled; retain the packaged CSP.
Until block 8 installs the import pipeline, reject paste and non-file drop
content that contains images, and consume all file drops without insertion.
Also consume empty paste slices (`slice.size === 0`) by returning `true` without
dispatch or fallback, so unsupported-only paste cannot delete selected text.
These guards must be in place before block 5 enables editing. Block 8 retains
the empty-slice guard, adds supported paste fallbacks, and replaces the image
rejection with its image validator. Do not add candidate-document image
validation.

Initialize with content checking and error handling. A schema error must leave
editing and saving disabled even if the library builds fallback content.
Opening must not normalize content, autolink old text, or schedule a save.
Do not revalidate loaded image data or rewrite saved URLs.

### 2. Publish complete actions atomically

Apply the root transaction and synchronous plugin/appended transactions to one
candidate state. Before publication, measure the complete compact JSON as
UTF-8; allow exactly **12,582,912 bytes**, reject one byte over. Include keys,
attributes, escaping, and Base64 images; exclude the transport envelope.

Publish an accepted candidate once with Tiptap's normal transaction, selection,
focus, and update notifications. Notify the save hook once for a changed final
document. Rejection leaves the original document, selection, history, insertion
position, and save serial unchanged; reconcile composition DOM using the
normal view path. Do not dispatch then Undo, apply a transaction twice, patch
private state, or use inserted-slice size estimates.

Put an opening history boundary on each document action and a closing
metadata-only boundary after acceptance. Formatting, links, pictures, paste,
prompt insertion, Replace, and Replace All each change history once. No-op
commands add no event. Retain normal typing grouping. Undo/Redo share exact
size acceptance and saving but receive no boundary that destroys Redo.
The editor owns document/selection/history; React owns no JSON mirror.

### 3. Implement the save state and deadline

Keep only `editSerial`, `savedSerial`, one `inFlight` request with its serial
and promise, one deadline, `saveDue`, a persistent serial-associated error, and
freeze state. Capture editor/context identity in asynchronous continuations.
Read `getJSON()` when making a request; retain no snapshot queue or baseline.

The first uncovered accepted edit starts a five-second deadline. Subsequent
edits do not restart it. At expiry, send the latest document immediately if
idle; otherwise set `saveDue`. Never have two Notes saves in flight.

When a save settles, clear the pending request and record acknowledgement or
failure before examining `saveDue`. Advance `savedSerial` only to the committed
request's serial. Clear an error only when a success covers its failed serial.
If due and `editSerial > savedSerial`, send the current document when unfrozen.
This comparison must also preserve an explicit Retry requested during a failing
pending write. Failure alone must not cause an automatic retry loop.

Retry sends the latest document, or marks it due if a save is pending; cancel
the covered deadline. Keep the unresolved error visible until covered success.
Typing after an error follows normal deadlines. Selection, search, focus,
sidebar tabs, and commands without document changes do not force saves.

### 4. Add one leave boundary before operation side effects

Implement `leaveNoteThen(action)` with one pending navigation intent. Resolve
the editor before loading/creating a note or session, entering Settings/other
screens, returning to start, or switching vaults. Guard `useLiveSession` at
operation entry, including its already-open-session shortcut. Do not guard
only the final view dispatch or switch after session cleanup has begun.

Freeze document changes and Notes submissions, keep selection/copy available,
and cancel the save deadline/due flag. Keep prompt typing and dictation
available. Settle an admitted save. If that write fails during the flush,
stop and offer Retry / Stay / Discard; do not implicitly send another write.
Otherwise submit remaining unacknowledged changes once, then run the action.
A pre-existing failure can receive one fresh final-flush attempt.

Retry repeats resolution explicitly. Stay cancels the continuation, restores
editing, and retains errors. If the abandoned leave canceled an uncovered
deadline/due save, submit those uncovered changes immediately. Discard settles
admitted writes, disposes the editor, clears its active ID/error, and then runs
the action; it never pretends unsaved changes were acknowledged. A failed
destination after save restores the current editor; after Discard it remains
closed. Reject stale continuations before submissions and destination actions.

Ignore competing navigation intents; reserve this same operation slot for
block 7 deletion. Background updates and sidebar browsing need no leave.
Do not add a navigation queue or a new fault-reload save path.

### 5. Handle browser history without a Notes route

Showing Notes and changing notes must not push or replace browser history.
For `Chat A → Chat B → Notes`, Back reaches Chat A. An accepted Back/Forward
opens the reached root/session without pushing another entry.

Retain one current URL in the existing route handling, initialized from
`window.location.href` and synchronized with app URL writes. On each
`popstate`/`hashchange`, capture a local `previousUrl`, then immediately update
the retained value to the reached URL before the guard or any await. On Stay
or an ignored request, restore that request's captured URL with
`history.replaceState(null, '', previousUrl)` and synchronize the retained
value. Never derive the cancellation URL from the displayed panel.

For paired `popstate` and `hashchange`, the second event captures the reached
URL, so ignoring it does not restore the pre-Back address. Keep cancellation
targets local. This restores an address, not a history index; add no stack.

### 6. Provide the points that native close uses

Block 6 implements the complete page-side close handler, including
`deferredCloseAttemptId`, close messages, and close aborts. Here provide only
what that handler uses:

- The single pending-operation slot, and whether a leave holds it.
- One release point after the action and the editor disposal or restore.
- The open state of the Retry / Stay / Discard decision, and the point where a
  leave failure opens it.
- The shared final flush, which close can run with its own continuation.

Add no close messages, deferred attempt ID, page timer, or native shutdown.

## Verification

Use real editor nodes/transactions, fake clocks, and controlled save promises
in the existing frontend suites. Cover:

- Root plus appended autolink acceptance/rejection, IME reconciliation, rejected
  selection/history, and exact boundary parity with core fixtures. Include
  marked-node splitting that duplicates a large link attribute.
- One Undo event per accepted action, no-op/rejected action preserving history,
  ordinary typing grouping, and Redo surviving saves and Undo during a write.
- Continuous edits from t=0 triggering a save at t=5; edits during a slow save
  becoming due; acknowledgement covering only its serial; pending-save Retry;
  visible errors, later edits, and no failure-driven retry loop.
- All leave outcomes, failure of an already-pending write, failed destinations,
  canceled uncovered deadlines, stale callbacks, and ignored second navigation.
- Entry guards for every note/session/screen/vault/history path, including the
  already-open session. Notes from root and session URLs add no route.
- One Back producing both history events: one flush/action, reached URL/view on
  success, exact prior URL restored on Stay, and no second-event rollback.
- Slot release after success, Stay, Discard, and failed destinations, never
  before editor disposal or restore; the decision state opens on leave failure.
- Paste and non-file drop content with images is rejected; file drops are
  consumed without changes.
- Unsupported-only HTML parsed to an empty paste slice over selected text leaves
  content, selection, history, and save serial unchanged, without dispatch or
  fallback.

Run affected Vitest suites and typecheck during development, then the frontend
check/build commands in plan section 5. No native-close claim is made here.

## Completion and handoff

The supported schema, exact atomic acceptance, isolated history, save rules,
and all ordinary leave paths have passing focused tests. Record adapter/version
choices, fixtures, guarded entry points, test evidence, and the slot, release,
decision, and flush points for block 6. Block 5 is next; it must reuse these paths before enabling editing.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
