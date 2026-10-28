# Block 3 — UI and feature acceptance

Status: Implementation instructions; requires completed blocks 1 and 2.

This is the third and final implementation block, sized for one 512K-token
session including implementation, tests, fixes, and review. It delivers the
approved folder-store UI, preserves editor behavior during asynchronous deletion,
and validates the complete feature. This file contains the implementation and
review requirements without requiring earlier chat history or another block
guide. Source documents: [store-design.md](store-design.md),
[store-prd.md](store-prd.md), and [store-plan.md](store-plan.md).

## 1. Product behavior and scope

A real folder owns at most one fixed remote snapshot on OpenAI or Google. Upload
captures all saved nonempty ordinary notes in that folder and descendants.
Local edits do not update the snapshot. Refresh means Remove, then Upload.
Parent and child stores are independent. The synthetic notes root cannot own
a store or have a folder Ref ID.

Copy Ref ID uses the folder's existing item ID. Submitting that ID alone attaches
the folder to a conversation without generation. Ordinary answers use only
current ready stores compatible with the target's direct service, protocol,
and saved-key ID. `/detach <Ref ID>` removes only the folder attachment. Attachments
survive restart, cover/uncover, and store replacement; Clear removes them, and
local folder deletion prunes them. Uploaded text is never copied into the prompt
as a compatibility fallback.

Remove deletes remote resources while preserving local notes and chat folder
attachments. Folder deletion cleans its own and descendant stores before local
deletion. Known cleanup failure keeps notes and offers Retry, Cancel, or explicit
Delete anyway. This last choice records unconfirmed resources atomically with
local deletion and schedules no later cleanup. Partial remote cleanup is not
undone. Unknown/not-found outcomes can complete local removal with a visible
warning; they must not be described as proven remote deletion.

Deliver the desktop menus/dialogs, editor coordination, focused frontend tests,
native smoke coverage, integrated checks, both live-provider checks, and final
acceptance evidence. Use the backend and typed client from blocks 1 and 2. Fix
integration defects with regression coverage, but do not redesign the feature or
add a new management API, provider setting, citation panel, synchronization,
discovery, expiry, daemon, or progress dashboard.

The user explicitly instructed that implementing a completed design counts as
design approval. The sketches and states below reproduce that design. Implement
them without an additional UI approval gate. Keep existing dialog styles and
focus/keyboard behavior. Do not put visible title labels or legends at the top
of panels. Copy is limited to controls, actionable state, validation, and errors;
do not add explanatory helper text for obvious behavior.

Inspect the current diff and preserve unrelated changes. Follow `AGENTS.md` and
KISS. Do not edit documentation as an incidental implementation task unless the
execution prompt explicitly authorizes status/evidence updates. Otherwise provide
the acceptance record in the final handoff.

## 2. Prerequisite contract

Verify these behaviors in the actual code and block handoffs before building UI.
Use the implemented DTO field/type names; the table states required semantics,
not permission to add a second frontend state store or API.

| Native method | Required contract |
| --- | --- |
| `notes.storeChoices` | Eligible service and saved-key ID/name choices. No raw secret. Every call uses the current context epoch. |
| `notes.storeUpload` | Folder ID, service, key ID; bounded acceptance returns operation ID. Preparation is asynchronous and can fail before remote creation. |
| `notes.storeRemove` | Folder ID and expected local store-record ID; returns operation ID. |
| `notes.storeOperation` | Lookup by folder ID returns no current control or its ID/kind/phase/counts/safe error/terminal outcome/known-cleanup status. Supports active, failed, and unacknowledged terminal results. |
| `notes.storeCancel` | Exact operation ID and current epoch; requests upload cancellation followed by cleanup. |
| `notes.storeClose` | Exact operation ID and epoch; acknowledges terminal result or closes a failed-delete attempt and releases its reservation. Active cleanup cannot be discarded. |
| `notes.folderDelete` | Initial folder ID plus confirmed store count. Retry uses failed operation ID; `delete_anyway = true` requires that same failed attempt and original context. |

