# Folder stores: technical design

Date: October 27, 2026

Status: Technical design for implementation

Requirements: [store-prd.md](store-prd.md). This document defines the implementation;
it does not implement the feature or change the PRD. The UI uses the PRD's sketches
and existing dialog styles.

## 1. Design decisions

A store is a provider-hosted snapshot owned by one real folder in one vault.
The folder's existing item ID supplies its identity and Ref ID. A conversation
remembers folder IDs and resolves their current stores for each ordinary answer.
No uploaded text is added to the local conversation prompt.

Use the current SQLite vault, note-to-Markdown exporter, saved API keys, session
journal, background workers, and native provider adapters. Add only the data and
operations required to upload, use, and remove snapshots. Transfer documents in
sequence, then poll their indexing together; no parallel upload workers are needed.

The important invariants are:

- One folder has at most one store record. Only `ready` records grant access.
- Every included note must finish indexing before the record becomes ready.
- Persist resource IDs before starting the next remote step. Failed or interrupted
  work remains removable.
- Cleanup makes the store unavailable before making remote requests. Local notes
  survive cleanup failures unless the user explicitly chooses **Delete anyway**.
- Requests match by service and saved key ID, with a direct endpoint and the
  correct protocol. A model change never changes the protocol implicitly.
- Folder attachments outlive store replacement and cover/uncover. Clear and
  detach remove attachments; neither deletes remote resources.
- An operation's cancellation or vault interruption cannot later publish success.

There is no synchronization index, copied corpus in SQLite, account discovery,
expiry policy, resumable job system, cleanup daemon, or new provider setting.

## 2. Existing code and changes

| Area | Existing implementation | Required change |
| --- | --- | --- |
| Notes and export | `src/storage/note_storage.{h,cpp}`; `note_items`, `note_document_markdown`, `note_get_text` | Read a consistent ordinary-note subtree and produce full-text upload documents. Guard recursive deletion against outstanding stores. |
| Vault persistence | `src/workspace/workspace_config_store.{h,cpp}` | Expose store operations under its existing database mutex and transaction conventions. |
| Session attachments | `src/storage/session_database.{h,cpp}`; `session_notes`, `session_files` | Add folder-ID associations and transactional attachment/detachment notices. Do not extend `notes_prompt()` with folder text. |
| Input and generation | `src/runtime/text_input.cpp`, `src/session/session_open.cpp`, `src/session/session_controller.cpp` | Resolve item kinds, handle `/detach`, select eligible stores per target, and supply request-local store access. |
| Remote store operations | New `src/providers/folder_store_client.{h,cpp}` | Small OpenAI/Google create, upload, inspect, and delete functions using libcurl and existing curl ownership helpers. |
| Operation coordination | New `src/app/folder_store_operations.cpp`; `application_internal.h`, `BackgroundJobs` | Explicit upload/removal workers, progress, cancellation, subtree-delete coordination, and vault-lifetime checks. |
| Inference | `src/characters/model_context.h`, `src/providers/responses_api.cpp`, `google_api.cpp`, `provider_client.cpp`, `tool_calls.cpp` | Carry IDs, append hosted tools, preserve them during function continuation, and keep tool instructions correct. |
| Desktop bridge and UI | `src/bridge/notes_dispatch.cpp`, `bridge_protocol.*`, `webapp/src/components/NotesTree.tsx`, `webapp/src/api/{client,nativeClient}.ts`, `resources/dto.yaml` | Folder actions, operation methods/status, upload dialog, removal and folder-delete states. |
| API-key usage | `src/app/settings_operations.cpp` | Add folder-store usage to existing `used_by` values, including incomplete stores. |

Put the small store schema and queries in `src/storage/folder_store_storage.{h,cpp}`
to avoid further expanding note storage. Register new sources and focused tests
in `CMakeLists.txt`; regenerate `webapp/src/api/schema.d.ts` from the DTO source.
No new SDK or general HTTP framework is required. `ProviderClient` remains the
inference client; its model-POST test transport is not a store-management API.

The desktop notes bridge owns the new management UI. ChaWeb currently has no
notes-management API, so this feature does not introduce one. Shared chat input
and generation paths must still handle folder refs and `/detach` consistently.

## 3. Persistence

### 3.1 Store ownership

Add the following tables through an idempotent schema helper. Call it from
`create_workspace_session_schema`, after the note schema, and from
`WorkspaceConfigStore::Impl::open_database` in its existing schema transaction.
The first call also covers `create_session_database`, including Welcome and test
journals; Clear must work even if no folder was ever attached. This follows the
current additive schema convention. Do not rewrite notes. Existing vaults start
with no stores or folder attachments.

```sql
CREATE TABLE IF NOT EXISTS folder_stores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    folder_id INTEGER NOT NULL UNIQUE
        REFERENCES note_items(id) ON DELETE RESTRICT,
    service TEXT NOT NULL CHECK (service IN ('openai', 'google')),
    api_key_id TEXT NOT NULL,
    remote_store_id TEXT,
    state TEXT NOT NULL CHECK (state IN ('incomplete', 'ready')),
    note_count INTEGER NOT NULL CHECK (note_count > 0),
    uploaded_at INTEGER,
    remote_outcome_unknown INTEGER NOT NULL DEFAULT 0
        CHECK (remote_outcome_unknown IN (0, 1)),
    CHECK (state <> 'ready' OR
        (remote_store_id IS NOT NULL AND uploaded_at IS NOT NULL
         AND remote_outcome_unknown = 0))
) STRICT;

CREATE TABLE IF NOT EXISTS folder_store_files (
    store_id INTEGER NOT NULL REFERENCES folder_stores(id) ON DELETE CASCADE,
    file_id TEXT NOT NULL,
    PRIMARY KEY (store_id, file_id)
) STRICT;

CREATE TABLE IF NOT EXISTS session_folders (
    session_key INTEGER NOT NULL REFERENCES sessions(session_key) ON DELETE CASCADE,
    folder_id INTEGER NOT NULL CHECK (folder_id > 0),
    PRIMARY KEY (session_key, folder_id)
) STRICT;
```

Validate that `folder_id` is a real `kind = 'folder'` item in the write operation.
The synthetic root has no row and cannot pass validation. Store IDs are local
attempt identities: deleting and re-uploading a folder allocates a new `id`, so a
late completion cannot modify its replacement. Compare this ID on every update.

`folder_store_files` holds only OpenAI Files resources owned by this upload,
including files whose attachment/indexing failed. It is a cleanup list, not a
note-to-file index. Remove an entry after confirmed deletion or acceptable
not-found, preserving the latter's warning as section 7.3 specifies.

