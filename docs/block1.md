# Block 1 — Store backend and lifecycle

Status: Implementation instructions; not yet implemented.

This is the first of three implementation blocks, sized for one 512K-token
session including implementation, tests, fixes, and review. It covers the complete
backend for folder stores. This file contains the requirements for implementing
and reviewing this block without the previous chat or another block guide.
The source documents are [store-design.md](store-design.md),
[store-prd.md](store-prd.md), and [store-plan.md](store-plan.md).

## 1. Deliverable and boundaries

A real folder in the active vault can own one provider-hosted snapshot on OpenAI
or Google. Upload captures all saved nonempty ordinary notes in its subtree,
uploads one Markdown document per note, and becomes ready only after every
document finishes indexing. Remove deletes the owned remote resources while
preserving notes. Folder deletion cleans up stores in the entire subtree before
deleting local data, with an explicit failure fallback.

Deliver application operations, persistence, both management clients, cancellation,
cleanup, lifecycle handling, and deterministic tests. Also deliver the journal
association primitives needed by later chat integration. Block 2 will connect
these operations to chat, inference, DTOs, and the native bridge. Block 3 will add
the UI and run full live acceptance. Do not expose menus or implement File Search
in model requests in this block.

The existing application and tests are the starting point; there is no prior
store block to depend on. Inspect the current diff first and preserve unrelated
changes. The user considers the completed design approved. Keep any necessary
implementation choices small and report material deviations in the handoff.

Repository constraints apply throughout:

- CHA is a personal application. Reuse its SQLite, libcurl, workers, credentials,
  runtime, Events, and logging. Prefer plain code over a new framework.
- Do not add synchronization, embeddings, a local vector database, copied corpus,
  durable job queue, account discovery, orphan registry, startup recovery job,
  expiry, automatic resubmission, cleanup daemon, or additional provider setting.
- Do not block work because of irrelevant or obsolete configuration. Ignore it
  with a warning. Validate the fields actually needed by the operation.
- Change no unrelated files or documentation. Report results in the final handoff;
  update plan status only if the execution request explicitly authorizes it.
- All tests in this block use fake HTTP and disposable local data. Live provider
  resources are not needed for block acceptance.

## 2. Existing code and ownership

Paths below are repository-relative and identify the implementation entry points.

| Area | Files and existing behavior |
| --- | --- |
| New store persistence | Add `src/storage/folder_store_storage.{h,cpp}` for schema, ownership/resource queries, and transaction primitives. |
| Notes | `src/storage/note_storage.{h,cpp}` owns `note_items`, `note_document_markdown`, recursive queries, annotation updates, and audio cleanup in `note_delete`. |
| Vault database | `src/workspace/workspace_config_store.{h,cpp}` owns a mutex-protected database connection. `Impl::open_database` installs additive schema in a transaction. |
| Database creation | `src/storage/workspace_session_database.{h,cpp}` provides `create_workspace_session_schema`, generic copying, and password protection. |
| Journals | `src/storage/session_database.{h,cpp}` owns transactional attachment notices and `clear_history`. Persistent journals use separate connections to the vault; Welcome has a private database. |
| Repository/runtime | `src/storage/session_repository.{h,cpp}` handles maintenance retarget. `src/runtime/live_session{,_manager}.{h,cpp}` contains the existing runtime command path. Only the runtime owns live controllers and journal mutation. |
| New remote client | Add `src/providers/folder_store_client.{h,cpp}`. Reuse `src/util/curl.{h,cpp}` and `src/providers/api_key_store.{h,cpp}`. |
| Configuration values | `src/characters/character_config.h` defines `ModelBackendConfig`, including host, port, base path, HTTPS, API, auth, and saved-key references. |
| New operations | Add `src/app/folder_store_operations.cpp`; extend `application.{h,cpp}` and `application_internal.h`. |
| Worker examples | `src/app/notes_operations.cpp` has compression controls. `note_audio_operations.cpp` has epoch/interruption guards. Reuse those patterns, not audio's automatic recovery behavior. |
| Maintenance/shutdown | `src/app/vault_operations.cpp`, `application.cpp`, and `background_jobs.{h,cpp}` own admission, maintenance, shutdown deadlines, and dependency retention. |
| Tests/build | `CMakeLists.txt`, `tests/storage`, `tests/workspace`, `tests/app`, `tests/providers`, and existing test support. |

