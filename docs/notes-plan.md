# Notes: implementation plan

Status: reviewed technical guidance; implementation and verification remain
pending.

This plan covers the first Notes release in the macOS and Windows standalone
applications. It uses the [PRD](notes-prd.md), the
[architecture](notes-arch.md), and the reviewed
[detailed design](notes-design.md). The PRD defines acceptance. Follow the
detailed design's corrections to the architecture, including the native
Retry ordering in design section 12.2. Report a requirement that cannot be
met instead of silently changing it.

## 1. Scope and working rules

Use one Notes table in the existing vault database, one active editor, one
metadata listing, one Chat draft, and one Notes draft. Reuse the current
native bridge, database locks, vault operations, composer, voice input, and
transliteration. Build small changes that can be reviewed and tested in order.

Do not add document revisions, document caches, persistent drafts, a separate
picture store, a worker, an outbox, a Notes event stream, HTTP Notes routes,
or configuration for hypothetical needs. Keep Notes out of chat context and
out of the ChaWeb editor. The required ChaWeb change is limited to applicable
vault replacement confirmation copy. Ignore unused configuration and log a
warning through the existing path.

The [repository instructions](../AGENTS.md) require:

> Before making any UI change, present a visual representation of the proposed
> change to the user.

They also require explicit approval before implementation. Preserve the
approvals recorded in design sections 1 and 3; obtain approval only for the
remaining or changed visible design. Technical approval of this plan does not
approve a screen. Backend work and tests without visible changes can proceed
while remaining UI approval is pending. Do not add panel title legends or
nonessential helper text.

Each step below has an output and a completion check. Add behavior tests with
the implementation they protect. Use existing test suites and disposable
vaults. Do not build a separate test framework or add tests that only repeat
private implementation details.

Record implementation progress, approval scope, feasibility decisions, test
commands/results, measurements, and limitations in the relevant implementation
commit message. If no commit is created, include them in the task response.
Instructions below to record implementation evidence use that location outside
`docs/`. The checkboxes specify required work; they are not a progress log.
Implementing the feature does not authorize updating this plan, ticking its
checkboxes, or editing other files under `docs/`. Those edits require an
explicit user request.

## 2. Delivery order

The order below keeps dependency risks early and puts saving and navigation
protection in place before connecting the editable Notes screen. Native close
verification follows that screen so it can exercise real unsaved notes. Step
numbers identify work items; the delivery order is 1–8, 10, 9, then 11–16.
Dependencies refer to completed technical work; UI approval additionally
applies wherever visible controls, labels, styling, or dialogs change.

| Step | Output | Depends on |
| --- | --- | --- |
| 1 | Baseline, acceptance checklist, and remaining visual approvals | Source documents |
| 2 | Tested dependency set and editor/tree feasibility results | 1 (technical baseline only) |
| 3 | Selected macOS termination path | 1 |
| 4 | Notes schema, storage operations, and compatibility tests | 1 |
| 5 | Application admission, DTOs, and native RPCs | 4 |
| 6 | Shared prompt input and App-owned drafts | 1 |
| 7 | Supported editor schema, atomic acceptance, and history | 2, 4 |
| 8 | Autosave and shared leave coordination | 5, 7 |
| 10 | Notes navigation, tree browsing, create/open, and screen | 5–8 and relevant UI approval |
| 9 | Native close negotiation on both platforms | 3, 8, 10 |
| 11 | Rename, move, deletion, and metadata recovery | 10; 9 for its close items on each platform |
| 12 | Formatting and prompt-to-note editing | 6, 7, 10 |
| 13 | Pictures, clipboard import, and web links | 7, 10 |
| 14 | Find and replace controls | 7, 10 |
| 15 | Vault transfer behavior and replacement confirmations | 4, 8, 10, 11 |
| 16 | Packaged acceptance, performance, and final cleanup | All earlier steps |

Use these as reviewable work units, not as a requirement for sixteen commits
or new abstractions. Integrate related changes together when that avoids a
temporary unsafe path. Do not release an editable Notes build until its save,
navigation, deletion, and native close checks pass.

Implement and verify step 9 on each platform when that environment is
available. Pending Windows close work does not block steps 11–15 on macOS;
both platforms must pass step 9 before release.

## 3. Implementation steps

### Step 1. Establish the baseline and finish visual approval

References: PRD sections 3–7; design sections 1–3 and 15.1.

- [ ] Read the current repository instructions and preserve unrelated work.
  Record the starting revision and any existing test failures.
- [ ] Run the frontend and C++ baseline checks listed in section 5 of this
  plan. Confirm access to both packaged desktop environments for later tests.
- [ ] Use the 23 PRD scenarios as the release checklist. Use section 4 below
  to associate each scenario with its implementation steps.
- [ ] Carry forward the recorded Direct tools toolbar and existing composer
  decisions. Present the remaining visual proposals from design sections 3
  and 14: navigation and tree controls, dialogs, find bar, link/picture
  controls, inactive selection marker, errors, and replacement confirmations.
- [ ] Include narrow/short window behavior, keyboard access, and the native
  timeout dialog in that review. Resolve any changed proposal before coding
  the affected visible behavior.
- [ ] Record the approval scope and later feasibility results using the
  reporting rule in section 1. Do not report an experiment as passed because
  its acceptance criteria are written down.

Completion: the baseline is known, approved UI scope is identifiable, and
unapproved visible work has a clear gate. Independent technical work can
continue while that gate is pending.

### Step 2. Verify dependencies and the editor/tree integration

References: design sections 7, 8, 9.5, 13, and 15.1.

Main files: `webapp/package.json`, `webapp/package-lock.json`, focused editor
tests, and the existing native test harnesses.

- [ ] Start with the candidate versions in the design, then verify the
  published matching Tiptap packages and Headless Tree packages. Install exact
  versions, retain the lockfile, check licenses, and retain required notices.
- [ ] Verify the existing React version and both embedded browsers. Use
  ProseMirror through `@tiptap/pm`; register Link and Underline only once.
  Do not install a Tiptap UI generator or its supplied panel dependencies.
- [ ] In focused tests, prove that a root action and its appended transactions
  can be accepted or rejected together before publishing editor state. Include
  appended autolinks, IME reconciliation, unchanged selection/history on
  rejection, and one Undo event per accepted action.
- [ ] Prove the prompt insertion cases in design section 8 against real
  ProseMirror nodes and selections. Include headings, list boundaries,
  cross-block/cross-item selections, images, and successive submissions.