Google operation names stay with their documents in worker memory until indexing
finishes. Log returned resource names for diagnostics, but do not add a persistent
operation column: work never resumes, and deleting the known store covers all its
documents. No upload URLs, API-key values, note bodies, or progress counters enter
these tables.

`remote_outcome_unknown` records a concrete crash/lost-response risk. Commit it
before a request that can allocate a new store or independently owned file; clear
it in the same transaction that saves the returned ID, or after a definite
rejection known to have created nothing. If the process exits between those
steps, reopening cannot falsely conclude that no remote resource exists. Stop
after the first ambiguous allocation, so one bit suffices. For Google uploads,
a known containing store makes force-deletion sufficient even if an upload's
operation name was lost. This is failure bookkeeping, not an idempotency promise.

### 3.2 Conversation lifetime and Welcome

`session_folders` deliberately has no foreign key to `note_items`, matching the
cross-database constraint of existing note attachments. Persistent sessions use
the vault database; Welcome has a private temporary journal and validates newly
submitted references against the active vault. A folder foreign key would fail
in Welcome.

For persistent sessions, use the metadata callback for initial item-kind dispatch,
then recheck existence/readiness through a transaction-neutral query from
`folder_store_storage` on the **journal's own connection** inside
`SessionJournal::add_folder`. Insert the association and notice in that same
`BEGIN IMMEDIATE` transaction. Do not call `WorkspaceConfigStore` while holding
this write transaction: its connection cannot participate in it. This small
recheck serializes attach with deletion and avoids leaving dangling rows.
Duplicate attachment does not change the association; return an existing-style
notice. Detach deletes the association and records its notice in one transaction.
It does not require a ready store; use the Ref ID if the folder name is unavailable.
Repeated detach reports that it is not attached. Do not delete earlier notices.

Extend `SessionJournal::clear_history()` to delete `session_folders` along with
`session_notes` and `session_files`. Session deletion already cascades by
`session_key`. Cover/uncover does not touch attachments. Read attached folder IDs
from the journal when needed; do not maintain a second controller cache.

Local folder deletion explicitly deletes affected `session_folders` rows in the
same vault transaction as the subtree. After commit, send a command through the
existing runtime to prune Welcome's temporary associations; do not mutate its
journal on an application worker. Welcome uses the existing callback to validate
an attachment because its journal has no source folders. Its attach and prune
commands run on the same runtime: a deletion committed after validation queues
pruning after the current attach command, closing that race. Do not add a
per-request pruning pass. Carry the originating context epoch so a delayed prune
cannot remove associations made after a vault switch.

Welcome's only answering member is the maintenance Assistant. Skip folder-store
resolution entirely for those answers. The temporary journal survives a vault
switch, so also clear **only its folder associations** when
`SessionRepository::MaintenanceGuard::retarget` changes database paths, while
live controllers are already drained. Keep its transcript and existing note/file
copies. A password change that retargets the same path must not clear associations.
This prevents duplicate attach/detach from interpreting a vault-A ID in vault B;
simply never retrieving from Welcome would not fix that identity problem.

### 3.3 Transaction boundaries

Use short transactions for registration, saving each returned ID, marking ready,
marking incomplete, recording each successful deletion, and final removal. Never
hold a database transaction or the application lifecycle mutex over network I/O.

Commit ready state and its success event together. Commit final store-record
removal and its outcome event together. For **Delete anyway**, record all
unconfirmed resources and delete local records/notes/associations in one vault
transaction using `event_append_in_transaction`. Refactor `note_delete` into a
transaction-owning wrapper and an internal transaction-neutral helper as needed;
do not nest transactions or lose its existing annotation/audio cleanup behavior.

The `ON DELETE RESTRICT` store relationship is a final guard against an old or
unexpected local-delete path silently abandoning remote resources. The normal
delete path must remove store rows through the specified cleanup flow first.

Database backups naturally include these tables. Configuration-only exports do
not become a store-transfer mechanism. Reopening or restoring a database never
automatically contacts providers or resumes incomplete uploads.
Configuration import/merge keeps existing store ownership rows and key IDs;
changed key values can produce access errors **or not-found responses**. Apply
the warning rule in section 7.3 in both cases. Do not import another vault's store
ownership through the configuration merge path or silently select another key ID.

**Create vault from a source** is different from backup/restore. After
`VaultMaintenance::create_vault` copies the source database, clear the destination's
`folder_store_files` and `folder_stores` in one local transaction before publishing
the new vault. Keep its copied `session_folders`: they refer to the copied folders
and remain unavailable until the child vault uploads its own stores. Make no
remote requests. Handle older source databases without these tables, and remove
the new destination on failure rather than publish shared ownership.

Do this in the new-vault creation path, **not unconditionally inside
`copy_workspace_session_database`**. `protect_workspace_session_database` also
uses that helper to encrypt the same vault; password protection must preserve
remote ownership. Backups/restores likewise retain ownership. Test child-vault
copy and same-vault password protection separately.

## 4. Credentials and matching

Implement one small eligibility helper shared by the upload choices and request
resolver. It returns an optional service and resolved saved-key ID, or a reason
why store access is unavailable. Check actual connection fields, not provider
labels or model-name prefixes.

| Service | Required connection |
| --- | --- |
| OpenAI | `mode = net`, API-key authentication, `api = responses`, HTTPS, host `api.openai.com`, port 443, empty `base_path` (the current URL builder appends `/v1/responses`). |
| Google | `mode = net`, API-key authentication, `api = google`, HTTPS, host `generativelanguage.googleapis.com`, port 443, `base_path = /v1beta`. |

Normalize host case as existing helpers do, but do not accept suffix matches,
custom proxies, alternate ports, subscription endpoints, or compatibility API
paths. Store-management URLs are fixed for each supported service. Never build
them from an arbitrary model endpoint.

The upload dialog gets provider choices and eligible saved-key IDs/names from
the backend. Inspect all compatible configured model connections, resolve any
supported legacy key-name reference once through `ApiKeyStore`, and deduplicate
by stable key ID. Require a currently saved nonempty key. A key can appear under
both services if both types of compatible configuration reference it; CHA does
not infer accounts from key text. One choice is selected automatically; multiple
choices require selection. Zero choices produce the PRD's settings error.

Revalidate at submission. Store the key ID, never the provider configuration ID
or key value. Removal uses the service/key in the store record even after all
model configurations that used it have changed or been deleted. Resolve the
key's current value before each remote operation, under existing vault guards.
Do not substitute another key if lookup fails.
Validate only settings relevant to the operation. Unused or obsolete model
settings must not block store creation/removal; ignore them with a log warning.