The full notes listing includes an optional real-folder summary: service, local
store-record ID, and ready/incomplete state. It contains no remote IDs. Use its
hierarchy to count stores in a subtree, including incomplete/no-remote-ID records.
The backend rechecks count and ancestry on admission. A changed count rejects
the initial request before deletion. There is no delete-info endpoint.

Operations belong to the application, not the bridge connection or dialog.
Disconnect/timeouts do not cancel them. A failed folder delete retains its
subtree reservation until explicit Retry/Cancel/Close/Delete anyway or vault
interruption. Status can be recovered by folder ID. Terminal local-deletion
status must remain usable after the folder disappears from the listing; retain
the captured folder/operation identity until acknowledging it.

The status contract must distinguish editable preflight error, normal upload
failure, upload committed ready, cancellation cleanup, confirmed cancellation,
Remove failure/completion, warning completion, failed reserved folder deletion,
and committed local folder deletion. If the typed API cannot express a required
distinction, fix that contract and its tests rather than guess from a missing
row or an accepted start reply.

Backend guarantees to preserve:

- One active operation per folder; subtree deletion rejects conflicts and
  membership changes. UI button disabling is not the authority.
- Only ready stores grant new access. Removal marks incomplete before networking.
- Cancellation can let a sent allocation finish to learn its ID, then cleans it
  up; it never later reports readiness. Cleanup can take time.
- Confirmed deletion or structured not-found clears known IDs; not-found and
  unknown allocation outcomes are warnings. Failed known cleanup retains IDs.
- Standalone Remove never deletes notes or session attachments and has no Delete
  anyway option. Confirmed subtree deletion prunes refs; historical chat stays.
- Vault/epoch changes interrupt old work. Stale UI commands cannot act on new
  controls. Incomplete attempts never resume automatically on reopening.
- Existing API-key usage includes ready and incomplete folder stores. Key deletion
  and value replacement remain allowed; stores never rebind to another saved ID.

Missing prerequisite behavior is an integration defect, not a reason to build a
frontend workaround that bypasses cleanup or changes the product rules.

## 3. Code map and implementation order

| Area | Primary files |
| --- | --- |
| Tree/menu/delete dialog | `webapp/src/components/NotesTree.tsx` and `NotesTree.test.tsx`. Existing menu root IDs are synthetic; distinguish them from real folder items. |
| New focused dialog | `webapp/src/components/FolderStoreDialog.tsx` and its focused test file. Reuse existing dialog classes/layout. |
| Editor ownership | `webapp/src/components/App.tsx`, `NotesScreen.tsx`, `webapp/src/notes/useNoteLeave.ts`, and `useNoteSave.ts`; regression coverage in `Notes.test.tsx` and note-save tests. |
| Client/DTOs | Block 2's `webapp/src/api/{client,nativeClient}.ts`, generated schema, guards, and test fixtures. |
| Reference formatter | `webapp/src/notes/reference.ts`. Preserve the existing base62 format and six-digit input limitation. |
| Native smoke | `tests/native/ui.js`, `tests/native/macos/test-ui.sh`, existing native test-vault/host support, and Windows host tests where available. |
| Integrated acceptance | Existing C++/Vitest/provider/integration harnesses; extend them only where necessary for repeatable store checks. |

Implement the menu and upload/removal dialog, then folder-delete states and
editor coordination, then integrated/native checks and both live flows. Test
failure states as they are added; do not defer them all to the end. Keep local
state in the owning component as existing code does. Do not introduce a general
operation UI framework, global polling service, or persisted frontend job state.

## 4. Menus and approved dialog states

### Step 1: Extend the folder menu

Preserve the order and behavior of existing menu actions. Add Copy Ref ID for
every real folder, whether it has a store or not, using the existing formatter.
Preserve existing note/annotation copy behavior; never enable folder actions on
the synthetic root. Add Upload when there is no store row and Remove when there
is any row, including incomplete. Do not display remote IDs, a stale badge, or
a separate store-management screen.