- [ ] Verify the headless search extension's literal replacement, empty
  replacement, RE2 restrictions, whole-word behavior, and Replace All history.
- [ ] Verify tree focus, keyboard activation, synchronous loading, and rebuild
  behavior using only the selected features.
- [ ] Run an early editor check in a temporary packaged test build with
  bundled CSS, `injectCSS: false`, and the actual CSP. This check needs no UI
  approval and does not ship a UI change. Exercise whitespace, highlights,
  selection, and representative clipboard content.
- [ ] Record the exact versions and results. If atomic editing or required
  commands cannot be made reliable, resolve that issue before broad editor
  integration. Retain useful production code/tests from the experiment and
  remove disposable scaffolding.

Completion: the dependency set and necessary editor hooks have evidence;
they are no longer assumptions based on an upstream manifest or demo.

### Step 3. Select the macOS termination implementation

References: design section 12.3.

Main files: `packaging/macos/main.swift`, `native_bridge.swift`, and
`tests/native/macos/`.

- [ ] In a packaged experiment, return `.terminateLater` while keeping the
  runtime and bridge attached.
- [ ] Verify page-to-host messages, `callAsyncJavaScript`, a complete real
  page/core RPC and reply, response-timer delivery, and cancellation restoring
  normal interaction while termination is pending.
- [ ] Record macOS/WebKit versions, the tested timer run-loop mode, and results.
- [ ] Select the passing `.terminateLater` path or the design's
  `.terminateCancel`/normal-run-loop path with a one-use termination flag.
  Record the latter's logout/restart limitation if it is required.
- [ ] Keep only the selected implementation path. Do not add runtime fallback
  detection or claim a normal development-browser test proves AppKit behavior.

Completion: step 9 has one supported macOS strategy. A missing macOS result
remains an explicit implementation gate, not an assumed pass.

### Step 4. Add the database schema and storage operations

References: design section 5.

Main files: new `src/storage/note_storage.{h,cpp}`,
`src/storage/workspace_session_database.cpp`,
`src/storage/session_label.{h,cpp}`,
`src/workspace/workspace_config_store.{h,cpp}`, and `CMakeLists.txt`.

- [ ] Implement the design's strict `note_items` table, parent foreign key,
  exact sibling-name unique index, and parent index. Allocate positive,
  JavaScript-safe, non-reused IDs with SQLite `AUTOINCREMENT`.
- [ ] Use shared schema creation for new databases, store open, and maintenance
  reopen after identity checks. Let the caller own each transaction; avoid
  nested transactions. Preserve schema-2 identity, schema-1 import behavior,
  and existing integrity checks without scanning note JSON at startup.
- [ ] Reuse session-label validation with Notes-specific field/error wording.
  Preserve spelling and existing session errors. Enforce valid UTF-8, the
  200-code-point limit, and the specified whitespace/control restrictions.
- [ ] Implement metadata-only list; create folder or valid empty note; get;
  update-only save; rename; move; and recursive deletion through the foreign
  key. Rename/move must not rewrite note bodies.
- [ ] In each mutation transaction, validate parents, source IDs, cross-kind
  sibling uniqueness, and move cycles. Use the ancestor query from the design;
  root moves need no ancestry query. Allow unchanged rename/move as no-ops.
- [ ] For get/save, enforce object JSON and the compact UTF-8 12 MiB limit.
  Require a committed update of an existing note before reporting success.
  Leave editor nodes, image bytes, and URL policies to the frontend.
- [ ] Add store forwarding under its existing mutex and database ownership.
  Keep Notes outside configuration revision, undo, and snapshot publication.
- [ ] Register sources and meaningful storage tests with the existing targets.
  Test rollback, root duplicates, case distinction, Unicode boundaries,
  missing/non-folder parents, cycles, subtree deletion, deleted-ID non-reuse,
  corrupt JSON, and save-to-deleted-ID failure.
- [ ] Test new/old and plain/protected vaults, maintenance reopen, and an early
  full-copy round trip with embedded image data and the ID sequence intact.

Completion: storage tests show atomic mutations and existing vault
compatibility. Notes do not introduce a second connection or migration system.

### Step 5. Add Application and native bridge contracts

References: design section 6.

Main files: `resources/dto.yaml`, `src/app/application.*`,
`src/bridge/bridge_protocol.*`, `operation_dispatch.h`, new
`src/bridge/notes_dispatch.cpp`, and `webapp/src/api/`.

- [ ] Define the item, listing, document, and content DTOs from the design;
  regenerate `webapp/src/api/schema.d.ts` with the existing generator.
- [ ] Register `notes.list`, `notes.create`, `notes.get`, `notes.save`,
  `notes.rename`, `notes.move`, and `notes.delete` as ordinary native methods.
  Keep the document an object in the request, not an encoded JSON string.
- [ ] Add small Application methods that admit the supplied context epoch
  under the lifecycle mutex, then enter the store. Preserve lock order.
- [ ] Validate request shapes, safe IDs, kind, and nullable parents in the
  thin dispatcher. Preserve connection checks, protocol version, the 16 MiB
  transport limit, and existing ordinary-request queue behavior.
- [ ] Return specific public Notes errors, including missing item, invalid
  name/parent/cycle, document size, and opening/saving failure. Log technical
  failures without note content. Success means commit, not queue admission.
- [ ] Add typed native client methods and response guards. Update client mocks,
  wire fixtures, and method-policy fixtures. Keep ChaWeb free of Notes routes.
- [ ] Test stale epochs, maintenance/stopping admission, malformed requests and
  replies, missing rows, size boundaries, and full-document retry behavior in
  the existing application/bridge/native client suites.

Completion: all seven operations work through the trusted native transport,
and old-context requests cannot mutate the active vault.

### Step 6. Extract shared prompt input and lift drafts

References: design sections 4.1 and 4.3.

Main files: `ChatScreen.tsx`, new `components/PromptInput.tsx`, `App.tsx`, and
the current voice/transliteration tests.

Use this contract and its lifecycle rules before extracting markup or handlers
from the existing composer. Design section 4.3 references this component
contract; the types below define its frontend props.