Keep key rename, replacement, and deletion behavior unchanged. Build API-key
`used_by` details from current store rows as well as workspace configuration, for
example `Folder store: Research (OpenAI)`. No key foreign key or deletion veto is
introduced. Different saved IDs do not match even when values happen to match.

## 5. Capturing the snapshot

On Upload, reserve the folder operation, validate credentials, and capture all
saved ordinary notes in one SQLite read transaction. Traverse descendants with
the existing recursive-CTE pattern. Read folder names, parent links, note names,
and saved documents from that same snapshot. Do not combine a tree listing with
later independent reads of each note.

Export each `kind = 'note'` through `note_document_markdown`, using the full saved
document, never `prefer_compressed = true`. The current note-reference callback
does prefer compressed text; it is unsuitable for this export. Exclude annotation
rows explicitly. Do not ask the editor to save or send its current document.

Check emptiness before adding metadata, using the saved document's exportable
text so empty headings/code fences do not turn an empty note into a document.
A whitespace-only body is skipped; image alt text and comments follow the existing exporter. If
there are no nonempty bodies, fail before remote creation. If export of any
nonempty note fails, fail the whole upload and identify that note.

Each UTF-8 Markdown document has this form:

```markdown
# Note: Experiment log

Folder: Research/Experiments
CHA Ref ID: ##00000B

<full exported body>
```

The folder path is relative to the vault's notes root and reflects capture time.
Sanitize filename separators/control characters and use a bounded readable title
plus the complete numeric note ID, such as `Experiment-log-11.md`. Duplicate
titles in different folders therefore stay distinct. Escaping metadata must not
alter the note body. Use `text/markdown`; upload no image/audio binaries, editor
JSON, imported originals, or link targets.

The worker owns an in-memory vector of exported documents until the operation
finishes. This is the required captured snapshot, not a persistent cache. Release
SQLite locks before upload. No temporary on-disk corpus is needed. Allocation or
export-size failure is an explicit preflight error; never silently drop notes.