Keep store clients independent of sessions, databases, and presentation. The
application saves resource IDs between remote calls. Do not expand the inference
`ProviderClient` transport into a store-management framework.

## 3. Persistent model and transactions

### Step 1: Install the additive schema

Use an idempotent helper with the following schema. Call it after note schema
creation both in `create_workspace_session_schema` and in
`WorkspaceConfigStore::Impl::open_database`'s existing schema transaction. The
first path includes `create_session_database`, Welcome, and test journals. Keep
the current schema-version convention; existing vaults begin with empty tables.

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

Validate `kind = 'folder'` when registering a store. A note, annotation, missing
item, or synthetic root cannot own one. The root has no real folder row.
Every replacement gets a new local store-record ID. Compare that ID on every
update so a late result cannot update a replacement.

`folder_store_files` records only OpenAI Files resources created by this attempt,
including files that never attached successfully. It is not a synchronization
index. Keep Google operation names with their documents in worker memory; deleting
the known Google store covers those documents. Persist no note bodies, key values,
progress counters, upload URLs, or Google operation column.

### Step 2: Define short, explicit transaction operations

Expose the needed operations through `WorkspaceConfigStore` under its existing
mutex. Low-level queries used inside a caller's transaction must not open another
transaction. Provide operations for registration, uncertainty, saving IDs, marking
ready/incomplete, recording individual cleanup results, and final local removal.

- Commit `remote_outcome_unknown = 1` before a request that can allocate a store
  or an independently owned OpenAI file. Clear it in the transaction that saves
  the returned ID, or after a definite rejection known to have created nothing.
  Stop after the first ambiguous allocation. Never replay it automatically.
- A Google upload to a known store does not need a second persistent ownership
  record: force-deleting that store covers a lost operation response.
- Commit readiness, uploaded time, and upload-success event together.
- Commit final store-record removal and its outcome event together.
- Commit each accepted not-found warning with removal of that resource ID.
- Commit Delete anyway warnings, store removal, notes, and session associations
  in one transaction. A warning-write failure must roll back local deletion.
- Never hold a database transaction or `lifecycle_mutex` over network I/O.

Split `note_delete` into a transaction-owning wrapper and a transaction-neutral
helper. Preserve its annotation rebuild, audio-attempt warning, audio cleanup,
and error behavior. The restrictive foreign key is the final safeguard against
an old deletion path abandoning a store; normal deletion must use the cleanup
flow first.

### Step 3: Add journal primitives and Welcome handling

Add methods to read attached folder IDs and add, detach, clear, and prune folder
associations. They store no remote IDs or corpus. Add/detach and their notice must
commit together; keep prior notices. A duplicate attachment is idempotent even
if its store later became unavailable. A new attachment requires a ready store.
Detach works without a ready store or key and uses the Ref ID if the name is
unavailable. Repeated detach reports that the folder is not attached.

For persistent sessions, recheck existence/readiness on the journal's own
connection in the `BEGIN IMMEDIATE` transaction that inserts the association.
Do not call `WorkspaceConfigStore` while holding this transaction. Initial item
metadata lookup in block 2 is not a substitute for the recheck.

Welcome validates against the active vault through the callback supplied by its
caller; it cannot query source folders in its own database. This is why
`session_folders.folder_id` has no note foreign key. After a committed local
subtree deletion, enqueue Welcome pruning on the existing runtime, with the
originating epoch. Attach and prune run on that same runtime. Workers must not
mutate a live Welcome journal, and stale prune commands must not affect another
vault. Do not add a per-answer pruning pass.