```ts
interface PromptVoiceControls {
  available: boolean;
  state: 'idle' | 'starting' | 'recording' | 'finishing';
  toggle(): Promise<void>;
}

interface PromptInputProps {
  value: string;
  onChange(next: string): void;
  enabled: boolean;
  submitEnabled: boolean;
  microphoneEnabled: boolean;
  inputLabel: string;
  placeholder?: string;
  submitLabel: string;
  client: ChaClient; // Existing voice runtime/transport access only.
  onSubmit(text: string): Promise<{clear_input: boolean}>;
  onSubmittingChange?(submitting: boolean): void;
  onError(message: string | null): void;
  stopAction?: ReactElement;
  formControls?: ReactNode;
  statusRow: (controls: {
    transliteration: ReactNode;
    voice: PromptVoiceControls;
  }) => ReactNode;
}
```

`enabled` controls text entry, independently of Send and microphone availability.
Chat passes `enabled = sessionAvailable` and supplies its connection/action
conditions through the other availability props. The component adds its existing
empty-input, voice-transition, and submission-in-progress checks. Notes gates
submission on the editable note state. During a leave freeze, keep `enabled`
true and `microphoneEnabled` unchanged; block only `submitEnabled`, so dictation
continues after Stay. Missing voice configuration disables only voice input.

`formControls` renders inside `cha-composer-controls`, before the microphone
and action buttons. Chat supplies its target selector with its existing
availability conditions. Notes omits this slot.
`statusRow` renders the complete existing row below the form, so Chat owns the
forum, From/To, speaker, picture, and sidebar content and their order. The
component supplies the bound transliteration control for placement in that
row. Notes supplies its approved row without Chat context or controls. The
narrow voice controls preserve Chat's existing speaker/dictation coupling in
`toggleAutoAudio`; the shared component does not learn audio-response policy.

Chat connects `onSubmittingChange` to its existing `sendPending` state so
target selection and transcript cover/delete actions remain disabled throughout
transcription finalization and submission. Notify at submission start and end,
release it during unmount cleanup, and ignore late completions from the old
instance. `onError` routes prompt/voice errors and error clearing to the
parent's existing error display; Chat keeps its current `actionError` row.

Chat supplies `stopAction` as its existing Stop button, with its disabled
state, handler, pending/error handling, and styling. While it is present,
PromptInput replaces Send with that button and blocks submission through the
form and Enter. It delays a detected voice phrase until `stopAction` is removed,
as today: retain the existing pending spoken-send state, then apply the existing
availability checks and delay. Stop uses `type="button"` and its own
availability; it must still work when sending is unavailable. Draft editing
and dictation keep their existing independent behavior. Notes omits Stop.
No session snapshot, target list, generation state, or Chat/Notes mode flag
belongs in PromptInput.

Keep the current synchronous `draftRef` pattern inside PromptInput. Initialize
it from `value`, synchronize it when an external `value` change commits, and
route every local edit through one `updateDraft(next)` that updates the ref
before calling `onChange(next)`. Transcription callbacks, send-phrase removal,
Ctrl+Enter, and reads after `await` must use that ref, not a captured `value`.
After accepted submission, read the current ref and remove only the matching
submitted prefix using the existing whitespace rule; otherwise leave the draft
intact. This ref is the existing async-read mirror, not another draft store.

Mount the Notes instance with `key={activeNoteId}` and keep its draft in App.
The key is a React lifecycle boundary, not a PromptInput prop. Reuse the current
unmount cleanup to abort voice startup, cancel transcription and the pending
voice-send timer, and invalidate submission continuations with the existing
composer/voice attempt checks. Check those guards after awaits and before
calling the captured submission handler or changing the draft. Remounting must
not clear the App-owned draft: remove the old mount/conversation effect's
`updateDraft('')` from the shared component. Vault replacement disposes the
Notes subtree even if the new vault contains the same numeric note ID.
Cleanup ignores late results; it cannot undo an already accepted insertion or
cancel an already admitted request. No new note-ID invalidation protocol is
needed inside the component.

PromptInput cancels voice input when `enabled` changes, as Chat does today for
`sessionAvailable`. Reuse the voice cleanup to abort startup, cancel recording,
clear pending spoken-send state/timers, and ignore stale transcription callbacks
without clearing the App-owned draft.

- [ ] Extract controlled text entry, resize behavior, transliteration, voice
  lifecycle, and serialized submission using the current composer markup.
  Keep Chat commands, target selection, and generation rules in ChatScreen.
- [ ] Implement the contract above, retaining the target selector inside the
  form, the status row's current order, and Chat's Send/Stop behavior. Keep
  the current Chat labels/placeholder and Notes destination accessible name.
- [ ] Move the two draft slots into App. Key the single Chat slot by vault,
  forum, and session. Reset it only on successful conversation change or
  successful vault switch; failed opens must retain it.
- [ ] Keep one Notes draft across note/panel changes. Clear drafts on a
  successful vault switch, not on an attempted switch or every maintenance
  epoch change. Do not add per-note or per-conversation draft collections.
- [ ] Preserve Enter, Ctrl+Enter, IME, and submitted-prefix clearing. An
  insertion failure retains the draft; text arriving during submission stays.
  Use the synchronized ref for every async draft read and local update.
- [ ] Preserve spoken submission while listening, manual submission after
  finishing transcription, microphone-stop without submission, send-phrase
  removal, and existing cleanup/stale-callback checks on panel or note change.
  Use the keyed Notes mount above instead of adding note identity props.
- [ ] Verify existing Chat behavior before connecting Notes. Include commands,
  addressing, draft editing during async work, voice failures, transliteration,
  resizing, target/cover/delete guards during pending submission, error display,
  Stop while sending is unavailable, and the speaker's dictation behavior.
  Extend the current `LiveChat.test.tsx` coverage and focused PromptInput tests.
- [ ] Test a spoken send phrase detected during Chat generation: it stays
  pending without submitting while `stopAction` is present. After generation
  ends and `stopAction` is removed, advance the existing delay and verify one
  submission with the phrase omitted and recording still active.
- [ ] Add one Chat regression test: start recording with a pending spoken-send
  phrase, make `sessionAvailable` false without changing the conversation/vault
  key, then restore availability and advance past the spoken-send delay.
  Verify recording was canceled, late transcription is ignored, the draft is
  retained, and the pending phrase never submits when the session returns.
- [ ] Test deferred transcription and submission with interleaved typing and
  speech: preserve later text and clear only the accepted prefix. Remount from
  note A to B with both a pending spoken-send timer and an unresolved submission;
  late A work must neither insert into B nor clear its shared Notes draft.
  Also verify remount retains that draft and typing works without voice setup.