```text
No store                     Store registered
┌───────────────────────┐    ┌───────────────────────┐
│ Copy Ref ID           │    │ Copy Ref ID           │
│ Upload to store       │    │ Remove from store     │
└───────────────────────┘    └───────────────────────┘
```

On menu/dialog reopen, query status for the real folder and recover active or
failed work before starting another operation. Disable conflicting actions while
status/admission is unresolved or work is active. Copying identity itself does
not start remote work. A failed status request must not trigger a fresh upload
or release a reservation.

### Step 2: Implement Upload

Use the accessible dialog name **Upload folder to store**, without rendering it
as a visible panel title. Initial fields are Provider, API key, Cancel, and Upload.
Provider choices are OpenAI and Google; key choices come from the backend's
compatible connections. Never ask for a raw key or offer arbitrary saved keys.

```text
┌─────────────────────────────────────────────┐
│ Provider                                    │
│ [ OpenAI                                ▾ ] │
│                                             │
│ API key                                     │
│ [ Personal OpenAI                       ▾ ] │
│                                             │
│                       [ Cancel ] [ Upload ] │
└─────────────────────────────────────────────┘
```

Automatically select a key only when the selected service has one eligible key.
With several, require selection; with none, disable Upload and show the actionable
settings error. Changing the provider must not retain an incompatible selected
key. The backend revalidates submission, including configuration changed since
choices were loaded. Unused model settings must not block a valid operation.

```text
┌─────────────────────────────────────────────┐
│ Provider [ OpenAI                        ▾ ]│
│ API key  [                               ▾ ]│
│ Add a compatible provider and saved key      │
│ in Settings.                                │
│                       [ Cancel ] [ Upload ] │
└─────────────────────────────────────────────┘
```

Upload uses only saved database content. Do not call the editor's save/flush
routine or submit its current document. All ordinary saved descendant notes are
eligible, including editable ones; generated annotations, binaries, compressed
summaries, and unsaved edits are excluded. A preflight error names the problem
and keeps fields editable. All-empty/damaged/oversized input must not allocate
remote resources.

Once accepted, retain operation identity and replace fields with concise phase
status. Disable duplicate submission and provider/key changes. Use actual backend
counts; do not invent percentages or treat uploaded count as indexing completion.

```text
┌─────────────────────────────────────────────┐
│ Uploading 3 of 12 notes…                     │
│                                  [ Cancel ] │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│ Indexing notes…                             │
│                                  [ Cancel ] │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│ Cleaning up…                                │
└─────────────────────────────────────────────┘
```

| Upload state/result | UI behavior |
| --- | --- |
| Before submission | Cancel closes without remote work. |
| Preparing/uploading/indexing | Cancel requests `storeCancel` for the current operation. It does not immediately close or mean remote deletion succeeded. |
| Cancellation cleanup | Show Cleaning up; prevent repeated cancellation or dismissal that would discard cleanup. |
| Ready committed | Acknowledge terminal state as required, close, and refresh listing. Only this result is successful upload. |
| Confirmed cancelled cleanup | Close and refresh; Upload is restored when no record remains. |
| Preflight error | Restore editable fields and actionable validation. A new submission starts a new attempt as permitted by the API. |
| Normal upload/indexing failure | Keep error and Close visible even if cleanup succeeded. Direct to Remove only if known cleanup remains. |
| Cancellation cleanup failure | Show error/Close; retain Remove for explicit cleanup retry. |
| Not-found/unknown outcome with known cleanup finished | Keep warning/Close visible. Local association removal is not proof that all remote data was deleted. |

Example failure and warning layouts:

```text
┌─────────────────────────────────────────────┐
│ Could not index “Experiment”.                │
│ Use Remove from store to retry cleanup.      │
│                                   [ Close ] │
└─────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Local removal completed. Some remote data could not be       │
│ confirmed deleted. See Events for manual-cleanup details.    │
│                                                   [ Close ] │
└──────────────────────────────────────────────────────────────┘
```