Clear removes folder associations with note/file attachments. Cover/uncover keeps
them, and session deletion cascades normally. Do not extend `notes_prompt()`.
Read association IDs when needed; add no controller-side attachment cache.

When `MaintenanceGuard::retarget` changes the database path, clear only Welcome's
folder associations while controllers are drained. Preserve its transcript and
note/file copies. A same-path password change retains its folder associations.

## 4. Snapshot capture and credential matching

### Step 4: Capture full saved documents

Run preparation off the UI thread after reserving the folder. Use one SQLite
read transaction to obtain the owning folder, ancestor path, descendant parent
links, names, IDs, and saved documents. Use the existing recursive-CTE pattern.
Do not list the tree and read individual notes later from different snapshots.

Include only `kind = 'note'`, whether editable or read-only. Export the full
document through `note_document_markdown`, never compressed text. The existing
chat-note callback uses `prefer_compressed = true` and cannot serve this purpose.
Do not request an editor save or accept unsaved editor content.

Check exportable text before adding metadata. Whitespace and empty formatting
such as empty headings/code blocks do not make a document nonempty. Image alt
text and comments follow the existing exporter. Skip empty notes; if all are
empty, fail before any remote creation. A damaged nonempty document fails the
entire preflight and identifies the note. Never silently truncate or omit one.

Each document is UTF-8 `text/markdown`:

```markdown
# Note: Experiment log

Folder: Research/Experiments
CHA Ref ID: ##00000B

<full saved Markdown body>
```

The folder path is relative to the notes root at capture time. Escape metadata
without changing the body. Use a bounded readable filename with separators and
control characters sanitized and the complete numeric note ID, for example
`Experiment-log-11.md`. Duplicate titles must remain distinct. Upload no binaries,
editor JSON, imported original files, or fetched link targets.

Match `webapp/src/notes/reference.ts` when formatting Ref IDs: encode the positive
item ID with `0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz`, pad
on the left with zeroes to at least six digits, and prefix `##`. Do not truncate
large IDs. The existing chat parser accepts exactly six digits; fixing that
pre-existing large-ID mismatch is outside this feature.

Own the exported vector in worker memory until completion; release database locks
before remote calls. No temporary on-disk corpus is needed. Allocation failure
is an explicit preflight error. Check final document bytes including metadata;
the 12 MiB saved-JSON limit does not bound exported Markdown and folder paths.
The design specifies OpenAI's 512 MB per file and Google's 100 MB per document;
verify current units/bounds in the official references before encoding them.
OpenAI token limits, indexing quotas, and unverified file-count limits remain
provider errors identifying the current note. Add no tokenizer or quota system.

After capture, note edits/deletions do not change the upload. Protect the owning
folder from deletion, including during preflight before a store row exists.
Parent/child stores are independent; source-note saves need not be frozen during
network work. Folder identity is its item ID, not its name or path.

### Step 5: Share exact service/key eligibility

Return an optional service plus resolved saved-key ID, or an omission reason.
Inspect actual connection fields:

| Service | Required configuration |
| --- | --- |
| OpenAI | Network mode, API-key authentication, Responses API, HTTPS, exact host `api.openai.com`, port 443, empty `base_path`. |
| Google | Network mode, API-key authentication, native `google` API, HTTPS, exact host `generativelanguage.googleapis.com`, port 443, `base_path = /v1beta`. |

In the current code API-key configuration uses `ProviderAuth::none` plus a saved
key; do not invent an enum member called `api_key`. Exclude subscription auth,
test mode, Chat Completions, compatibility endpoints, suffix lookalike hosts,
proxies, Azure/Vertex, and alternate ports. Normalize host case consistently.