Completion: Chat regression tests pass, and Notes can supply its own handler
without depending on a live chat or configured voice provider for typed input.

### Step 7. Build the editor foundation and acceptance boundary

References: design sections 7–9.

Main files: new `webapp/src/notes/editor.ts` and `webapp/src/styles/app.css`.

Follow design section 7.3's size-only transaction boundary. Step 13 implements
the entry-point image checks described in design sections 9.1–9.2.

- [ ] Promote the tested configuration from step 2: paragraphs, text, hard
  breaks, heading levels 1–6, nested lists, block images, and the approved marks.
  Disable unsupported nodes/styles and automatic trailing-node creation.
- [ ] Configure a shared HTTP/HTTPS link predicate and single-color highlight.
  Bundle required editor/search CSS with CSS injection disabled. Keep CSP
  unchanged. Prevent unchecked image or external drop paths from bypassing
  the entry-point checks in step 13.
- [ ] Initialize with schema content checking and block editing/saving after
  an initialization error. Do not save stripped or fallback content. Loading
  alone must not autolink, normalize the saved document, or schedule a save.
- [ ] Implement the verified acceptance boundary once. Apply the action and
  synchronous appended transactions to one candidate, check its total size,
  and either publish it once or leave document/selection/history/save serial
  unchanged. Preserve Tiptap notifications and browser composition
  reconciliation.
- [ ] Measure the complete candidate JSON in UTF-8 before accepting every
  document change. Do not use the removed inserted-slice size estimate or
  add image-change detection and its tests. Undo/Redo use this same size
  boundary without rechecking restored image data.
- [ ] Share frontend/core boundary fixtures with escaping, multilingual text,
  actual attributes, exactly 12 MiB, and one byte over. Include marked-node
  splitting that duplicates a large link attribute.
- [ ] Isolate action history with boundaries before and after accepted
  formatting, links, paste, pictures, prompt insertion, Replace, and Replace
  All. Keep ordinary typing grouping and preserve Redo across saves.
- [ ] Keep editor selection authoritative, including image node/gap selections
  and end-of-document initialization. Expose accepted-change and command
  outcomes without copying the document into React state.

Completion: real editor tests prove all-or-nothing changes and independent
Undo events, including rejection after appended transactions.

### Step 8. Implement autosave and the shared leave procedure

References: design sections 10 and 11.1.

Main files: new `notes/useNoteSave.ts`, `App.tsx`, `useLiveSession.ts`,
`state/route.ts`, and the existing dialog component where needed.

- [ ] Track only edit/saved serials, one pending save, one deadline, the due
  flag, persistent error, and freeze state. Capture editor/context identity in
  callbacks. Read JSON for a request instead of retaining document snapshots.
- [ ] Start the five-second deadline on the first uncovered edit. Do not
  restart it on further typing. Keep one save request in flight; a deadline
  reached during that request marks the current document due.
- [ ] Process acknowledgement/error before the due flag. Use
  `editSerial > savedSerial` to decide whether a due or explicitly requested
  Retry is still needed. A failed request must not create an automatic retry
  loop, and a retry must not hide the unresolved error.
- [ ] Implement `leaveNoteThen(action)` around one pending user intent, before
  navigation side effects. Freeze document changes/submissions, stop the
  deadline, settle the pending write, and save remaining changes once.
- [ ] On failure, retain the editor and offer approved Retry / Stay / Discard.
  Keep selection/copy available. Discard waits for admitted writes, disposes
  the editor, then runs the action; it never marks unsaved edits as saved.
- [ ] Restore editing after Stay or destination failure. If an aborted leave
  canceled an uncovered deadline/due save, submit those changes at once rather
  than granting a new five seconds. Invalidate abandoned continuations.
- [ ] Guard opening/creating notes and sessions, the already-open-session
  shortcut, return-to-start, Settings/other screens, vault switching before
  cleanup, and route/back/forward entry points.
- [ ] Keep Notes outside URL routing: showing Notes or changing its active
  note does not push or replace history. The URL remains the current root or
  session URL. For history `Chat A → Chat B → Notes`, Back targets Chat A,
  not Chat B. Accept this behavior; Notes does not add a return-to-Chat entry.
  When the leave guard permits Back/Forward, open the reached root/session
  destination without pushing another history entry.
- [ ] Retain one current URL value in the existing route handling, initialized
  from `window.location.href` and synchronized with app URL writes. On every
  `popstate` and `hashchange`, capture the retained value as a local
  `previousUrl`, then immediately update the retained value to the new
  `window.location.href` before consulting the leave guard or awaiting work.
  Do not wait for navigation acceptance to update it. Thus the second event
  from one Back/Forward captures the URL reached by the first event. Keep each
  request's captured `previousUrl` local to that request so a later event cannot
  replace its cancellation target. On Stay or an ignored history request,
  restore that captured URL with `history.replaceState(null, '', previousUrl)`
  and synchronize the retained value. Do not derive it from the displayed
  Notes view or active Chat. Replacement restores the address, not the previous
  history index; no Notes route or separate navigation stack is needed.
- [ ] Keep tab/focus changes and in-place background updates outside the leave
  path. Reserve the same pending-operation slot for deletion in step 11.
  Connect native close through step 9. A running leave/deletion defers close;
  only an open Retry / Stay / Discard dialog cancels it immediately. Resume a
  deferred attempt when the operation releases the slot, or answer cancelled
  when a leave failure opens that dialog. Leave fault reload recovery unchanged.
- [ ] Use fake clocks and controlled promises to test continuous edits, slow
  writes, all error decisions, Retry during an unsuccessful pending save,
  covered errors, Undo during saving, failed destinations, and stale callbacks.
  Include Notes entered from root and session URLs, accepted Back/Forward,
  Stay restoring the exact prior URL, and ignored history requests while a
  leave is pending. Verify that showing Notes creates no history entry.
- [ ] Test one Back that emits `popstate` followed by `hashchange` while its
  leave promise is pending. Verify one leave/flush and one destination action;
  the second event must not restore the pre-Back URL. After successful leave,
  the URL and view must both identify the reached destination. Also cover Stay:
  the first request restores its captured pre-Back URL and keeps Notes open.

Completion: ordinary navigation cannot discard or submit late note changes,
and tests establish the save scheduling and acknowledgement rules.

### Step 10. Connect Notes navigation, browsing, creation, and opening