Omit the Remove guidance when no known cleanup remains. Show provider/folder/note
context from safe errors, never raw provider bodies, note text, secrets, or upload
URLs. Diagnostic resource IDs may appear in explicit recovery errors/Events, not
ordinary menu labels. Do not auto-retry a start request after a bridge timeout;
first recover status by folder ID.

### Step 3: Implement standalone Remove

Use the existing confirmation style, naming the folder and service. It is the
same action for ready and incomplete records and keeps all local notes.

```text
┌─────────────────────────────────────────────┐
│ Remove “Research” from OpenAI?               │
│                       [ Cancel ] [ Remove ] │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│ Removing store…                             │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│ Could not remove “Research”:                 │
│ the saved API key is missing.                │
│                                   [ Close ] │
└─────────────────────────────────────────────┘
```

Submit with the captured expected store-record ID and current epoch. During
removal show pending status and prevent duplicate submission. Failure retains
the incomplete record and actionable error; Close acknowledges it and a later
Remove retries remaining IDs. Confirmed completion restores Upload. Warning
completion uses the warning/Close state above. Never offer Delete anyway here.

### Step 4: Extend folder deletion

The current tree skips confirmation for empty folders. Override that shortcut
when the selected subtree contains stores, even with no local notes. Count all
owning folders using the full listing, including incomplete rows. Count the
selected folder itself; exclude ancestor stores outside the subtree.

```text
┌───────────────────────────────────────────────────┐
│ Delete “Research” and all its notes?               │
│ 2 remote stores will also be removed.              │
│                             [ Cancel ] [ Delete ] │
└───────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────┐
│ Removing remote stores…                           │
└───────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────┐
│ Could not remove 1 OpenAI store: access denied.     │
│ Delete anyway removes the local folder and notes.  │
│ Remote data and charges may remain.                │
│                                                   │
│             [ Cancel ] [ Retry ] [ Delete anyway ] │
└───────────────────────────────────────────────────┘
```

Use actual folder/provider/count/error. Preserve existing local-only note/folder
confirmation behavior where no store exists. Pass the confirmed store count on
initial `folderDelete`. If the backend detects a changed count, refresh the
listing, retain an ordinary validation message, and require a new explicit
confirmation. Do not proceed automatically or add a new confirmation-token state.

During cleanup, disable repeated deletion. After known cleanup failure:

- Retry sends the retained failed operation ID and cleans only remaining IDs.
  It may succeed after replacing the value of the same saved key.
- Cancel or explicit dialog Close calls `storeClose` to release that failed
  reservation, keeping local notes and remaining incomplete records. Stores
  already deleted stay deleted.
- Delete anyway uses that exact failed operation/epoch with `delete_anyway`.
  It deletes local data only after the backend atomically saves recovery warnings.
  It makes no further remote requests. A local transaction error remains visible
  and is not treated as successful deletion.

Unknown/not-found warning completion with no known failed resources is a terminal
local deletion plus warning/Close, not a failed-delete Retry state. Refresh the
listing and editor because local data is gone, even while the warning stays open.
The backend decides whether local deletion committed; a successful start reply,
cleared remote ID, or missing listing item is not a sufficient substitute.

## 5. Editor, lifetime, and interaction details

### Step 5: Preserve editor-save coordination

`App.tsx` currently wraps deletion with `noteLeave.beginNoteAction()`, tests whether
the open note is in the subtree, freezes its save controller, waits for in-flight
saves, deletes, then disposes the editor only on success. On failure it unfreezes
and resumes the existing save behavior. Adapt this path to asynchronous operation
completion; do not bypass it by calling a store client directly from the tree.

1. Capture the selected subtree, active note identity, and epoch for the attempt.
   Preserve existing protection against a late save racing local deletion.
2. For an affected open editor, freeze and wait for in-flight saves before the
   running delete attempt as the current code does. Do not dispose it when the
   backend merely accepts work.