Build upload choices from all compatible configured models. Resolve a supported
legacy key-name reference through `ApiKeyStore`; it is not an environment lookup.
Require a saved nonempty value and deduplicate by stable key ID within a service.
The same ID may appear for both services when referenced by both. Different saved
IDs never match merely because their values are equal.

Revalidate on submission. Persist the saved key ID, never a model-configuration
ID or secret. Before each remote operation, resolve that ID's current value under
the vault guard. Removal must still work after the model configuration is deleted.
Missing/revoked/replaced keys produce explicit errors; never choose another ID.
Key rename, replacement, and deletion remain allowed. Usage display is block 2.

## 5. Remote management clients

### Step 6: Implement narrow libcurl calls

Use `CurlHandle`, `CurlHeaders`, TLS verification, cancellation callbacks, and
existing timeout/error conventions. Management origins are fixed by service;
never derive them from a user-configured model endpoint. Accept immutable inputs
and a small injectable HTTP transport. Leave persistence and polling rounds to
the application. Inject waits/barriers only where needed for deterministic tests.

| OpenAI step | Request/result on `https://api.openai.com/v1` |
| --- | --- |
| Authenticate | `Authorization: Bearer <current saved key>`. |
| Create | `POST /vector_stores` with `name`; return `id`. Omit expiry and custom chunking. |
| Upload | Multipart `POST /files`, `purpose=assistants`, one Markdown file. Return its ID before attachment. |
| Attach | `POST /vector_stores/{store_id}/files` with `file_id`. |
| Inspect | `GET /vector_stores/{store_id}/files/{file_id}`. Require `completed`; `failed`/`cancelled` fail the upload. |
| Remove | Delete `/vector_stores/{store_id}`, then every recorded `/files/{file_id}`. Attempt all known deletions even when one fails. |

An HTTP upload success is not indexing completion. Inspect individual file status;
no batch-state model or paginated discovery is required. Files are owned by one
upload only, including those that failed attachment. Deleting membership alone
does not satisfy cleanup.

| Google step | Request/result on `https://generativelanguage.googleapis.com` |
| --- | --- |
| Authenticate | `x-goog-api-key` with the current saved value. |
| Create | `POST /v1beta/fileSearchStores`; retain the returned store resource name. |
| Upload | Direct media flow at `POST /upload/v1beta/{fileSearchStoreName}:uploadToFileSearchStore`, with Markdown MIME type and readable display name. Use provider-default embedding/chunking. |
| Inspect operation | Poll the returned operation beneath `/v1beta/`; upload operation names have the form `fileSearchStores/{store}/upload/operations/{operation}`. Require `done` without `error`. |
| Inspect store | Read the known store beneath `/v1beta/`; parse decimal-string active/pending/failed document counts. |
| Remove | `DELETE /v1beta/{store_name}?force=true`. |

Use the documented Google media-upload protocol and validate its current request
headers/fields against the official API before implementation. Validate returned
operation names against the expected store, and upload-session URLs against the
expected service origin before sending credentials. Keep secret upload URLs only
in memory, never in logs or DTOs. Do not add the separate Files/import route or
an Interactions inference adapter.

After all Google document operations succeed, require active count equal to the
captured note count, pending zero, and failed zero. Short active count or pending
documents means keep Indexing and poll again. A failed document or excess active
count fails the attempt. Counts can lag operation completion.

Official references for implementing the specified calls:

- [OpenAI File Search setup](https://developers.openai.com/api/docs/guides/tools-file-search#how-to-use)
- [OpenAI retrieval limits](https://developers.openai.com/api/docs/guides/retrieval#limits)
- [OpenAI Files deletion](https://developers.openai.com/api/reference/resources/files/methods/delete)
- [Google store and upload API](https://ai.google.dev/api/file-search/file-search-stores)
- [Google File Search limits](https://ai.google.dev/gemini-api/docs/generate-content/file-search#rate-limits)

## 6. Operation coordination and cleanup

### Step 7: Establish admission, controls, and interruption first

Use `BackgroundJobs` and a small application-owned control map guarded by
`lifecycle_mutex`. Each control carries its epoch, operation identity, folder/root
ID, current store-record ID if any, kind/phase, counts, safe outcome/error, and
cancellation state. Durable store state remains only `incomplete` or `ready`.
Keep progress phases such as preparing/uploading/indexing/cleaning/removing in
memory. No new queue, expiry policy, or connection-owned lifecycle is needed.

Reject duplicate store operations and overlapping subtree-delete/store work.
Recheck ancestry at admission. Guard legacy local deletion against both store
records and active operations, including an ancestor delete during preparation.
A reserved delete subtree also blocks create/import or other structural changes
that alter its membership. Check existing mutation paths; do not add a move
feature. Ordinary note saves remain possible.

Before each new network step and vault write, check admission, epoch, control
identity, and store-record identity under the lifecycle lock. Release locks for
network I/O. Mutations require existing main-process admission. A stale command
cannot cancel, retry, close, or finalize newer work.

During `drain_for_maintenance`, interrupt controls and retire reservations without
waiting for their workers. The caller already owns the lifecycle lock: do not
join workers waiting for it or unlock a caller's `lock_guard`. Preserve current
switch/merge/R2/vault-deletion lock structure. On shutdown use existing
`BackgroundJobs` deadlines and dependency retention, not another drain policy.

After interruption, an old worker must not write to the new vault, reopen the
old database by path, or start cleanup. Log late returned IDs with original
vault/folder/service/key-ID context. Precommitted uncertainty or the known Google
store remains the restart evidence. Reopening never resumes or contacts providers.

### Step 8: Implement shared cleanup and standalone Remove

Reserve the folder and mark its store incomplete before Remove makes any remote
call. Use the stored service/key. Remove preserves notes and session associations;
a future upload becomes available to existing folder attachments.

| Remote/local result | Required action |
| --- | --- |
| Confirmed resource deletion | Commit removal of that resource ID, then continue. |
| Authenticated, structured not-found for the expected resource | Commit a warning containing that ID, folder, service, and key ID with removal of the ID. Also log it. |
| Generic/malformed 404, 401/403, timeout, network failure | Keep the known ID and incomplete record. Continue other known deletions where applicable; report remaining failures. |
| Local bookkeeping failure | Do not claim cleanup is durably recorded; retain recovery information and report the error. |
| All known IDs cleared, allocation outcome unknown | Finalize local removal with an atomic uncertainty warning. No retry loop for an ID that was never learned. |

Structured not-found completes cleanup of that known resource, but does not prove
the original project deleted it: a replaced key for another project may produce
the same response. Explain possible project mismatch and manual cleanup. Retry
must not erase earlier warnings or claim full remote deletion when prior results
cannot be established. Use neutral local-association/known-resource wording in
that case. Add no durable warning-history store.

Once every known ID is cleared, delete the store record with its outcome event.
Unknown/not-found outcomes produce a warning terminal state, not a false success.
Known failures retain the incomplete record and remaining IDs for explicit Remove.
Use this same routine for failed/cancelled uploads and folder deletion.

### Step 9: Add upload and user cancellation

Execute this sequence:

1. Reserve the folder and start asynchronous preparation. Validate credentials
   and capture the saved subtree. Preflight failure releases admission and leaves
   an editable error; no remote effect has occurred.
2. Insert an incomplete attempt with service/key/note count before creation.
   Registration or launch failure must not allocate anything remotely.
3. Commit uncertainty, create the store with a readable folder name and Ref ID,
   log its returned ID, then persist it and clear uncertainty atomically.
4. Transfer notes sequentially. For OpenAI, persist each file ID before attach.
   Keep file/operation results with each exported document. Check immediate
   terminal errors, but do not wait for one note's indexing before the next upload.
5. Poll all unfinished entries in rounds, with one interruptible wait per round.
   Honor a supplied poll interval, otherwise use a short fixed interval. Perform
   Google's final count check after document operations complete.
6. Under the operation/lifetime guard, verify all documents succeeded, the
   original attempt is current, and user cancellation is false. Commit readiness
   and its event before reporting success.
7. Any failure/cancellation uses the shared cleanup path. Never publish partial
   readiness or discard failed known IDs.

User cancellation and lifecycle interruption are different:

| Situation | Forward requests | Bookkeeping/cleanup |
| --- | --- | --- |
| User cancels preparation/upload/indexing | Stop further steps; cancel a non-allocating transfer and wake polling. Let an already-sent allocation or Google upload/finalize finish within its normal timeout. | Still parse/log/save returned IDs, then clean up. Upload cancellation must not immediately cancel cleanup. |
| Ready committed before Cancel | Upload is already complete; cancel is a no-op. | Normal later Remove is a separate operation. |
| Vault interruption/shutdown | May abort allocations as well as other transfers. | Stop writes/cleanup when lifetime checks fail; log late IDs with original context. |

Do not reset the upload-cancel flag to run cleanup. Serialize cancel and ready
commit through the same operation guard. A cancelled upload can never later
publish readiness. Confirmed cancellation cleanup is a cancelled terminal result;
uncertainty/not-found remains a visible warning. Normal upload failure stays an
error even if cleanup succeeds.

Log returned IDs before the database write. If that write fails, retain the ID
in the active control, stop forward work, and try cleanup only while its lifetime
still passes. If persistence and cleanup both fail, expose unresolved IDs in a
safe diagnostic error and attempt a warning event. Do not promise recovery of an
ID lost by both storage and logging; uncertainty must report that limitation.

### Step 10: Complete subtree deletion

The ordinary local delete path remains for subtrees without stores. For a subtree
with stores, count the selected folder and descendant stores, including incomplete
rows with no remote ID and stores on otherwise empty folders. Do not touch an
ancestor store outside the subtree.

1. On confirmed submission, recompute the count and reserve the subtree. A count
   mismatch is an ordinary validation error and must not start deletion.
2. Mark every affected store incomplete in one transaction before cleanup.
   Process stores sequentially; keep successful cleanup committed and report
   failures by folder. Do not undo successful remote deletion after another fails.
3. Keep any row with unknown allocation outcome until the final local-delete
   transaction, even after its known IDs are cleared. This retains warning
   context across another store's failure or restart.
4. When all known cleanup completes, atomically write remaining uncertainty
   warnings, remove remaining store rows, delete the subtree and persistent
   session-folder associations. Perform existing compression/audio retirement
   and enqueue Welcome pruning after commit.
5. After a known-resource failure, stop the worker but retain the failed control
   and subtree reservation. Retry uses that operation ID and only remaining IDs,
   resolving a replacement value for the same saved key if available.
6. Cancel/Close releases the reservation, retains notes and incomplete records,
   and leaves already deleted stores deleted. It does not restore readiness.
7. Delete anyway requires the failed operation ID and original epoch. With the
   reservation still held, atomically record every unconfirmed folder/service/
   key/store/file ID and unknown outcome, remove ownership, notes, and persistent
   folder refs. Make no further remote request and schedule no future cleanup.

Delete anyway is never a standalone Remove fallback. Uncertainty with no known
failed resources finalizes with warning/Close; it is not a Retry/Delete anyway
failure. Preserve local-only deletion behavior and existing compression,
annotation, audio, and editor-facing outcomes.

Controls survive bridge disconnect and request timeout. Status lookup by folder
ID can recover active/failed operations. Close acknowledges terminal state or
cancels a failed delete; it cannot discard active cleanup. Explicit closure,
successful completion, or vault interruption releases reservations as appropriate.
Do not add an operation expiry or a separate confirmation-token mechanism.

## 7. Vault copying and diagnostic evidence

In `VaultMaintenance::create_vault`, after copying a source database, clear the
destination's owned-file/store rows in one transaction before publishing it.
Keep copied session-folder refs: they remain unavailable until that child uploads
its own stores. Handle an older source without the tables. Make no remote calls.
If reset fails, remove the new unpublished destination and definition using
existing safe cleanup conventions; keep the source intact.

Do not reset ownership in `copy_workspace_session_database`: password protection
uses that helper for the same vault. Password protection and backups/restores
retain ownership. Configuration export/import is not a transfer of remote stores;
merge preserves local ownership and saved key IDs. A changed key value may cause
access errors or not-found, with the warning behavior described above.

Use Events for success and failures needing later action, including folder
name/ID, service, count where relevant, and safe error category. Manual-cleanup
warnings include remote IDs. Log returned store/file/operation IDs at info level
before persistence; late/abandoned IDs at warning level. Never log note bodies,
key values, raw error bodies that may echo content, or secret upload URLs.
Existing Events/log retention is finite; do not introduce permanent recovery
storage. Restore can revive stale records or omit newer ones; expose stale-store
errors and explicit cleanup, without discovery or reconciliation.

## 8. Contract delivered to block 2

Expose the following capabilities through application/domain methods; follow
existing C++ naming and value conventions rather than prescribing a new framework.
The semantic contract is required even if exact type names differ:

| Capability | Input/output requirements |
| --- | --- |
| Choices | Eligible service plus saved key ID/name; no secrets. Shared eligibility helper is callable during inference resolution. |
| Upload | Folder ID, service, key ID, epoch; promptly accept an operation ID. Preparation failure is distinguishable from a remote failure. |
| Remove | Folder ID, expected store-record ID, epoch; return an operation ID. |
| Status | Folder ID and epoch; return no current control or its identity, kind, phase, counts, safe error/result, and whether known cleanup remains. Terminal folder deletion must be distinguishable from operation acceptance. |
| Cancel upload | Exact operation identity and epoch; apply the cancellation rules above. |
| Close | Exact operation identity and epoch; acknowledge a terminal result or release a failed-delete reservation, never active cleanup. |
| Folder delete | Folder ID and confirmed store count for initial admission; operation ID for Retry or explicit Delete anyway; epoch on all calls. |
| Queries | Real item metadata, current store summaries/records, subtree ownership, and current store usage by saved key ID. |
| Journal | Folder association read/add/detach/clear/prune with atomic notice and persistent attach recheck. |

Block 2 will serialize these into seven native methods. Do not require its bridge
or block 3's UI to test backend invariants. Hand off the actual method/type names,
status/result representation, and test hooks so later agents reuse this code.
Status and terminal acknowledgement must still work with the captured folder ID
after a successful local deletion removed its item row; read the retained control
instead of requiring that the deleted folder still exists.

## 9. Required validation

Add focused tests in `unit_folder_store_storage.cpp`,
`unit_folder_store_client.cpp`, and `unit_folder_store_operations.cpp` in their
existing storage/providers/app test directories. Extend existing note, workspace,
session repository, vault maintenance, and runtime tests. Register each in the
appropriate existing CMake test target. No new test framework or Google harness.

| Area | Cases that must be demonstrated |
| --- | --- |
| Schema | Fresh and existing vaults, Welcome/test creation, Clear with no attachments, uniqueness, root/nonfolder rejection, replacement attempt IDs, ready constraints, restrictive delete. |
| Export | Nested editable/read-only notes, full rather than compressed text, annotations/empty formatting excluded, duplicate titles, path snapshot, UTF-8/filename metadata, size boundaries, damaged document/all-empty/allocation failure before remote calls. |
| Journal | Notice insertion rollback, idempotent attach while unavailable, detach/repeated detach, session isolation/cascade, Clear/cover lifetime, separate-connection attach/delete race. |
| Credentials | Exact host/port/protocol/auth matrix, legacy key names, deduplication, equal values with distinct IDs, missing/empty/replaced keys, deleted model configuration, irrelevant settings ignored. |
| Remote calls | Both complete HTTP sequences, ID-before-next-step ordering, individual index failures, sequential transfers then polling rounds, lagging Google counts, operation/session URL validation, malformed responses and safe errors. |
| Failure/cancel | Launch failure, crash/lost allocation response, database failure after remote success, cancel during preparation/allocation/indexing/ready commit, separate cleanup cancellation, no automatic retry, late IDs. |
| Cleanup | All OpenAI files including unattached ones, remaining-ID retry, structured not-found warning atomic with ID removal, generic 404/401/403 retention, unknown-only warning completion, neutral outcome after earlier warnings. |
| Deletion | Parent/child independent stores, ancestor preservation, empty folder with store, changed confirmation count, duplicate/overlapping operations, structural mutation guard, failed reservation recovery, Retry/Cancel/Delete anyway and warning rollback. |
| Lifecycle | Drain with worker waiting for lifecycle lock, switch during upload/removal, no old-worker writes, shutdown dependency retention, explicit cleanup after reopen without automatic calls. |
| Vault/Welcome | Child copy clears only ownership and leaves source/ref rows intact; older source; failed copy removed; same-vault password protection retains ownership; Welcome pruning ordered with attach and clearing only refs on path change. |
| Diagnostics | Known IDs retained or logged on failures, no content/secrets/secret URLs in status or logs, configuration-merge key mismatch, stale restored records do not trigger discovery. |

Use barriers, injected transport, and injected waits for races and polling; no
real sleeps or credentials. Use existing fault injection, or add only the narrow
hooks necessary to prove rollback and lifecycle behavior.

Run focused tests during work. At completion, from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_tests cha_app_tests
build/ninja/cha_tests
build/ninja/cha_app_tests
git diff --check
```

Inspect untracked new files as well as the tracked diff. A filter that matched
zero tests is not evidence. Record actual commands/results and any environment
failure separately. No frontend or live-provider pass is required for this block.

## 10. Independent review and completion

The reviewer should inspect this block's diff and surrounding callers, not just
the implementation summary. Establish the block's actual starting revision/diff;
do not attribute pre-existing or unrelated changes to it. Verify tests exercise
observable behavior and transaction/race failures rather than mirror helper code.

Prioritize these checks:

1. Every store or independently owned file allocation has durable uncertainty
   first, and returned ownership IDs are recorded before the next step. Google
   document operations remain covered by the known containing store. Partial
   work remains removable.
2. No cancelled/stale worker can commit readiness or write to another vault.
   Maintenance never joins a worker under the lock it needs.
3. Cleanup is shared, attempts all known resources, persists partial progress,
   and distinguishes not-found/unknown outcomes from proven remote deletion.
4. All subtree delete entry points enforce ownership/reservations. Delete anyway
   warnings and local deletion are atomic and cannot run for standalone Remove.
5. Persistent attach validates on its own connection; Welcome mutations stay on
   the runtime and respect epoch/path identity.
6. Child-vault copying, password protection, and restore have distinct correct
   ownership behavior. Unpublished failed copies are not left usable.
7. The schema and control map contain only required state. No resume policy,
   extra registry, background reconciliation, duplicate corpus, or speculative
   provider limits have been introduced.
8. Block 2 can use the documented application/query/journal contract without
   duplicating storage, HTTP, or cleanup logic.

Report actionable findings with severity, file/line, a concrete failure scenario,
and required behavior. Distinguish defects from unverified provider behavior.
Do not demand the later blocks' UI or inference as a condition of this block.

The block is complete when both management backends and local lifecycle flows
work through application methods, the required deterministic checks pass, and
the diff has been reviewed. The handoff must name the APIs/types delivered, tests
run, any remaining limits, and the starting point for block 2. Full product/live
acceptance remains block 3.