References: design sections 3, 4.1–4.2, and 13. Relevant UI approval required.

Main files: `App.tsx`, `state/view.ts`, `Sidebar.tsx`, new
`components/NotesScreen.tsx`, new `components/NotesTree.tsx`, and `styles/app.css`.

- [ ] Add sidebar tab, Notes main view, and active note ID state. Preserve the
  active Chat conversation independently. Tab switching changes browsing only;
  explicit note/session activation changes the main panel through the guard.
- [ ] Render the approved editor/composer layout without a panel title, chat
  history, or session picture panel. Preserve composer controls and omit only
  the Notes-inapplicable context/buttons specified by the design.
- [ ] Keep the tree instance for the current vault across tab changes. Load
  metadata lazily, derive child lists, identify empty folders by kind, and sort
  folders first with the specified name comparison and exact tie break.
- [ ] Use Headless Tree's focus and expansion state without a selection store.
  Compose its element props intact. Implement keyboard focus/activation and
  an open-note marker independent of the focused row.
- [ ] Implement root/folder creation with explicit destinations. Creating a
  note must resolve the old editor before the create RPC. Refresh/rebuild the
  tree, expand ancestors, reveal the result, and open the new note.
- [ ] Open through `notes.get`, retaining a disabled loading state. Reject
  stale responses and initialization failures without an editable fallback.
  Re-activating the valid open note focuses it without reloading its history.
- [ ] Connect accepted editor changes to autosave and the shared navigation
  leave procedure before enabling editing. Connect and verify native close
  afterward in step 9. Initialize the selection at the end and expose the
  active note path through the Send accessible name/tooltip.
- [ ] Connect the two draft slots. Mount Notes PromptInput with
  `key={activeNoteId}` as specified in step 6, preserving its App-owned draft
  while old voice/submission work is cleaned up. Dispose the Notes subtree
  and import origins on vault replacement, including same-ID notes across
  vaults. Keep the draft when no note is open but disable submission.
- [ ] Verify responsive layout, composer resizing, toolbar wrapping, scrolling,
  focus restoration, accessible labels/ARIA, and empty/loading/error states.

Completion: a user can create, open, edit, save, navigate within CHA, and reopen
a basic note through the actual native stack without losing either panel's
draft. This development build supplies the real editor for step 9; normal-close
safety is not established by step 10, and the release gate still applies.

### Step 9. Implement native close negotiation

References: design section 12, including its revised Retry ordering.

Main files: `packaging/macos/main.swift`, `native_bridge.swift`,
`packaging/windows/main.cpp`, native frontend helpers, and App's leave path.

Start after step 10 provides an actual Notes screen. Use a disposable vault
and an open note with unsaved edits for the save/close cases. Step 3 establishes
macOS run-loop feasibility; it does not establish these end-to-end outcomes.

- [ ] On both platforms, negotiate only when the runtime and a page connection
  exist. If either is absent, enter the existing shutdown path immediately,
  without prepare-close, a response timer, or a timeout decision dialog.
- [ ] Add the typed prepare-close, close-reply, close-aborted, and page-close
  messages on the existing trusted shell channel, outside the ordinary RPC
  queue. Match host attempt IDs and connection identity and ignore stale or
  untrusted messages.
- [ ] Install the injected default ready responder for startup, then replace
  it with App's responder before a note can exist. A connected page's failed
  script call or missing reply still reaches the native fallback; it is not
  the no-connection fast path. App's immediate-ready path requires no editor
  and no pending navigation, deletion, or save.
- [ ] Add one nullable `deferredCloseAttemptId` to App's close state. On
  prepare-close, reply cancelled at once only if Retry / Stay / Discard is
  open. Otherwise, if a leave or deletion holds the operation slot, store the
  ID without replying or starting a competing flush. When that operation
  releases the slot after its action and editor disposal/restoration, clear
  the field and process the still-current ID through the normal prepare-close
  handler against the current editor. If a leave failure opens the decision
  dialog, process it then so the cancelled reply stops the native wait.
- [ ] Keep the window, runtime, and bridge alive for one 15-second response
  wait, including time spent deferred behind a leave/deletion. Resume the same
  attempt with its remaining time; do not send another close request or add a
  page timer. Repeated close requests do not restart the timer. Ready starts
  normal shutdown; save failure cancels the wait and opens the page decision.
- [ ] On a close-dialog Retry, send a fresh close request before saving. Its
  new prepare-close runs the flush under a new native timer. Another failure
  returns to the decision. Discard settles writes and disposes the editor
  before a fresh close request. Stay restores editing.
- [ ] On timeout, invalidate the attempt and notify the page before showing
  native Cancel close / Close anyway. Run no timer while the user decides.
  Clear a matching deferred ID without canceling the earlier navigation or
  deletion or releasing its freeze. If close already owns the flush, apply
  its existing abort cleanup. Late completion must not reply or request a
  later close. Clear deferred state on page/native-connection replacement;
  stale aborts must not clear a newer attempt.
- [ ] On macOS, route Quit, last-window close, and page close through the
  selected step 3 path. Keep the fatal-error guard first and the window visible.
  Implement only the selected run-loop/termination strategy.
- [ ] On Windows, keep `request_close()` as the normal entry, keep `closing_`
  false during negotiation, and call `begin_shutdown()` after permission to
  close. Preserve direct fatal-error and smoke-test shutdown paths.
- [ ] Begin existing bridge detachment and the ten-second runtime shutdown
  deadline only when entering shutdown after ready, Close anyway, or an
  applicable direct shutdown path above. Set the `Closing CHA…` title at that
  same point on both platforms; keep the current title during negotiation,
  either decision dialog, and canceled close.
- [ ] On macOS, manually check the production packaged app for no-runtime and
  no-page-connection fast paths, startup/no-work close, every save decision,
  duplicate requests, stale replies, and a connected but unresponsive page.
  Verify title timing, including cancellation. Cover a failed save followed
  by a hung Retry, and close during a pending navigation
  flush. Cover completion resuming the same attempt, failure opening a decision,
  an already-open decision, and a hung flush reaching both native timeout
  choices. Check late completion after cancel and fatal shutdown. Exercise
  Quit and last-window close. Add deletion-specific close checks in step 11.
  Record reproduction steps, including how failures/delays were induced, and
  results with build and macOS/WebKit versions.