3. When status confirms local deletion, dispose/close only the affected editor
   still belonging to the captured context. Refresh listing/selection and retain
   existing refresh-failed handling without trying to delete again.
4. On cleanup failure, rejected start, or Cancel, keep the editor and restore
   save behavior. Retry or Delete anyway starts the appropriate guarded local
   attempt again. No failed attempt may leave saving permanently frozen.
5. A UI reconnect may find deletion active or already complete. Reconcile that
   outcome with the current editor/listing rather than relying on the lost start
   promise. An old completion must never close a note opened in another vault.
6. Preserve annotation/audio/compression refresh effects and local-only deletion.
   Backend subtree reservations prohibit structural membership changes, not all
   ordinary note saves; do not add an application-wide editor lock.

### Step 6: Preserve recovery, keyboard handling, and focus

Poll only while an operation dialog is active. Stop frontend polling on dialog
unmount or epoch change, without sending an implicit backend cancellation.
Refresh notes/key usage through existing refresh mechanisms after outcomes.
No permanent background poller or global progress panel is needed.

Check context epoch and operation identity before applying every asynchronous
reply. On context invalidation, dismiss old UI state without sending cleanup
commands into the new vault. Backend interruption retains incomplete records
for explicit Remove on reopening.

Restore focus to the source tree row or a sensible surviving neighbor after
deletion. Maintain menu keyboard navigation and dialog focus containment. Map
Escape to the permitted action for that state: ordinary Cancel before submission,
the same upload cancellation path during forward work, Close for terminal/failed
states, and no dismissal that discards active cleanup/removal. Do not let Escape
silently close a failed-delete dialog without releasing its reservation.
Disable duplicate controls while their request is pending. Use current error
rendering/alert conventions and accessible names. Warning text must not vanish
before the user can read and close it.

## 6. Automated and native validation

### Step 7: Add focused frontend tests

Extend `NotesTree.test.tsx`, `Notes.test.tsx`, existing native-client and settings
tests, and add `FolderStoreDialog.test.tsx`. Use typed fake client results,
deferred promises, and fake timers where needed. Avoid test-only production
timers or another operation framework.

| Area | Required cases |
| --- | --- |
| Menus | Real-folder copy with/without store; stable ref after rename/replacement; synthetic root excluded; existing actions/order and note copy unchanged; ready/incomplete Upload-versus-Remove state. |
| Choices | Both providers, one-key auto-selection, multiple-key explicit selection, zero-key error, deduplicated choices, provider change clearing incompatible selection, choices changed before submit. |
| Upload | Saved rather than unsaved editor content; preflight fields remain editable; duplicate starts blocked; backend progress/indexing; no success before ready; failure with/without remaining cleanup; no raw error-body exposure. |
| Cancel | Before start; preparation/upload/indexing; late allocation response still cleaning; no premature close/readiness; cleanup failure and warning outcomes; Cancel/Close/Escape behavior by phase. |
| Remove | Expected record ID supplied; incomplete records removable; pending controls; safe missing-key/access errors; explicit retry from remaining record; notes retained; no Delete anyway. |
| Delete | Selected/descendant store count, incomplete and empty-folder cases, ancestor excluded, changed-count validation/reconfirmation, retained failed reservation, same-ID Retry, Cancel release, explicit Delete anyway, local-write failure. |
| Editor | In-flight save before deletion; editor remains while accepted/pending/failed; unfreeze on error/Cancel; dispose only after local commit; warning completion still disposes; unaffected editor survives; retry, reconnect, and epoch races. |
| Recovery | Dialog/menu reopen during work or failure; start request timeout followed by status lookup; no auto duplicate start; terminal result after folder removal; stale IDs/epochs; unmount does not cancel; polling exists only with active dialog. |
| Access/focus | Accessible upload name without visible title; keyboard/menu focus; valid Escape action; focus after deleted row; safe errors and warnings remain readable. |

Do not duplicate all block 1/2 backend tests in React. Assert the UI sends the
correct command and responds to the tested contract. When integration exposes a
backend defect, add the regression at its owning layer as well.