Keep a simple final-byte-size comparison, including metadata, before creation.
The saved JSON limit is 12 MiB, but that is not a proven bound on exported
Markdown plus the separately stored folder path; formatting can expand text.
This check needs no new framework. OpenAI documents 512 MB and five million
tokens per file; Google documents 100 MB per document. No per-store file-count
limit was verified for these current APIs. Count, token-indexing, and account
quota rejections therefore remain provider errors, naming the current note.
Do not add a tokenizer, speculative count-limit helper, or local total quota.
[OpenAI retrieval limits](https://developers.openai.com/api/docs/guides/retrieval#limits),
[Google File Search limits](https://ai.google.dev/gemini-api/docs/generate-content/file-search#rate-limits).

Once captured, edits, renames, moves, and deletions of ordinary source notes do
not modify the upload. Parent and child stores capture independent snapshots.
The operation must protect its owning folder from deletion; it need not freeze
all source note edits for the duration of network work.

## 6. Explicit operations and cancellation

Use `BackgroundJobs` to run store operations. Add a small application-owned map
of active controls guarded by `lifecycle_mutex`, following note compression's
ownership pattern. Each control contains the context epoch, folder/root ID,
store-record ID where applicable, operation identity, phase, completed/total
counts, sanitized failure, and cancellation state. It is not a persisted queue.

Admission rejects duplicate store operations on a folder. A folder-delete
operation reserves its subtree and rejects overlapping deletion and store
operations, including ancestor deletion of an uploading folder. Recheck current
ancestry on admission. While a delete attempt is active, prevent structural
mutations that change its subtree membership. Existing note saves need not be
blocked. Folder rename/move after an operation finishes retains identity.

The backend remains authoritative even if the UI disables buttons. A request
with a stale epoch or operation identity cannot cancel, retry, or finalize a
newer operation. Use existing main-process mutation admission where required.

### 6.1 Upload sequence

1. Reserve the folder and launch preparation off the UI thread. Validate and
   capture the snapshot. If validation fails, release the reservation and keep
   the dialog editable with its error; no remote request has occurred.
2. Insert the `incomplete` store record with the selected service/key and note
   count. If registration or worker launch fails, do not create anything remotely.
3. Create the remote store, recording uncertainty before the call and its ID
   immediately afterward. Use a readable name containing the folder name and
   Ref ID to help manual identification; names are not lookup identities.
4. Transfer documents sequentially and attach OpenAI files as their IDs arrive.
   Keep each file/operation ID and terminal result with its existing in-memory
   export entry. Do not wait for one note's indexing before uploading the next.
   Once uploads finish, poll unfinished entries in rounds, with one interruptible
   wait between rounds. Check immediate terminal errors as responses arrive.
   Publish phase/count changes; indexing must finish for every entry.
5. Under the operation/lifetime guard, confirm that all documents succeeded,
   cancellation is still false, and the original record remains current. Commit
   `ready`, upload time, and success event together. Only then report success.
6. On failure or cancellation, keep the record incomplete and enter the same
   cleanup routine used by Remove. Retain every remaining identifier on failure.

Persistent state has only `incomplete` and `ready`. UI phases such as preparing,
uploading, indexing, cleaning up, removing, completed, and failed belong to the
active control. They are not additional durable job states.

### 6.2 User cancellation

Before submission, Cancel closes the dialog. During preparation/upload/indexing,
Cancel atomically prevents further forward work and readiness, cancels the active
non-allocating transfer, and wakes any polling wait. Let an already-sent store/file
allocation or Google upload/finalize request finish within its normal timeout so
CHA can learn its resource ID and clean it up. Start no next forward step.
Shutdown/vault interruption can abort these transfers too. Cancellation and the final
ready commit serialize through the same operation guard; if ready committed
first, the upload has already completed and a later cancel is a no-op.

Still parse and persist resource IDs in a response received after cancellation.
Then clean them up. Do not return early merely because the cancel flag is set.
Cleanup needs its own cancellation predicate: the user's upload-cancel flag
stops forward work but must not immediately abort the cleanup requests. The
existing worker interruption/shutdown signal can stop both. Do not reset a
cancelled upload flag to run cleanup.

Show **Cleaning up…** while this runs. Confirmed cancellation cleanup closes
the dialog; not-found or uncertainty keeps section 7.3's warning/Close state.
Failed cleanup exposes Close and retains Remove. Normal upload
failures remain visible even if cleanup succeeded; reopening Upload captures a
fresh snapshot. Do not retry a create/upload automatically after an ambiguous
response, timeout, or cancellation.

### 6.3 Shutdown, vault changes, and local failures

Integrate store interruption into `VaultMaintenance::drain_for_maintenance` and
application shutdown, using note audio's existing cancellation/epoch pattern.
While holding `lifecycle_mutex`, flag all store controls interrupted and retire
their admission reservations. Do not wait for these workers in vault drain and
do not unlock the maintenance caller's `lock_guard`. Keep current switch, merge,
R2, and vault-deletion lock structure. No new worker drain or timeout is needed.

Before each new network step and before each vault write, a worker takes
`lifecycle_mutex` and checks admission, captured epoch, control identity, and
store-record ID. User cancellation still permits bookkeeping/cleanup in the same
vault; lifecycle interruption does not. If any lifetime check fails, stop without
touching the current vault or starting cleanup. Do not reopen an old database by
path from a stale worker. An in-progress removal was marked incomplete before its
first request, and an uncertain allocation was marked before sending.

Log any resource ID returned after interruption, with its original vault/folder,
service, and key ID, for manual cleanup. Never write it to the new vault. The old
record remains incomplete; a lost OpenAI allocation ID is represented by its
precommitted uncertainty flag, and a known Google store still covers its documents.
On reopen, use explicit Remove, never automatic resume or resubmission. Shutdown
retains worker dependencies through the existing `BackgroundJobs` deadline policy.

Log returned IDs before attempting the vault write. If that write fails, keep the
IDs in the active control, stop forward work, and attempt cleanup only while its
lifetime guard still passes. Do not report ready or discard the record. If both
persistence and cleanup fail, show the IDs in the error and attempt a warning
event. The diagnostic log is a best-effort recovery aid, not a second registry;
database/log failures can still lose an ID. The precommitted uncertainty flag
makes a subsequent restart report that limitation honestly.

## 7. Provider store clients

The store client accepts immutable request inputs, cancellation, and a small
injected HTTP transport for tests. It knows no sessions or UI. The application
coordinates persistence between its remote calls. Reuse `CurlHandle` and
`CurlHeaders`, TLS verification, cancellation callbacks, and the project's
timeout/error conventions. Do not hold credentials in operation-status DTOs.

Use bounded requests and interruptible polling waits only while the explicit
upload is active. Honor a provider polling interval when supplied; otherwise a
short fixed interval such as the documented Google five-second example is enough.
Polling has no persisted schedule. Do not introduce automatic retries for
resource creation or replay uploads after uncertain failures.

### 7.1 OpenAI

Authenticate with `Authorization: Bearer <saved key>` on the fixed
`https://api.openai.com/v1` origin.

| Step | Request and result |
| --- | --- |
| Create | `POST /vector_stores` with `name`; save `id`. Omit expiry/chunking settings. |
| Upload each note | Multipart `POST /files`, `purpose=assistants`, one Markdown file; save the returned file ID before attachment. |
| Attach | `POST /vector_stores/{store_id}/files` with `file_id`. |
| Index | After transfers, poll pending files with `GET /vector_stores/{store_id}/files/{file_id}` until every one is `completed`; `failed` or `cancelled` fails this upload. Report the note and safe error. |
| Remove | Delete the vector store and every recorded `/files/{file_id}`. Track each result independently. |

An upload HTTP success is not index completion. Use individual file status,
avoiding a new batch-state model or dependence on a paginated list. Hosted
retrieval is performed by Responses, not CHA.
[OpenAI File Search setup](https://developers.openai.com/api/docs/guides/tools-file-search#how-to-use).

Delete the vector store first, then all owned files; attempt the remaining known
deletions even if one fails. Deleting only vector-store membership does not meet
CHA's cleanup requirement. Files deletion also removes the file from vector
stores, so files that never attached are covered by the same cleanup list.
[OpenAI Files deletion](https://developers.openai.com/api/reference/resources/files/methods/delete).

### 7.2 Google

Authenticate with the existing `x-goog-api-key` header on
`https://generativelanguage.googleapis.com`. Create with
`POST /v1beta/fileSearchStores`, then upload directly to
`POST /upload/v1beta/{fileSearchStoreName}:uploadToFileSearchStore` using the
documented media-upload flow. Send the Markdown MIME type and readable display
name. Use provider-default embeddings and chunking.

Keep/log each returned operation name, then poll unfinished operations beneath
`/v1beta/` after transfers finish. Require terminal completion without `error` for
every document before readiness.
For upload operations this is the
`fileSearchStores/{store}/upload/operations/{operation}` resource. Validate that
returned names belong to the expected store and origin. Any upload-session URL
is secret transport state, retained only in memory and never logged.

After every document operation succeeds, verify the fresh store has the expected
active count, zero pending, and zero failed documents before readiness. Parse the
API's decimal-string counts explicitly. Do not assume counts become consistent
with operations immediately: a short count or pending documents keeps Indexing
active and is polled again. A failed document or excess active count fails the
upload; a transient short count does not. Cancellation remains available. Remove
using `DELETE /v1beta/{store_name}?force=true`.
[Google store/upload API](https://ai.google.dev/api/file-search/file-search-stores).

Do not use the separate Files upload/import path, store a second set of raw-file
IDs, or add an Interactions adapter. The existing Generate Content protocol is
the requested inference boundary, even though Google's general File Search guide
now also describes another API.

### 7.3 Cleanup results and uncertainty

For either provider, confirmed deletion or an authenticated, structured not-found
result for the expected resource completes **that known resource's cleanup**,
as the PRD specifies. A generic 404 page, malformed response, 401/403, network
error, or local write failure does not. Persist each completed resource before
moving on, clearing its ID; retries act only on remaining resources.

Not-found is not proof that the original project no longer holds the resource.
A valid key for another project may produce the same response. Exact codes are
provider-dependent and need live verification. Whenever a resource is not found,
write a warning event containing its remote ID, service, saved key ID, and folder
in the **same transaction that clears the ID**. Also log it. Explain the possible
project mismatch and manual-cleanup route. This preserves evidence even if the
process stops before the terminal outcome event. It covers manual key replacement
and a configuration merge that changes the value behind an existing key ID.

Use the same finalization rule for Remove, upload-failure cleanup, and folder
deletion: once every known resource was deleted or returned acceptable not-found,
the caller may remove the store record. Remove restores Upload; folder deletion
may delete its local subtree. If an allocation outcome is unknown, save a warning
in the same transaction as that final local change. No automatic discovery or
meaningless retry of an unknown ID is possible. Keep the warning visible in the
dialog with Close instead of claiming remote deletion was proved.

An operation that observed not-found or an unknown outcome ends with a warning,
listing the affected IDs where known. A resumed cleanup must not turn earlier
warnings into a claim that all remote data was deleted: describe the terminal
event as local association removal/known-resource cleanup, with neutral severity
when earlier outcomes cannot be established. Reserve full deletion-success
wording for confirmed deletions. If any known resource still failed cleanup,
retain its incomplete record; folder deletion offers Retry/Cancel/Delete anyway.
There is no uncertainty-only Retry loop or new orphan registry.

## 8. References and request resolution

### 8.1 Input handling

Generalize `parse_note_reference` into an item-reference helper, retaining the
base62 alphabet and whole-input gesture. Dispatch by the current item's kind:
ordinary notes keep their text-snapshot path; folders use `add_folder`; generated
annotation behavior remains as it is today. Unknown IDs fail normally. A folder
without a ready store returns the PRD's upload-first notice without generation.

Reuse `webapp/src/notes/reference.ts` for Copy Ref ID and keep the current C++
six-base62-digit parser unchanged apart from its item-oriented name. Extending
the ID width is outside this feature. Longer or embedded strings retain today's
ordinary-text behavior. The existing formatter/parser width mismatch at very
large IDs is a separate limitation, not part of this implementation.

Add `CommandKind::detach` and its descriptor to `text_input`. Require exactly one
valid Ref ID and otherwise return `Usage: /detach <Ref ID>`. It removes only a
folder association, even if its store/key is missing or incompatible. It makes no
provider request, does not clear history, and does not modify note attachments.
Keep existing submission-expiry and generation-in-progress guards.

Extend the narrow callbacks from `session_open.cpp` for initial reference metadata
and generation-time folder-store lookup through `WorkspaceConfigStore`.
Persistent attachment insertion additionally rechecks on the journal connection,
as section 3.2 specifies. Controllers use no HTTP and own no store client.

### 8.2 Compatibility and limits

At attachment time, treat an existing association as idempotent and require a
ready store for a new folder. A folder with no currently matching character can
still be attached for later use.

As of this review, no numeric request-store limit was verified for the currently
supported model/API combinations. OpenAI's two-store example belongs to
`o3-deep-research` and `o4-mini-deep-research`, shut down July 23, 2026. The guide
now explicitly labels those examples historical. Its replacement recommendation
does not establish the same limit for the replacement model. Google's schema
also supplies no maximum. [OpenAI deprecations](https://developers.openai.com/api/docs/deprecations),
[historical deep-research guide](https://developers.openai.com/api/docs/guides/deep-research),
[Google FileSearch schema](https://ai.google.dev/api/generate-content#FileSearch).

Therefore add no speculative limit helper, attachment-time count scan, or
`ProviderRequestInput` preflight-error field. Keep the PRD's conditional rule:
if an applicable limit is verified before release, enforce that specific limit
before a new attachment and again per target at dispatch. Reject only the new
attachment/affected target; count distinct ready remote IDs for its service/key.
Do not impose the historical two-store limit on current models. Without a
verified rule, acceptance criterion 13's known-limit cases are not applicable;
its provider-rejection and detach recovery cases remain mandatory. Recheck the
actual supported models during the live validation, without a monitoring service.

At each ordinary answer's dispatch, load current attached folders and their
current records, then resolve separately for each target's effective connection.
This point follows recipient selection and includes multicast children. An
ineligible/missing/incomplete store produces a concise omission reason and the
target continues normally with any remaining eligible stores. Combine omissions
into one notice for the turn, grouped by target when necessary. Name the folders
and applicable detach commands; do not copy source text as a fallback.

Deduplicate eligible remote IDs in each target's request. Provider count/access/
model failures remain that target's normal request errors, augmented with the
supplied folders and detach commands. They do not abort other multicast targets.
Never guess a subset, retry without stores, or change models. This error path
also covers a formerly usable set after replacement upload or configuration edit.

### 8.3 Request data and exclusions

Add a small optional value to `GenerationRequest`, for example:

```cpp
struct FolderSearchAccess {
    StoreService service;
    std::vector<std::string> remote_store_ids;
};
```

Populate it only after resolution; keep folder labels/refs with the request's
error context, not in its protocol payload. IDs are immutable for that logical
answer and reused across its continuation calls. No provider client performs a
fresh database query halfway through an answer. A removal blocks newly resolved
answers, but an answer that already captured access may finish using it.

Keep this field empty for recipient detection, session naming, compression, and
other auxiliary requests. Explicitly exclude the Welcome Assistant maintenance
case already detected in `start_generation`; being a chat answer or having
`include_tool_instructions = true` is not sufficient eligibility. Builders also
give maintenance tools precedence defensively. Ordinary requests without eligible
stores keep their current behavior.

## 9. Inference adapters and continuation

### 9.1 OpenAI Responses

Append one tool to the existing tool list:

```json
{"type":"file_search","vector_store_ids":["vs_example"]}
```

Keep supported provider-hosted web search and CHA web functions. Preserve the
existing initial `tool_choice` (`auto` or `required`) and its continuation behavior.
The current broad `required` choice already permits CHA functions to satisfy it;
adding File Search does not justify a separate strict-hosted-search change.
Do not introduce an unverified hosted-tool choice or claim that a supplied store
was searched. Strict hosted-web-search selection is outside this feature.

Preserve provider output items needed for continuation. File-search output is
hosted-tool context, not a CHA `ToolCall` to execute. Reuse normal answer/usage
parsing; no file-citation UI or retrieval-results request is added.

### 9.2 Native Google

Append a File Search tool to the existing native request:

```json
{
  "tools": [
    {"fileSearch":{"fileSearchStoreNames":["fileSearchStores/example"]}}
  ]
}
```

Keep the selected model, endpoint, context, supported generation settings,
streaming choice, saved-key authentication, cancellation, and usage handling.
`api = chat_completions` never enters this builder, including on Google's
compatibility endpoint.

For documented mixed-tool models, keep CHA `web_search`/`web_read` declarations
and set:

```json
{
  "toolConfig": {
    "includeServerSideToolInvocations": true,
    "functionCallingConfig": {"mode":"VALIDATED"}
  }
}
```

Google documents this combination for Gemini 3 and 4 as a preview, and lists File
Search among the supported server tools. Preserve all returned model content
parts, including hosted `toolCall`/`toolResponse` parts, IDs, and thought
signatures; return custom `functionResponse.id` unchanged. The current decoder
and continuation loop already retain these parts and function IDs, so extend and
test them instead of creating a second loop.
[Google tool combinations](https://ai.google.dev/gemini-api/docs/generate-content/tool-combination).

Use a small model-capability helper for this tool-combination decision, separate
from service/credential matching. Gemini 2.5 models known to lack the combination
receive File Search without optional CHA functions, with a diagnostic warning.
Documented Gemini 3/4 models use the mixed form. Unrecognized models use the mixed
form if functions were requested and surface any rejection; do not silently
disable tools based on an unverified guess. Unsupported File Search itself also
fails visibly. Saved settings are never rewritten.

Google's File Search guide separately discusses incompatibility with built-in
Google Search/URL Context while also documenting custom-function combinations.
CHA's web tools are custom functions. The live mixed-tool check must verify the
exact request used here; a successful upload or File-Search-only answer does not
validate this combination.
[Google supported combinations](https://ai.google.dev/gemini-api/docs/generate-content/file-search#supported-tool-combinations).

### 9.3 Function-budget handling

Fix two current assumptions in `tool_calls.cpp`: Google tools are not all
`functionDeclarations`, and declarations need not occupy the first tool entry.
Inspect optional members when adding functions and calculating availability.
Derive web and folder-search availability independently from the serialized
request. Do not announce a search as performed merely because it is available.

When the existing CHA function-call budget is exhausted:

- Responses requests with stores remove every tool except `file_search` and use
  auto choice. Hosted web search still stops at this point, as it does today.
- Google requests with stores retain `fileSearch`, preserved continuation parts,
  and server-side invocation support; set function-calling mode to `NONE` to
  disable custom calls without disabling hosted retrieval.
- Recompute the tool-availability block. The final-answer directive must say
  that CHA web functions are unavailable; it must not forbid all further tools
  while File Search remains available.

Keep the existing budget behavior for requests without stores. Hosted retrieval
does not consume CHA's local function budget. Add serialization and continuation
tests for both protocols, including Google mixed streaming responses. The
documented mixed mode is `VALIDATED`; compatibility of `NONE` with these specific
parts still requires a live check. If Google rejects this final round, report the
request error and keep the attachment. Do not silently retry, strip signatures,
drop File Search, change protocols, or switch models. That model's mixed-tool
path fails acceptance until a tested continuation satisfies the required behavior.

## 10. Removal and folder deletion

Remove reserves the folder, changes ready to incomplete, and invokes the shared
cleanup routine. Its credential comes from the record. Confirmed resources are
removed from the cleanup list as work proceeds. A failure leaves local notes and
remaining identifiers intact, and the dialog supplies an actionable reason.
Retry starts from those identifiers. Once known cleanup completes, remove the
record using section 7.3's outcome/warning rule; conversation folder associations
remain available for a future upload.

Folder Delete must collect stores owned by the selected folder and all descendant
folders before invoking `note_delete`. Ancestor stores outside the deleted
subtree are not touched. Count incomplete stores too, including ones with no
returned remote ID. An empty folder with a store still requires confirmation.

After confirmation, reserve the subtree and mark all its stores incomplete in a
single transaction before cleanup begins. Process stores sequentially and retain
per-folder failures for the dialog. Successful cleanup stays committed even if a
later store fails. Retain a store row with an unknown allocation outcome until
the final local-deletion transaction, even after its known resources are cleared;
that row preserves the warning context across another store's failure or restart.
It needs no further remote retry. When all known cleanup completes, commit those
warnings, remove the remaining store rows, and delete local notes plus conversation
associations in one transaction. An uncertainty-only result follows section 7.3:
show the warning/Close state, not the failed-delete Retry state.

After a known-resource cleanup failure, stop the worker but keep the subtree
reservation with its failed control. Present Retry, Cancel, and Delete anyway:

- **Retry** uses that reserved subtree and cleans only the remaining identifiers.
  It may use a replacement value for the same saved key ID. Never recreate stores
  already removed.
- **Cancel** closes the dialog, retaining local notes and any incomplete records.
  Stores already deleted stay deleted; remaining stores are not restored to ready.
- **Delete anyway** is accepted only for the failed folder-delete attempt and
  original context. Under its retained reservation, save a warning with each
  affected folder name/ID, service, key ID, remote store ID, owned file IDs, and any
  unknown outcome. Then discard those store records, delete
  the subtree, and delete its persistent session associations in the same
  transaction. Issue no further remote requests and schedule no later work.

Keep the normal operation identity to reject a delayed Cancel/Close/Delete anyway
from an old dialog. The retained reservation prevents overlapping structural or
store changes between attempts, so no second confirmation-state mechanism is
needed. Cancel, explicit dialog close, vault change, or successful completion
releases it. A reloaded UI can recover the control by folder ID and release or
retry it. No timeout/expiry policy is needed. Delete anyway is unavailable for
standalone Remove and is unnecessary for an uncertainty-only warning result.

Do not replace the existing local-only delete path for subtrees with no stores.
Retain existing compression cancellation, note-audio abandonment, annotations,
and active-editor behavior on the eventual local deletion. A failed warning-event
write rolls back Delete anyway; it is not a successful local deletion.

## 11. Bridge and UI contract

Use bounded start/status/cancel commands rather than one long bridge request.
Proposed native methods, all requiring the current context epoch:

| Method | Input / result |
| --- | --- |
| `notes.storeChoices` | Returns eligible service/key ID/name choices; no secrets. |
| `notes.storeUpload` | Folder ID, service, key ID; returns accepted operation ID. Validation completion/status can return an editable preflight error. |
| `notes.storeRemove` | Folder ID and expected store-record ID; returns operation ID. |
| `notes.storeOperation` | Folder ID; returns its current operation ID, kind, phase, counts, safe error, terminal result, and whether known cleanup remains. Supports reconnecting to a control. |
| `notes.storeCancel` | Operation ID; requests upload cancellation and cleanup. |
| `notes.storeClose` | Operation ID; acknowledges a terminal result or cancels a failed-delete dialog, releasing its reservation. Cannot discard active cleanup. |
| `notes.folderDelete` | Folder ID and confirmed store count to start cleanup/deletion. Retry uses the existing operation ID. `delete_anyway = true` requires that failed operation ID and performs only the specified local fallback. |

The existing `notes.delete` must reject a subtree with store records and direct
the caller to the confirmation flow. This prevents an older UI from bypassing
remote cleanup. Normal note/local-only folder deletion is unchanged.

Add an optional store summary to folder listing DTOs: service, local store-record
ID, and ready/incomplete state. Compute it from current rows; do not expose remote
IDs in normal menus. API-key usage and explicit diagnostic errors are separate
views. Poll operation status only while its dialog is active; refresh the listing
on completion. No application-wide progress panel or permanent polling loop.

Compute the folder-delete confirmation count from that existing full listing;
no `folderDeleteInfo` endpoint is needed. At initial submit the backend recomputes
the count and reserves the subtree. If the count changed, return an ordinary
validation error and refresh the listing before confirmation; do not delete.
Failed attempts keep their reservation and reuse the same dialog for Retry.

Controls are application-owned, as compression/audio work is today. A bridge
disconnect or request timeout does not cancel a store operation or release its
reservation. Do not add connection ownership. On menu/dialog reopen, query status
by folder ID and recover any active or failed operation before starting another.
Explicit Close releases a terminal/failed control; a new operation may replace
an acknowledged or non-reserving terminal status. Vault change discards controls
after interrupting them, leaving incomplete records for explicit cleanup.

Extend `NotesTree.tsx` without reordering existing actions. Show Copy Ref ID for
real folders and notes, never the synthetic root. Show Upload when no record
exists and Remove when one does; disable conflicting actions during active work.
Use a focused `FolderStoreDialog` component for the PRD's upload states and extend
the current delete dialog for cleanup failures. Keep title/legend text out of the
panel; the upload dialog gets the accessible name **Upload folder to store**.

Preserve focus restoration, keyboard handling, disabled duplicate submission,
existing error rendering, and context-epoch invalidation. Before starting Upload,
show only Provider, saved API key, Cancel, and Upload. Working states replace
fields with concise progress and Cancel. Removal has pending/error states. The
delete fallback warning explicitly states that local notes will be deleted and
remote data/charges may remain. Add no citation panel, badges, or helper copy.

The following compact state sketches supplement the PRD. They use the same
dialog layout; the labels above them are document captions, not panel titles.

```text
No eligible key                  Upload/indexing failure
┌────────────────────────────┐   ┌────────────────────────────────┐
│ Provider [OpenAI        ▾] │   │ Could not index “Experiment”.  │
│ API key  [             ▾] │   │ Use Remove from store to retry │
│ Add a compatible provider │   │ cleanup.                       │
│ and saved key in Settings.│   │                       [ Close ]│
│       [ Cancel ] [Upload] │   └────────────────────────────────┘
└────────────────────────────┘
Upload is disabled.

Indexing                         Cancel cleanup
┌────────────────────────────┐   ┌────────────────────────────────┐
│ Indexing notes…            │   │ Cleaning up…                   │
│                [ Cancel ] │   └────────────────────────────────┘
└────────────────────────────┘

Removal pending                  Removal failure
┌────────────────────────────┐   ┌────────────────────────────────┐
│ Removing store…            │   │ Could not remove “Research”:  │
└────────────────────────────┘   │ the saved API key is missing.  │
                                 │                       [ Close ]│
                                 └────────────────────────────────┘

Known cleanup finished with uncertainty
┌──────────────────────────────────────────────────────────────┐
│ Local removal completed. Some remote data could not be       │
│ confirmed deleted. See Events for manual-cleanup details.    │
│                                                   [ Close ] │
└──────────────────────────────────────────────────────────────┘
```

Omit the cleanup-retry sentence when no known resources remain. Validation errors
retain the editable fields; working states replace them. Failed cancellation
uses the error/Close layout. The PRD supplies the folder-delete failure sketch.
These are design artifacts, not implemented UI changes. The session instruction
for implementing a completed design applies to any later implementation request;
this review does not add a second UI-approval gate.

## 12. Events, errors, and validation

Use normal Events retention for successful upload/removal and failures requiring
later action. Record folder name/ID, service, note count where relevant, and safe
error category. Include remote IDs in manual-cleanup warnings. Do not log note
bodies, key values, raw provider response bodies that may echo content, or secret
upload URLs. Progress stays in the dialog. A missing key error names the saved
key reference and explains the available replacement-value or folder-deletion
recovery; it never binds another key automatically.

Write returned remote store/file/operation IDs to the existing diagnostic log at
info level with original vault/folder/service context, before the persistence
attempt; late IDs and abandoned/unconfirmed cleanup lists use warning level.
Log no secret upload URL or key value. This uses existing logging, not an orphan
registry, and helps recovery when a vault switch prevents a late database write.

Accept the existing retention/restore limits explicitly. Events retain only 1000
entries and logs have their existing retention; neither is a permanent recovery
record. R2 download/backup restore can revive records for deleted stores or omit
newer store records. Surface stale-store errors and permit explicit cleanup;
do not add remote discovery or reconciliation. A restored copy of the same vault
is not an independently owning child vault. New-vault copies use section 3.3's
ownership reset instead.

Implementation validation must cover these behaviors in the existing harnesses:

| Tests | Required cases |
| --- | --- |
| Storage/export | Additive schema on vault and Welcome creation/open; unique folder ownership; nested snapshot; saved editable notes; annotations/empty bodies excluded; duplicate titles; full Markdown vs compressed text; final export byte bound; invalid export rolls back; warning plus Delete anyway atomicity/rollback. |
| Operation worker with fake HTTP | Both providers' complete upload/index/delete paths; sequential transfers then polling rounds; partial/terminal errors; lagging Google counts; user cancel lets allocation finish then cleans up; interruption can abort; late IDs; lost responses; database failure; missing/replaced key; 404 warning committed with ID removal vs retained 401/403; uncertainty-only completion; remaining-ID retries. |
| Vault lifetime | Copied child has no remote ownership but keeps folder refs; source remains usable; password protection preserves ownership; interrupt/reopen never auto-resumes; switch does not wait for lifecycle-blocked workers; late IDs are logged and stale workers write nothing; shutdown retains dependencies. |
| Session/input | Current note refs and six-digit grammar unchanged; folder refs; stable ID after rename/re-upload; root rejected; duplicate attach; detach unavailable/repeated/invalid; restart; cover/uncover; Clear in Welcome and stored sessions; atomic persistent attach/delete; Welcome deletion pruning and clearing only folder refs on a path-changing retarget. |
| Matching/errors | Service/key/protocol/endpoint matrix; key deduplication; changed settings and replacement stores; provider count rejection with detach guidance; one multicast target failing while others finish. No invented numeric limit test. Add known-limit tests only if section 8.2's verification condition is met. |
| Adapter/client regression | Responses File Search with existing auto/required choices; budget exhaustion retains only File Search; Google streaming/non-streaming, mixed parts/signatures/IDs, `VALIDATED` and final-round behavior; truthful availability; auxiliary/maintenance exclusions before store lookup; no-store and Chat Completions requests unchanged. |
| Bridge/React | Choices/zero-key error; folder menu/root; preflight vs remote failure; progress/cancel/cleanup; duplicate requests; empty-folder store confirmation; count refresh through ordinary validation; retained failed-delete reservation; Retry/Cancel/Delete anyway; reload recovers control by folder ID; stale operation IDs rejected; key usage; focus/accessibility. |

Extend `unit_note_storage`, `unit_workspace_session_database`,
`unit_workspace_config_store`, `unit_text_input`, `unit_session_controller`,
`unit_responses_api`, `unit_google_api`, and `unit_provider_client`, plus focused
new store-client/storage/application tests. Use existing bridge fixtures and
NotesTree tests. Store HTTP tests inject responses and wait functions; they need
no real credentials or sleep delays. Avoid a second Google provider harness.

Run the affected C++ tests and the frontend API-type/typecheck/test checks, then
the normal suite appropriate to the integration. The design-only change does
not require application tests to be run now.

Before feature acceptance, perform one small live flow for each provider using
an explicit temporary test folder and existing eligible keys. Include a child
note with a distinctive fictional fact, attach the parent Ref ID, and retrieve
the fact in an answer. Edit the local note to verify snapshot behavior, remove
and re-upload to verify replacement through the same attachment, then detach.
For Google, also exercise File Search and a CHA web function in one answer with
continuation/signatures, and the function-budget final round. Remove all created
stores/files afterward and confirm cleanup. Do not infer inference support from
upload success. Record the actual tested model IDs and any provider limitation.

## 13. Implementation order and completion criteria

1. Add schema, full subtree export, session associations, and guarded deletion
   transactions with storage tests.
2. Add direct-service/key eligibility and the small store HTTP client. Validate
   resource ownership, index terminal states, and cleanup using fake transport.
3. Add explicit operation controls, cancellation, vault interruption, Events, and the
   folder-delete fallback. Test failures before connecting the UI.
4. Add Ref ID dispatch, detach, per-target resolution, and the request
   field. Extend both inference adapters and continuation tests together.
5. Add native DTOs/methods, key usage, folder menu/dialogs, and frontend checks.
6. Run the two live provider flows, including Google's mixed tools and cleanup.

The feature is complete when the applicable PRD acceptance cases pass, with the
conditional known-limit cases recorded as section 8.2 specifies. Provider facts
are linked at their use sites; undocumented behavior remains a validation risk,
not a guaranteed capability. Implementation can start from this design. Passing
storage tests or uploading successfully is not enough to accept the inference
integration.

Before acceptance, record results for these remaining live checks:

- Google mixed continuation, including `NONE` at the function budget. A rejection
  ends the affected request visibly; that model/path is not accepted as working
  until the required combination is tested. There is no silent fallback.
- Google upload-operation completion versus store-count visibility. Count lag
  remains Indexing; every operation and the final ready counts must succeed.
- OpenAI replay of hosted File Search output alongside function continuation with
  the application's existing `store: false`. Preserve the existing continuation
  contract; do not discard needed items or change persistence silently on error.
- Both providers' wrong-project-key deletion responses. Always preserve not-found
  IDs in warning events/logs, whether the tested service returns 403 or 404.
- Native Google `generateContent` support on the selected model despite the guide's
  Legacy label. The official page still documents this request and supported
  models; unsupported models fail visibly. Do not migrate protocols as a fallback.

These checks were not run during this document review. They require the feature's
implemented client and test resources; they introduce no new product choice or
extra approval gate.

## 14. Disposition of Claude's review

The findings in [claude-review.md](claude-review.md) were checked against the PRD,
current code, and official provider documentation. The review remains unchanged
as the record of the earlier design. All 16 findings are addressed below.

| Finding | Decision and correction |
| --- | --- |
| P1-1: copied-vault ownership | Accepted. Clear ownership only in new-vault creation. The generic copy helper must retain it because password protection also uses that helper. Keep child session-folder refs. Section 3.3. |
| P1-2: lifecycle lock during drain | Accepted. Use non-waiting cancellation/epoch guards, with late IDs logged and incomplete state retained. Do not refactor every maintenance lock or wait while holding it. Section 6.3. |
| P1-3: not-found with another project's key | Accepted, with stronger crash handling. Persist a warning with each missing ID when clearing it, not only at final completion. Missing-resource evidence survives a later interruption. Section 7.3. |
| P2-1: Welcome association lifecycle | Partly accepted. Skip all maintenance-answer resolution and remove per-request pruning. Keep deletion pruning and clear folder refs on vault-path change. Never resolving for inference alone would still let attach/detach confuse identical IDs across vaults. Section 3.2. |
| P2-2: conflicting validation connections | Fixed by defining each connection's role. Initial callback lookup is followed by a small recheck on the journal connection inside its insert transaction. Retain explicit deletion of associations as required by the PRD; accepting dangling rows is unnecessary. Sections 3.2 and 8.1. |
| P2-3: Welcome schema creation | Accepted. Name both schema entry points explicitly and test Welcome Clear. Section 3.1. |
| P2-4: unverified limits | Accepted for request/count machinery: the cited models are retired; remove speculative checks and the preflight-error field. Preserve the PRD's conditional verified-limit requirement and provider-error recovery. Retain the trivial final-byte check because a 12 MiB JSON input is not a proven bound on expanded Markdown/path metadata. Sections 5 and 8.2. |
| P2-5: forced hosted-search choice | Accepted as a scope reduction, without claiming the API form is proved invalid. Preserve existing tool choice instead of introducing strict hosted-search behavior. Section 9.1. |
| P2-6: uncertainty-only Retry loop | Accepted. All cleanup callers finalize when known resources are cleared, with an atomic warning for uncertainty. Only actual remaining cleanup failures need Retry/Delete anyway. Sections 7.3 and 10. |
| P2-7: Google operation persistence/index waits | Accepted. Keep/log operation names in the active worker, remove the unused column, and poll unfinished documents together after sequential transfers. Sections 3.1, 6.1, and 7.2. |
| P3-1: cancellation during allocation | Accepted. User cancel stops the next step, allows an already-sent allocation to return within its normal timeout, then cleans up. Lifecycle interruption may abort it. Section 6.2. |
| P3-2: hosted web search after budget | Accepted. Responses retains only File Search at the function budget. Section 9.3. |
| P3-3: bridge and delete-state size | Partly accepted. Remove delete-info and separate retry/anyway endpoints, reuse listing counts, retain the failed subtree reservation, recover status by folder ID, and avoid connection-loss cancellation. Keep operation IDs for mutating actions so delayed commands cannot affect a replacement. Sections 10–11. |
| P3-4: Ref ID width change | Accepted. Keep the six-digit parser; the pre-existing large-ID mismatch remains outside this feature. Section 8.1. |
| P3-5: extra UI approval | The proposed extra gate is not adopted: this is a document edit, and the user's completed-design instruction governs a later implementation request. Add missing state sketches and remove the separate refreshed-confirmation state. Section 11. |
| P3-6: retention and restore limits | Accepted as limits, with existing diagnostic logging as a recovery aid. No permanent orphan registry or automatic reconciliation. Section 12. |