- [ ] Do not count the existing macOS harness as coverage of the production
  close path: `tests/native/macos/run.sh` compiles `native_bridge.swift` with
  `test_host.swift`, not `main.swift`, where `applicationShouldTerminate`
  lives. Keep useful bridge tests, but require the packaged manual checks
  above. No coordinator extraction or separate test framework is required.
- [ ] On Windows, extend the existing native tests for the same cases using
  the actual Notes screen. Verify Exit and application-window close in the
  packaged app. Record any manual cases separately from automated coverage.

Completion: both real hosts protect normal close and provide the native
fallback even when a leave flush, retry, or renderer stops responding.

### Step 11. Implement organization and deletion safely

References: design sections 3.2, 11.2, and 13.

- [ ] Add approved row menus and create/rename/move/delete dialogs. Capture
  source ID and vault context; keyboard focus changes cannot retarget them.
  Include root destinations and exclude a moved folder's own subtree.
- [ ] Serialize tree mutations, including their metadata refresh. Rebuild and
  restore/reveal focus after success; keep old rows and an error after failed
  refresh. Retry only the listing after a committed mutation.
- [ ] Mark a retained failed-refresh listing stale and require fresh metadata
  before another mutation/confirmation. Test move success followed by refresh
  failure and deletion of the destination folder.
- [ ] Confirm the full captured deletion scope, including all folder contents.
  Acquire the same pending-operation slot as navigation before checking whether
  the active note is in that subtree. No note can open there during deletion.
- [ ] For an affected editor, freeze changes, cancel scheduled saves, settle
  an admitted save, and delete without saving the unsaved remainder. A failed
  save does not block the confirmed deletion or add a second save dialog.
- [ ] Dispose the editor and its unsaved remainder only after deletion commits.
  On deletion failure, retain content/history/save errors, restore editing,
  and submit a canceled uncovered deadline at once. Preserve the Notes draft.
- [ ] Keep unrelated-note editing available during deletion while holding the
  operation slot against navigation. Defer native close using step 9's single
  attempt field; keep its host timer running. After deletion and required
  editor disposal/restoration, release the slot and resume any still-current
  attempt through the normal close handler against the resulting editor.
- [ ] Reject stale dialog callbacks on vault replacement. Test cancel, empty
  folder deletion, missing IDs, pending saves, failed deletion, focus after
  deletion, and late callbacks that must not affect another note.
- [ ] Extend the step 9 close checks after deletion exists: close during an
  affected or unrelated subtree deletion, while waiting for an admitted save,
  and while the delete RPC is pending. Cover success/failure resuming close
  without a competing flush, hung operations reaching the native fallback,
  duplicate Quit preserving the timer, and late completion after Cancel close
  unable to close CHA. Use packaged manual checks on macOS and the existing
  Windows coverage; include these results in step 16's release checks.

Completion: organization preserves contents and no deletion can race with
navigation, rely on stale ancestry, or resurrect a removed row.

### Step 12. Add formatting and prompt insertion

References: design sections 3.1, 3.5, 4.3, 7.4, and 8.

- [ ] Connect approved paragraph/heading, bold, italic, underline, highlight,
  list, Undo, and Redo controls to the same tested editor commands. Retain
  selection when toolbar focus changes and omit no-op history events.
- [ ] Finish the tested paragraph insertion command from step 2. Normalize
  line endings, create literal text/hard breaks without inherited marks, and
  preserve unselected fragments and list/heading structure in one action.
- [ ] Cover empty paragraphs, block starts/ends, images/gap selections, nested
  lists, and selections across blocks/items. Put selection at the new
  paragraph's end so subsequent submissions precede the old trailing content.
- [ ] Add valid new URL marks, including a final URL without trailing space,
  using the shared link policy. Never parse prompt input as HTML/Markdown or
  pass `/clear`, `/mcast`, or addressing through Chat command processing.
- [ ] Return `clear_input: true` only after full transaction acceptance. Keep
  focus in the prompt, retain later input, and prevent save failure from
  reinserting text. Undo/Redo must not restore or change prompt drafts.
- [ ] Add the approved inactive caret/selection decoration as view-only state.
  Use current editor selection, including Find selections, rather than a
  second remembered selection.
- [ ] Exercise typing, transliteration, IME, Send, Enter, Ctrl+Enter, and
  continuous spoken submission. Verify cleanup and origin checks when a note
  changes during voice completion or submission.

Completion: every PRD insertion example and one-step Undo/Redo behavior passes
through the live shared composer without sending a chat request.

### Step 13. Add pictures, clipboard content, and web links

References: design sections 3.4 and 9.1–9.4.

Main files: `webapp/src/notes/pictures.ts` and the editor integration.

- [ ] Complete the shared file/clipboard image importer. Check PNG, JPEG,
  WebP, and GIF using magic bytes, raw byte size, and valid Base64/MIME
  agreement for embedded data URLs. Permit exactly 1 MiB before encoding.
  Preserve supported original bytes and reject temporary/remote references.
- [ ] Capture and map a selection bookmark through accepted edits while an
  async file read runs. Recheck editor/context/freeze state before insertion;
  stale or failed imports cannot change the note or another editor.
- [ ] Add the approved one-file picker, image display/selection, and ordinary
  removal. Check the complete resulting document size and isolate insertion
  history. Do not add resizing, cropping, uploads, or a picture store.
- [ ] Implement detached HTML preprocessing in `transformPastedHTML`, followed
  by normal schema parsing:
  retain supported formatting/links, normalize supported highlights, remove
  font colors/unsupported objects, and flatten tables with readable separators.
  Preserve internal copy/paste slice metadata and omit remote-only images.
- [ ] Use one synchronous slice-image validator in both `handlePaste` and
  `handleDrop`, reusing the image rules above. Validate the slice supplied by
  ProseMirror, including internal drag slices. If any image is invalid or
  oversized, reject the entire paste/drop: return `true` without dispatching
  or falling back to another format. Keep file checks in the importer; do not
  repeat them in the transaction boundary.
- [ ] Handle empty paste slices explicitly so unsupported-only paste cannot
  delete a selected passage. Use the design's image-file or `pasteText`
  fallback once when appropriate, without duplicate image insertion or
  fallback recursion.
- [ ] In `handleDrop`, consume all file drops without insertion or import
  fallback, even if the event also supplies HTML or text. Return `true` before
  slice validation or insertion. Consume empty or unsupported non-file drops
  without insertion. Apply one history/size boundary to each complete paste
  or accepted non-file drop. No unchecked drop path may add pictures.
  External HTML drops already pass through `transformPastedHTML` before
  `handleDrop`; reuse that pipeline instead of adding another HTML parser.