### Step 8: Run integrated checks and native smoke coverage

From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
npm --prefix webapp run check
make itest-local
npm --prefix webapp run build
npm --prefix webapp run build:chaweb
git diff --check
```

`check` includes API-type consistency, TypeScript checking, and Vitest. If fixing
the DTO contract, regenerate with `npm --prefix webapp run api-types` and inspect
the generated diff. Do not run unrelated live/R2 integration tests just to validate
stores. Use sanitizer builds only when unresolved worker-race concerns justify it.

Extend the existing native smoke harness for the new menu/dialog/editor flow.
On macOS, `make test-ui` builds and runs the instrumented host at desktop and narrow
widths using disposable test vaults; the unmodified script does not by itself
prove coverage of new store controls. Add the focused coverage and inspect the
rendered layout. Check Windows host behavior with its existing harness/build when
available, and report unavailable platforms explicitly. A web build is not proof
of native-host interaction correctness.

Keep the UI responsive during background operations. Confirm the approved copy,
dialog sizing, keyboard behavior, and focus in a real native WebView, not just
DOM tests. No new page redesign or title/legend is part of this task.

## 7. Live provider acceptance

### Step 9: Prepare a small, explicit test fixture

Use a disposable test folder and existing eligible saved keys. Do not upload
unrelated personal notes. Use nested ordinary notes with a distinctive fictional
fact, an empty note, and duplicate titles where useful. Record service, actual
model ID, protocol, streaming mode, operation/resource IDs for cleanup, and result.
Keep credentials and document bodies out of logs. Use separate snapshots for the
two services, removing the first before switching if using the same folder.

The compatible configurations are direct OpenAI Responses at HTTPS
`api.openai.com:443` with empty base path, and direct native Google at HTTPS
`generativelanguage.googleapis.com:443/v1beta`, both with the store's saved key ID.
Subscription login, proxies, and Chat Completions are not store access paths.
Do not alter the user's unrelated model settings to force a pass.

Verify current official model/API contracts for file bounds, model support,
mixed-tool behavior, and any applicable request-store limit. The approved design
does not establish a universal numeric store-count limit. Historical deep-research
examples must not become a two-store cap on other models. If no current applicable
rule is verified, mark only known-limit cases not applicable; provider-rejection
and detach recovery still require evidence.

References available without the other design files:

- [OpenAI File Search](https://developers.openai.com/api/docs/guides/tools-file-search)
- [OpenAI retrieval limits](https://developers.openai.com/api/docs/guides/retrieval#limits)
- [OpenAI deprecations](https://developers.openai.com/api/docs/deprecations)
- [OpenAI Files deletion](https://developers.openai.com/api/reference/resources/files/methods/delete)
- [Google native File Search](https://ai.google.dev/gemini-api/docs/generate-content/file-search)
- [Google mixed-tool requirements](https://ai.google.dev/gemini-api/docs/generate-content/tool-combination)
- [Google store/upload/delete API](https://ai.google.dev/api/file-search/file-search-stores)
- [Google FileSearch schema](https://ai.google.dev/api/generate-content#FileSearch)

### Step 10: Exercise both services through the completed feature

For each service:

1. Upload the parent folder through the UI. Check progress and wait for every
   included note to finish indexing before success. Confirm the menu becomes
   Remove and no partial result is treated as ready.
2. Copy and submit its folder Ref ID as the entire input. Confirm attachment
   notice with no generation. Ask an ordinary question requiring the fictional
   fact and establish that the provider can retrieve the saved snapshot.
3. Edit/save the local fact. Verify the old store remains unchanged. Use a fresh
   conversation or another fact/query when needed to avoid treating an earlier
   answer in shared history as evidence of retrieval.
4. Remove, then re-upload the same folder. In the already attached conversation,
   verify request access now uses the new remote ID and retrieval reaches the new
   snapshot. Preserve the folder Ref ID and local notes.
5. Detach and confirm the next request carries no store. Do not infer this from
   the model forgetting a fact; inspect request behavior because chat history
   can still contain earlier answers.
6. Exercise missing/external-deletion behavior with test resources. If a suitable
   second-project credential is available, verify wrong-project deletion behavior
   without changing unrelated saved keys. Structured not-found must preserve the
   ID in a warning when clearing it; 401/403 must retain remaining cleanup.
7. Remove all test-created stores and owned files and confirm cleanup. Report any
   unknown or failed cleanup with its known identifiers. Delete local test data
   only after its required resource warnings/cleanup evidence have been retained.

Use the existing harness or narrowly scoped request capture to verify payloads;
do not log entire note content/secrets or add a permanent retrieval-debug UI.

### Step 11: Prove provider-specific continuation behavior

**OpenAI:** Exercise File Search alongside a CHA function through the existing
Responses continuation loop with `store: false`. Verify hosted output required
for replay is retained. Reach the local function-budget final round: only File
Search remains, with auto choice; hosted web search/custom functions stop. The
answer and usage still parse normally. Supplying a tool alone is not proof that
retrieval happened.

**Google:** Exercise native streaming and non-streaming File Search on the actual
selected model. On a supported mixed-tool model, require File Search and a CHA
web function in the same answer through continuation, preserving hosted parts,
function IDs, and thought signatures. Initial mixed mode is `VALIDATED` with
server-side invocation support. Reach the function budget and verify final `NONE`
disables custom functions while retaining File Search and signatures. Use existing
test controls where possible; do not add a permanent user budget setting just
to force this case.

Verify upload-operation completion versus store-count visibility: short active
count/pending documents stay Indexing, and every operation plus final expected
active/zero-pending/zero-failed counts must succeed. Live count lag may not occur
on demand; distinguish the observed live result from deterministic injected-lag
coverage. A successful upload alone does not validate native inference or mixed
tools.

Known models without the optional mixed combination should use File Search only,
with warning logs and accurate availability, without changing saved settings.
Unknown or unsupported combinations fail visibly. A rejected mixed final round
does not authorize silent retry, stripping signatures, dropping stores, changing
models, or switching to another protocol. That model/path remains unaccepted
until the required request behavior works.

If a required key/model/second-project setup is unavailable, complete independent
implementation/testing and report that live check as outstanding. Do not mark it
passed using a mock. Both providers' live flows belong to this block; they do not
require separate planned development blocks.

## 8. Complete acceptance matrix

The reviewer can use this table without opening the PRD. Evidence should identify
tests or observed live/native results; a checkbox without evidence is insufficient.

| PRD criterion | Behavior to accept | Evidence |
| --- | --- | --- |
| 1 | Real-folder Copy Ref ID and correct Upload/Remove action; stable ID across rename/removal/re-upload; root excluded. | Tree tests and native menu smoke. |
| 2 | Both services use eligible saved keys only; exact direct protocol/endpoint matching; shared-key configurations deduplicated; Chat Completions-only keys excluded. | Eligibility/client tests, choice UI tests, both live uploads. |
| 3 | One full saved Markdown document per nonempty ordinary descendant note; editable notes included; annotations/binaries/summaries/unsaved edits excluded; duplicate titles distinct. | Storage/export tests and saved-versus-unsaved UI flow. |
| 4 | Empty/preflight failures allocate nothing; any included-document failure prevents readiness; full indexing required; duplicate work rejected; cancel cleans known resources and never later publishes ready. | Worker/fault tests, dialog tests, native responsiveness. |
| 5 | Edits do not change a snapshot; Remove/re-upload captures fresh saved content with the same folder ref. | Both providers' live snapshot/replacement flows. |
| 6 | Whole-input attach without generation, idempotency, restart, Clear, cover/uncover, detach unavailable/repeated/invalid without history loss or remote changes; note/inline behavior retained. | Journal/input/controller tests and live attach/detach. |
| 7 | Each target receives only service/key-compatible current stores; other targets continue with combined omission notice; no corpus fallback or cross-provider direct access. | Matching/multicast tests and payload inspection. |
| 8 | Responses File Search and Google native streaming/non-streaming work with supported existing tools; mixed continuation/signatures/budget preserved; maintenance/auxiliary exclusion; known unsupported mixed tools omitted with warning; unknown rejection visible; no-store/Chat Completions unchanged. | Adapter/client regression plus actual OpenAI/Google continuation evidence. |
| 9 | Remove deletes all owned resources and keeps notes; new access stops; replacement is resolved by existing attachments with new remote ID. | Cleanup tests and both live removal/replacement checks. |
| 10 | Subtree cleanup includes own/descendant/incomplete stores and empty folders; offline/missing-key/access/partial failures preserve local data; Retry/Cancel/Delete anyway semantics and atomic warning; no later cleanup scheduled. | Backend deletion/fault tests, UI states, editor/native smoke. |
| 11 | Interrupted upload/removal reopens incomplete/removable without automatic resume; lost allocation response reports uncertainty honestly. | Worker/reopen/lifecycle fault tests. |
| 12 | Missing/replaced/deleted keys, external store deletion, vault switch, safe errors, unchanged unrelated requests and ordinary note attachments. | Credential/lifecycle/regression tests and available live deletion checks. |
| 13 | A verified current limit is enforced before new attachment and per-target dispatch without damaging history; unexpected provider count rejection has detach recovery. No invented universal limit. | Applicable known-limit tests or explicit not-applicable reason; injected provider count rejection and detach recovery in all cases. |

Also verify child-vault copies clear remote ownership while keeping copied refs;
password protection/restore preserve same-vault ownership; Welcome refs cannot
cross vault identities; not-found warnings commit with ID removal; late workers
cannot write to a new vault; dialog reload recovers reservations. Existing Events
and logs have finite retention. Restore may expose stale records; no automatic
discovery/reconciliation or permanent recovery registry is part of acceptance.

## 9. Independent review and final handoff

Review the actual block diff and integration changes against their starting
revision. Inspect native/client contracts and editor ownership alongside the UI.
Re-run representative deterministic tests and assess live evidence separately.
In addition to the matrix, check these failure-prone points:

1. An operation accepted by the backend is never shown as finished or used to
   dispose the editor. Success follows committed backend terminal state.
2. Close/Escape/reload cannot discard active cleanup, strand a failed reservation,
   duplicate remote allocation, or mutate a new vault through stale callbacks.
3. Failed deletion preserves editor content/save behavior; warning completion
   still handles local deletion correctly; Retry/Delete anyway use the same
   failed identity and correct save guards.
4. Menus include incomplete records and empty-folder stores; confirmation counts
   cover descendants and refresh on mismatch without automatic deletion.
5. Errors distinguish preflight, known cleanup failure, and uncertainty. Delete
   anyway is exclusive to failed folder deletion and warns about local loss and
   remaining remote data/charges.
6. The approved UI remains small, accessible, and responsive, with no panel
   titles, extra explanatory copy, raw keys, remote-ID badges, or permanent polling.
7. Live evidence proves retrieval and continuation on actual named models, not
   just upload or a plausible answer from conversation history. Test-owned
   resources are cleaned up or unresolved IDs are explicitly reported.
8. Cross-block fixes preserve the simple architecture and relevant regressions;
   no speculative limits, silent fallback, duplicate state, or new lifecycle
   policy was added to make a test pass.

Report actionable defects with severity, file/line, concrete trigger, impact,
and required behavior. Separate unavailable external checks from code failures.
Do not infer a live pass from mocked tests or claim support for untested models.

The final handoff must include changed behavior, exact check commands/results,
native platforms checked, acceptance-matrix gaps, actual live models/protocols,
and cleanup outcome. Full feature acceptance requires passing applicable automated
checks, successful required live retrieval/continuation flows for both providers,
and confirmed test-resource cleanup. If any prerequisite prevents that, state
what is implemented and what remains unverified or unresolved; do not mark the
entire feature complete.