- [ ] Apply the shared HTTP/HTTPS predicate to parsing, typed/pasted/prompt
  autolinks, edited destinations, rendering, and activation. Preserve explicit
  HTTP; use HTTP for recognized bare domains; keep unsupported links as text.
- [ ] Implement approved keyboard/context-menu Edit link and Remove link.
  Capture the intended mark range, keep visible text, isolate history, and
  prevent immediate or unrelated-edit relinking after removal.
- [ ] Extend native HTTP/HTTPS navigation interception on both hosts, including
  new-window handling. Use the stored mark destination, validate the parsed
  scheme, and keep the embedded editor open. Secondary click must not launch.
- [ ] Test real Word/web/internal clipboard content, all supported image types,
  offline reopen after source removal, internal drags, and external HTML drops.
  Verify file drops leave content, selection, and history unchanged, including
  events with an HTML/text alternative. Cover
  malformed Base64, MIME/magic mismatch, and oversized images through both
  slice handlers; invalid mixed content must leave the whole action unchanged.
  Verify valid image insertion and Undo/Redo, total document size rejection,
  invalid loaded links, ordinary/secondary clicks, and persisted unlinking.

Pipeline references: [ProseMirror clipboard parsing](https://github.com/ProseMirror/prosemirror-view/blob/master/src/clipboard.ts)
and [drop dispatch](https://github.com/ProseMirror/prosemirror-view/blob/master/src/input.ts).
Verify these hooks against the packages pinned in step 2.

Completion: accepted content stays offline-capable, supported, within limits,
and reversible as a whole; web links open consistently in both system browsers.

### Step 14. Add Find and Replace

References: design sections 3.3, 3.5, and 9.5.

- [ ] Add the approved CHA find bar using the installed headless extension.
  Use its transient matches/index, with CSS injection disabled and the initial
  zero search debounce from the design. Do not maintain a second search index.
- [ ] Connect term, literal replacement, case, whole-word, regex, navigation,
  Replace, and Replace All commands. Regex clears/disables Whole word; empty
  replacement deletes; invalid/unsupported RE2 patterns follow tested behavior.
- [ ] Show applicable result/no-match state, scope Mod+F and field shortcuts,
  and preserve prompt shortcuts. Find navigation changes the editor selection
  that also controls subsequent prompt insertion.
- [ ] Pass replacements through atomic acceptance and isolated history.
  Replace All uses the original match set in one action, not repeated dispatch.
- [ ] Clear search decorations on close and reset controls on another note.
  Search-only operations must not schedule a save or alter history.
- [ ] Test mark-boundary matching, block/hard-break boundaries, literal `$1`,
  empty replacement, case options, pending terms, zero results, keyboard
  behavior, complete Undo/Redo, and near-limit rejection on both platforms.

Completion: the approved controls meet the PRD independently of the supplied
Tiptap panel's behavior.

### Step 15. Verify vault operations and replacement copy

References: design section 14; PRD section 5.1.

Main files: existing vault operations and maintenance tests, relevant desktop
vault controls, and `webapp/src/chaweb/vaultActions.tsx` where applicable.

- [ ] Verify empty new vaults and whole-vault copies, including nested metadata,
  formatting, pictures, links, and the non-reuse sequence. Exercise plaintext
  and protected databases and password/protection changes.
- [ ] Verify Upload/Copy after leaving Notes include the final saved edits.
  Keep the Settings leave guard as the save boundary, without another Notes
  flush in each vault button.
- [ ] Apply the approved Download and preliminary Parent merge confirmation
  copy, including applicable ChaWeb controls. State note/folder replacement
  and the single previous-database `.bac` scope.
- [ ] Test replacement with newer and older valid schema-2 vaults, saved edits
  in the backup, explicitly discarded edits absent from it, and a second
  successful replacement overwriting the prior backup.
- [ ] Verify Merge, workspace Import/Export, and session Markdown mirroring
  remain configuration/session operations: source Notes are excluded and
  destination Notes remain intact.
- [ ] Verify Parent merge changes the downloaded parent collection/backup but
  not the active vault's Notes; a later merge failure leaves the completed
  parent Download in place.
- [ ] Verify successful, failed, and canceled vault switches; prompt draft
  reset only on success; obsolete context rejection; and old editor/tree
  callbacks unable to affect the new database. Corrupt individual note JSON
  must not introduce a new blocker for otherwise valid whole-vault operations.

Completion: transfer tests establish the PRD's full-copy versus configuration
transfer boundary and backup behavior, without adding Notes transfer machinery.

### Step 16. Complete packaged acceptance and remove unused work

References: PRD section 8; design sections 15.3–15.4.

- [ ] Run the affected frontend, C++, bridge, runtime, and native tests. Run
  the full normal checks after integration, and build desktop and ChaWeb assets.
- [ ] Execute all 23 PRD scenarios on packaged macOS and Windows builds. Record
  build revision, package pins, OS/browser versions, results, and remaining
  failures. Automated DOM tests do not certify native clipboard, CSP, or close.
- [ ] Include keyboard-only operation, real IME, supported languages, actual
  clipboard image files, resizing/short windows, and real dictation where
  configuration permits. Keep provider-dependent checks separate from offline
  Notes acceptance and use disposable configured data for live transfers.
- [ ] On the packaged macOS app, focus the note editor and click Edit ▸ Undo,
  Edit ▸ Redo, and Edit ▸ Paste. With multiple history entries available,
  verify each Undo/Redo moves exactly one step through the editor's history,
  including the native `historyUndo`/`historyRedo` input-event path. Verify
  Paste uses the same clipboard validation and inserts the whole paste as one
  undoable action. Repeat with Command+Z, Command+Shift+Z, and Command+V;
  each shortcut must act once, without duplicate keyboard/native handling.
- [ ] Profile the near-12-MiB note used for PRD scenario 22, with valid pictures
  and multilingual text:
  typing/IME, complete JSON size checks, paste, Replace All, search navigation,
  save-start timing, memory, and reopening. Include continuous typing during
  a slow write and confirm the one-save/due-save behavior.
- [ ] Only if scenario 22 shows lag, investigate with relevant measurements
  of vault open/switch/maintenance with many large notes or UI-thread parsing
  of large save envelopes in the affected shell. These two additional
  measurements from design section 15.4 are conditional diagnostics, not
  independent release checks. Optimize only a measured bottleneck, preserve
  exact acceptance, and rerun the affected checks. Do not introduce the removed
  size estimate, a speculative cache, or an autosave worker.
- [ ] Build local packages using the platform-specific commands in section 5.
  Verify dependencies, licenses, bundled CSS, and native smoke tests. Publishing
  or deployment is a separate action from developing and validating Notes.
- [ ] Remove experiment scaffolding, duplicate state, unused code/dependencies,
  and tests/contracts that support abandoned behavior. Review the diff for
  accidental changes to Chat, ChaWeb, vault configuration, and other docs.
- [ ] Report completed acceptance and any actual limitation. Do not mark the
  feature ready while a platform gate or PRD scenario remains unverified.

Completion: all checks in section 6 pass and the feature is ready for use on
both desktop platforms.

## 4. PRD acceptance coverage

Run each original scenario in full. This table assigns ownership; it does not
replace the detailed scenario text or the failure cases in the design.

| PRD scenario | Required outcome | Main steps |
| --- | --- | --- |
| 1 | Create, format, normal close, and reopen on both hosts | 4, 7–10, 12, 16 |
| 2 | Persistent tree organization and sorting | 4, 10, 11, 16 |
| 3 | Independent panels, browsing, drafts, and submission targets | 6, 8, 10, 12, 16 |
| 4 | Find/replace modes, literal/empty replacement, complete Undo/Redo | 7, 14, 16 |
| 5 | Prompt insertion, voice/transliteration, structure, marks, and history | 2, 6, 7, 12, 13, 16 |
| 6 | Vault isolation and empty new vault | 4, 8, 10, 15, 16 |
| 7 | Confirmed deletion and preserved work on cancel/failure | 4, 8, 11, 16 |
| 8 | Safe opening/saving failure and persistent actionable errors | 5, 7–11, 16 |
| 9 | Continuous-typing save deadline without focus/history changes | 7, 8, 16 |
| 10 | Same saving rules for every edit and intact Redo | 7, 8, 12–14, 16 |
| 11 | Save before Settings/other navigation; no delayed writes | 8–10, 16 |
| 12 | Copy/Upload/Download and previous-database backup | 4, 8, 15, 16 |
| 13 | Native unresponsive-page close choice and separate shutdown limit | 3, 9, 16 |
| 14 | Supported mixed paste, table text, and unsupported-content no-op | 7, 13, 16 |
| 15 | Four image formats, clipboard, offline persistence, whole Undo | 4, 7, 13, 15, 16 |
| 16 | Allowed links and native browser opening; blocked unsupported schemes | 7, 13, 16 |
| 17 | Name validation and exact cross-kind sibling uniqueness | 4, 5, 10, 11, 16 |
| 18 | Invalid moves leave the tree unchanged | 4, 11, 16 |
| 19 | Merge/Import/Export/mirror exclude Notes as required | 15, 16 |
| 20 | Parent Download/backup and merge-failure behavior | 15, 16 |
| 21 | Exact picture limit and whole-paste rejection | 7, 13, 16 |
| 22 | Exact document limit, atomic rejection, and responsiveness | 2, 4, 7, 12–14, 16 |
| 23 | Link editing/removal, keyboard access, persistence, and history | 7, 13, 16 |

## 5. Verification commands and environments

Use the Node.js/npm versions declared by `webapp/.node-version` and
`webapp/package.json`. From `webapp/`, install the lockfile dependencies for
the baseline and run these checks after the relevant changes:

```sh
npm ci
npm run api-types
npm run check
npm run build
npm run build:chaweb
```

`api-types` regenerates the DTO output; `check` checks that generated output,
types, and tests agree. Review generated changes with the source DTO change.
Use targeted Vitest tests during a step, then the full check after integration.

From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

On supported non-Windows hosts, use `make itest-local` for local integration
coverage. Keep live-provider/R2 tests separate and configured for disposable
data, as described in the [repository build instructions](../README.md).

Extend the current [macOS harness](../tests/native/macos/run.sh) and
[Windows harness](../tests/native/windows/run.ps1), with their existing test
hosts and vault preparation helpers. Add Notes cases to the harness entry
points as well as the test files: the Windows runner currently selects named
tests with a grep expression, so an unselected new test does not establish
coverage. On macOS, perform the step 9 close checks manually in the production
packaged app. The harness does not compile `main.swift` and cannot establish
that application's `applicationShouldTerminate` behavior. Keep its bridge
coverage separate from the recorded packaged-app close results.

For local package validation, use the appropriate command on its host:

```sh
# macOS
make package-macos VERSION=0.0.0-notes-dev
```

```sh
# Windows
make package-windows VERSION=0.0.0-notes-dev
```

Use the explicit platform target. The generic `make package` target publishes
to R2 and is not part of these local development checks. Replace the example
version with the intended build version when preparing an actual release.

## 6. Final completion checklist

- [ ] Remaining visible changes have explicit approval, and implementation
  matches that approval without new panel titles or unnecessary helper text.
- [ ] Dependency pins, editor acceptance/history behavior, and the selected
  macOS termination path have recorded evidence.
- [ ] All 23 PRD scenarios pass on packaged macOS and Windows builds.
- [ ] Review regressions pass: empty paste cannot delete a selection; exact
  size checks cover marked-node splitting; pending-save Retry is not lost;
  native Retry is timed; stale tree metadata cannot drive deletion; deletion
  cannot race with navigation; close during leave/deletion retains the native
  timeout and aborted attempts cannot resume; typed input needs no voice
  configuration; interleaved speech/typing is retained; keyed note changes discard late
  submission effects without clearing the shared draft; Chat target, Stop,
  status-row, and speaker/dictation behavior are preserved.
- [ ] Notes preserve Chat behavior and remain outside chat context, ChaWeb
  editing, configuration transfer, and the session Markdown mirror.
- [ ] Vault copy/protection/transfer and backup checks pass with disposable
  plain/protected data, including old schema-2 compatibility.
- [ ] Near-limit measurements establish usable editing and the specified save
  scheduling on both hosts; no correctness check is weakened for performance.
- [ ] Normal builds, relevant integration tests, native tests, and local
  packages pass. Unrun platform/provider checks are reported, not marked passed.
- [ ] Temporary work and unnecessary machinery are removed, and the final
  change is limited to the feature and its required regressions.
