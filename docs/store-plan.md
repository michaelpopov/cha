# Folder stores: implementation plan

Date: October 27, 2026

Status: Ready for implementation; no feature code has been changed.

Design: [store-design.md](store-design.md). Requirements and approved UI sketches:
[store-prd.md](store-prd.md). The requested `store-desing.md` is named
`store-design.md` in this repository.

This plan turns the design into ordered changes and verification steps. The
technical design remains the reference for schema, provider payloads, transaction
boundaries, and dialog states. Its section 14 contains the disposition of the
earlier Claude review; this plan uses those revised decisions.

## 1. Review outcome

The design fits the current application. It extends the existing vault database,
session journal, background workers, and provider adapters. A separate retrieval
service, provider SDK, or durable job system is not needed.

The following points were checked against the current code:

| Current code | Consequence for implementation |
| --- | --- |
| `WorkspaceConfigStore::Impl::open_database` adds schema inside a transaction; `create_workspace_session_schema` also creates Welcome/test databases. | Add the store schema at both entry points. Do not assume opening the active vault initializes Welcome. |
| `open_session` reads note attachments with `prefer_compressed = true`. | Keep that path for note refs. Capture store documents through a separate full-text subtree query. |
| `SessionJournal::add_note` commits the attachment and notice together; `notes_prompt()` reads saved copies. | Follow the transaction pattern for folder IDs, but keep folder content out of `notes_prompt()`. |
| `note_delete` owns its transaction and performs audio-attempt and annotation cleanup. | Extract a transaction-neutral deletion helper before adding atomic store warnings and subtree deletion. Preserve the existing cleanup. |
| `VaultMaintenance` calls drain while the caller owns `lifecycle_mutex`. | Interrupt store workers without joining them in drain. Workers must check their epoch and identity before later actions. |
| `create_vault` copies source databases; password protection also uses the generic database-copy helper. | Reset remote ownership only in new-vault creation. Do not change generic copy behavior. Extend creation failure cleanup to remove an unpublished destination database. |
| `start_generation` resolves each character and explicitly identifies the Welcome maintenance Assistant. | Resolve stores here for each ordinary answer target, after the maintenance exclusion. Do not infer eligibility from `include_tool_instructions`. |
| Google decoding and continuation already retain model parts and function IDs. | Extend the existing path. Fix tool-list assumptions and budget handling instead of adding another continuation loop. |
| `NotesTree` skips confirmation for empty folders and rejects folders in `copyReference`. `App.tsx` coordinates deletion with the active editor. | Include store count in confirmation, enable folder refs, and adapt the existing editor deletion flow to asynchronous completion. |
| API-key `used_by` is derived from workspace configuration. | Supply current folder-store usage from vault rows, including incomplete records. Do not add a key-deletion veto. |

There is no additional design-approval step for implementing the documented UI.
The completed-design instruction applies. Provider-dependent behavior still
requires the live validation in section 5; this review did not run those checks.

## 2. Scope and implementation rules

- A real folder owns at most one fixed snapshot. Upload reads all saved,
  nonempty ordinary notes below it. It does not save the editor or use compressed
  text. Parent and child stores remain independent.
- Only `ready` records provide access. Folder IDs remain stable across rename,
  removal, and replacement. A session stores folder IDs, not remote IDs or text.
- Match a request by direct service, protocol, and stable saved-key ID. Keep
  configured models and protocols unchanged. Resolve the key's current value for
  each management call; never substitute another saved key.
- Persist ownership before the next remote step. Use only the design's two
  durable states and its uncertainty bit. Progress, operation IDs, cancellation,
  and Google operation names belong to the active worker/control.
- Use one cleanup implementation for upload failure, cancellation, Remove, and
  folder deletion. Keep failed known IDs available for retry. Commit warnings
  with the local changes that discard their evidence.
- Reuse `BackgroundJobs`, `CurlHandle`, `CurlHeaders`, existing runtime commands,
  Events, logging, and test harnesses. Add only narrow HTTP/wait injection needed
  to test failures and ordering without network access or real delays.
- Do not add synchronization, caches of session attachments, automatic retries
  of allocations, startup recovery jobs, expiry, discovery, a cleanup daemon,
  a general job queue, new provider settings, or a citation UI.
- Keep the six-digit Ref ID input grammar. Do not add a numeric request-store
  limit without evidence for the supported API/model. Unused model settings must
  not block store operations; log a warning where appropriate.

Management UI and methods belong to the desktop notes bridge. Shared chat input
and request dispatch also serve ChaWeb; keep those paths consistent without
adding a ChaWeb notes-management API.

## 3. Delivery order

Implement the feature in three consecutive blocks, each planned to fit one
512K-token Codex session, including implementation, tests, fixes, and review.
The seven detailed stages are steps within these blocks. They do not require
separate sessions.

Each block must leave the affected build and tests passing. Register new production
and test sources in `CMakeLists.txt` in the block that adds them. Implement storage,
cleanup, and lifetime guards before exposing upload actions in the UI.

| Stage | Deliverable | Depends on |
| --- | --- | --- |
| 1 | Vault ownership, snapshot export, journal associations, deletion primitives | Existing storage |
| 2 | Service/key eligibility and small OpenAI/Google store clients | Stage 1 domain values |
| 3 | Upload and Remove workers, progress, cancellation, interruption | Stages 1–2 |
| 4 | Subtree deletion and vault/session lifecycle integration | Stages 1–3 |
| 5 | Folder refs, detach, request resolution, both inference adapters | Stages 1–2; lifecycle work from stage 4 |
| 6 | Native contract, key usage, approved menus and dialogs | Stages 3–5 |
| 7 | Integrated regression and live provider acceptance | Stages 1–6 |

### Session blocks

Three blocks provide complete, testable outcomes at the main integration
boundaries. Keep both providers together in each block and include failure-path
tests with the implementation. The 512K-token session budget provides room for
substantial work; individual files, provider adapters, and test runs are internal
steps, not additional blocks.

All blocks are **not started**. Execute B01, then B02, then B03.

| Block | Scope | Complete when |
| --- | --- | --- |
| [ ] B01 — Store backend and lifecycle | All of stages 1–4: schema, snapshot export, journal persistence, credential matching, both store clients, upload/indexing, cancellation, shared cleanup, subtree deletion, and vault/Welcome safeguards. | Application-level operations work for both providers with fake HTTP. Storage, worker, deletion, and lifecycle tests pass, including partial failures, unknown outcomes, stale workers, and atomic warnings. The backend is ready for chat and UI integration. |
| [ ] B02 — Chat, inference, and native API integration | All of stage 5 and stage 6 steps 1–3: folder refs and detach, per-target resolution, both inference adapters and continuation paths, bridge methods, DTOs/client guards, and API-key usage. | Reference, controller, provider, bridge, and client tests pass. Eligible answers carry current store IDs through continuation; exclusions, multicast errors, and budget handling work. Generated API types and typechecking pass, and the UI has a complete tested contract. |
| [ ] B03 — UI and feature acceptance | Stage 6 steps 4–8 and all of stage 7: approved menus/dialogs, asynchronous deletion/editor handling, recovery, integrated regression, native smoke checks, and both providers' live checks from section 5. | The complete user flows work, applicable integrated checks pass, and PRD acceptance coverage is reconciled with live results. Test resources are cleaned up. Any unavailable external prerequisite or failed live check is reported explicitly. |

B01 is the largest block because persistence, ownership, cleanup, and cancellation
form one connected backend change. Keeping them together avoids handing off a
partially safe remote-operation lifecycle. B02 completes the request and API
contracts. B03 connects the UI and validates the finished feature.

Use the stage order within each block. In B01, put cleanup, lifetime checks, and
local-deletion guards in place before exercising remote allocation. In B02,
implement and test both inference adapters before enabling chat resolution.
Keep both live provider flows in B03; they do not need separate planned sessions.
If credentials or a supported model are unavailable, finish the independent work
and record the specific acceptance check that remains outstanding.

### Starting and finishing a session

Use this prompt, replacing `B01` with the selected block:

```text
Implement block B01 in docs/store-plan.md within one 512K-token session. Read
AGENTS.md and the relevant design/PRD sections. Verify prerequisites against the
current code, then complete the block's implementation, tests, fixes, and diff
review. The completed design is approved. Preserve unrelated changes. Do not
start the next block. Update this block's checkbox and the latest handoff in
store-plan.md with the result, exact checks, remaining work, and next block.
Do not change other docs files.
```

The checkbox requires passing checks, not just written code. Keep the handoff to
completed behavior, relevant contract decisions, check commands/results, and any
unfinished work. If an unexpected issue prevents completion, keep the block open
and identify the precise continuation; do not split routine steps into new blocks.

**Latest handoff:** Planning only. No blocks have started. Next: B01.

### Stage 1 — Persistence and captured documents

**Files:** new `src/storage/folder_store_storage.{h,cpp}`;
`src/storage/{note_storage,workspace_session_database,session_database}.{h,cpp}`;
`src/workspace/workspace_config_store.{h,cpp}`.

1. Add the exact additive tables from design section 3: `folder_stores`,
   `folder_store_files`, and `session_folders`. Call their idempotent schema helper
   after note schema creation in both database entry points. Keep the current
   workspace schema-version convention and existing data.
2. Add small typed values and transaction-neutral queries for store lookup,
   subtree ownership, and attachment readiness. Expose application operations
   through `WorkspaceConfigStore` under its existing mutex. Use short owning
   transactions for registration, uncertainty changes, ID bookkeeping, readiness,
   and final removal. Every update checks the local store-record ID.
3. Validate ownership against a real folder row. Keep the unique folder
   constraint and restrictive note foreign key. Keep `session_folders` without
   a note foreign key because Welcome uses another database. OpenAI file rows
   are an owned-resource cleanup list only.
4. Add snapshot capture in one read transaction. Read the root path, descendant
   names/parents, IDs, and saved documents consistently. Export only ordinary
   notes through `note_document_markdown`; exclude generated annotations and
   compressed summaries. Detect empty exportable text before adding metadata,
   including empty headings/code blocks. Fail the entire preflight on a damaged
   nonempty document, allocation failure, or oversized final document.
5. Produce an in-memory vector containing each note's identity, safe unique
   filename, and UTF-8 Markdown with title, capture-time folder path, and Ref ID.
   Include the complete numeric note ID in filenames. Check final bytes including
   metadata against the design's provider-specific bounds; verify the applicable
   published bounds before encoding them. Release the read transaction before
   remote work. Do not persist a corpus or create temporary document files.
6. Add journal methods to list, add, detach, and clear folder associations. Add
   and detach commit their notice in the same transaction. For persistent
   sessions, recheck a new attachment's folder/readiness on the journal's own
   connection inside its write transaction. Never call `WorkspaceConfigStore`
   from inside that transaction. Welcome uses active-vault callback validation.
   Existing attachments remain idempotent even if the store is now unavailable.
7. Extend `clear_history()` to remove folder associations. Keep cover/uncover
   behavior and session-key cascading. Do not change `notes_prompt()`.
8. Split `note_delete` into its current public transaction-owning wrapper and a
   transaction-neutral helper. Add subtree association deletion and guarded
   store-row removal primitives so stage 4 can commit warnings, records, notes,
   and persistent folder refs together. Retain audio-attempt warnings, audio
   cleanup, annotation rebuilding, and existing error behavior.

**Verification:** add `tests/storage/unit_folder_store_storage.cpp`; extend
`unit_note_storage`, `unit_workspace_session_database`, and
`unit_workspace_config_store`. Cover old and new vaults, Welcome Clear with no
attachments, unique ownership, invalid/root/note IDs, fresh attempt IDs,
ready-state constraints, and journal rollback if notice insertion fails. Test
nested editable notes, duplicate titles, empty formatting, full versus compressed
text, metadata/filename escaping, export errors, and final byte boundaries.
Exercise attach/delete on separate connections to prove no dangling persistent
association can commit. Test deletion rollback and preservation of existing
annotation/audio behavior.

**Complete when:** storage can represent a removable partial upload, capture a
consistent snapshot, and manage folder attachments without provider calls.

### Stage 2 — Eligibility and remote operations

**Files:** new `src/providers/folder_store_client.{h,cpp}` and a small shared
eligibility helper, colocated there unless a separate header materially improves
dependencies; existing `ApiKeyStore` and `ModelBackendConfig` interfaces.

1. Implement one eligibility function used by both upload choices and request
   resolution. Check network mode, API-key authentication, HTTPS, exact host,
   port 443, and API/base path: OpenAI Responses on `api.openai.com` with an empty
   base path; native Google on `generativelanguage.googleapis.com` with
   `/v1beta`. Normalize host case only as specified by the design.
2. Resolve legacy key-name references through `ApiKeyStore` and return stable
   saved IDs. Build choices from compatible configured connections, deduplicated
   by service/key ID, requiring a saved nonempty value. Return omission reasons
   for incompatible connections. Removal uses the record's service/key even
   when the original model configuration no longer exists.
3. Build narrow create, upload, attach, inspect, and delete functions using
   libcurl and fixed service origins. Keep persistence and polling coordination
   outside the client. Inject a small transport for deterministic tests; do not
   expand `ProviderClient`'s inference POST transport into a management API.
4. Implement the OpenAI sequence from design section 7.1: vector-store creation,
   Markdown Files upload with `purpose=assistants`, attachment, per-file indexing
   inspection, vector-store deletion, and owned-file deletion. Return file IDs
   before attachment so the caller can persist them. Distinguish successful
   transfer from completed indexing.
5. Implement Google direct-to-store media upload, operation inspection, store
   inspection, and force deletion. Validate resource names and upload-session
   destinations before authenticated follow-up calls. Retain secret session URLs
   only in memory. Parse decimal-string document counts and preserve the
   distinction between pending/short counts, failure, and excess active count.
   Do not introduce a separate raw Files upload/import path.
6. Define explicit outcomes for confirmed deletion, authenticated structured
   not-found, definite rejection, and ambiguous allocation. Generic/malformed
   404 responses, 401/403, and network failures are not cleanup success. Return
   safe errors with enough identity for the application to name the failing note.
   Never include note bodies, credentials, or secret URLs in status/log output.

**Verification:** add `tests/providers/unit_folder_store_client.cpp`. Test both
services' request methods, paths, authentication, MIME types, resource validation,
index statuses, Google counts, and error classification with injected responses.
Test the complete eligibility matrix, legacy-name resolution, duplicate configs,
different saved IDs with equal values, missing/empty keys, and irrelevant settings.
Use no live credentials in these tests.

**Complete when:** the application can call every required remote step and tell
whether a resource ID must be saved, retried for cleanup, or reported as unknown.

### Stage 3 — Upload and Remove workers

**Files:** new `src/app/folder_store_operations.cpp`;
`src/app/application.{h,cpp}`, `application_internal.h`, `vault_operations.cpp`;
small test-injection additions to `src/runtime/runtime_settings.h` if needed.

1. Add an application-owned control map guarded by `lifecycle_mutex`, using
   `BackgroundJobs` for execution. A control carries operation identity, epoch,
   folder/root ID, current store-record ID, phase/counts, safe result, and separate
   user-cancel/lifecycle-interruption state. Reject duplicate work and conflicts
   with reserved delete subtrees. Require main-process mutation admission.
2. Reserve before asynchronous preparation. Revalidate choices, capture the
   saved snapshot, and register an incomplete attempt before remote creation.
   Preflight or launch failure releases admission and returns an editable error;
   it must not allocate remote resources.
3. Before an allocating request, commit `remote_outcome_unknown`. Log the returned
   ID before attempting persistence; save it and clear uncertainty atomically
   before the next remote step. Clear uncertainty on a definite no-side-effect
   rejection. Stop after an ambiguous allocation; do not resubmit it.
4. Upload documents sequentially, attaching each OpenAI file after saving its ID.
   Keep indexing state beside the captured document. Check immediate errors,
   finish transfers, then poll pending entries in rounds with one interruptible
   wait per round. For Google, after all operations succeed, wait for the expected
   active count and zero pending/failed documents. Short counts remain Indexing.
5. Serialize cancellation and final readiness through the operation guard.
   Recheck lifetime, attempt identity, cancellation, and every document result;
   commit ready state, timestamp, and success event together before publishing
   completion.
6. Implement the shared cleanup routine. Mark Remove incomplete before its first
   remote request. Delete the OpenAI vector store first, then attempt every owned
   file even if another deletion fails; force-delete Google stores. Persist each
   confirmed result before continuing. Retried cleanup reads only remaining IDs.
   Standalone Remove preserves local notes and all session-folder associations.
7. Commit a not-found warning with the ID removal, including folder, service,
   saved key ID, and remote ID. Once all known resources are cleared, finalize
   the local association with the required uncertainty warning. Preserve the
   distinction between confirmed remote deletion and known-resource/local
   cleanup, including on retry after interruption. Do not add a warning-history
   registry or an uncertainty-only retry loop.
8. User Cancel stops forward work, interrupts non-allocating requests, and wakes
   waits. Let an already-sent allocation or Google upload/finalize return within
   its request timeout; save any returned IDs and clean them up. Cleanup observes
   lifecycle interruption, not the upload-cancel flag. Cancellation after the
   ready commit is a no-op. Normal failures stay visible after successful cleanup.
9. Interrupt and retire controls in maintenance drain and shutdown. Do not join
   store workers while holding the lifecycle lock or add a drain timeout. Before
   each new remote step and vault write, check admission, epoch, control, and
   attempt identity. Late IDs after interruption go to diagnostics with original
   vault context; they must not write to the current vault or reopen the old one.
   Keep the existing shutdown dependency-retention policy.
10. Handle a local write failure after remote success explicitly: stop forward
    work, retain returned IDs in the control, attempt cleanup only in the same
    valid lifetime, and report unresolved IDs safely. Durable uncertainty remains
    the restart evidence if both persistence and cleanup fail.

**Verification:** add `tests/app/unit_folder_store_operations.cpp`. Use injected
transport, waits, and barriers to test sequencing; cancellation during preparation,
allocation, upload, indexing, and the ready commit; late responses; ambiguous
results; missing/replaced keys; launch and write failures; partial cleanup; and
not-found warning atomicity. Verify reopening performs no remote work. Test a
maintenance drain while a worker is waiting for the lifecycle lock, stale workers
after switch, and shutdown dependency retention. Avoid real sleeps.

**Complete when:** both providers can upload and remove through application
operations, with failures recoverable before any UI is connected.

### Stage 4 — Folder deletion and vault lifetime

**Files:** `src/app/{folder_store_operations,notes_operations,vault_operations}.cpp`;
`src/storage/session_repository.{h,cpp}`; existing live-session/runtime command
path in `src/runtime/live_session{,_manager}.{h,cpp}`; stage 1 storage helpers.

1. Keep local-only deletion unchanged for subtrees with no store records. Reject
   legacy `notes.delete` when any owned store exists below the target, including
   an incomplete record with no returned ID. Collect descendant folder IDs as
   well as affected compression controls before the final delete.
2. On confirmed folder deletion, recompute the store count and reserve the
   subtree under admission protection. Reject a changed count as ordinary
   validation. Reject overlapping store/delete work and structural changes that
   alter membership, including create/import into the reserved subtree. Check
   existing mutation entry points; do not add a new move feature for this work.
   Ordinary note saves can continue.
3. Mark all affected stores incomplete in one transaction before remote cleanup.
   Process stores sequentially, retaining per-folder errors. Keep successful
   cleanup committed. Retain uncertainty-bearing rows until the final local
   deletion transaction so another store's failure cannot lose warning context.
4. When known cleanup completes, commit uncertainty warnings, remaining store
   removal, subtree deletion, and persistent association deletion together.
   Preserve compression cancellation and note-audio abandonment after commit.
   Ancestor stores outside the deleted subtree remain unchanged.
5. On known-resource failure, retain the control and subtree reservation. Retry
   uses the same operation and remaining IDs. Cancel/Close releases the
   reservation while keeping notes and incomplete records. Delete anyway is
   accepted only for that failed operation and epoch; write all unconfirmed
   resource details and perform local deletion in one transaction, with no more
   remote requests. Warning-write failure rolls everything local back.
6. After local deletion, enqueue Welcome association pruning on the existing
   runtime with the originating epoch. Do not access its live journal from the
   worker. Serialize attach and prune through that runtime; stale prune commands
   cannot affect another vault.
7. In `MaintenanceGuard::retarget`, clear only Welcome folder associations when
   the database path changes, while live controllers are drained. Keep its
   transcript and note/file copies. Same-path password changes preserve refs.
8. After copying a source for `create_vault`, clear only the destination's store
   and owned-file rows in a transaction before publishing the new vault. Keep
   copied session folder refs. Handle old sources without the tables. On failure,
   remove the newly created destination and definition through existing safe
   file-cleanup conventions. Do not change generic copying, password protection,
   backup/restore, or configuration-merge ownership behavior.

**Verification:** extend store-operation, `unit_vault_maintenance`,
`unit_session_repository`, and live-session tests. Cover nested independent
stores, an empty folder with a store, ancestor preservation, overlapping requests,
membership changes, failed-delete reservation recovery, Retry/Cancel/Delete anyway,
and atomic warning rollback. Test persistent attach/delete races and Welcome
attach/prune ordering, including ID reuse across vaults. Compare source copying
with same-vault password protection; verify the source retains ownership and a
failed child copy is not published. Include configuration merge changing a key's
value and restore/reopen of incomplete or stale records.

**Complete when:** all local deletion paths preserve cleanup evidence, and a
vault change cannot transfer ownership or allow stale worker writes.

### Stage 5 — References and inference

**Files:** `src/runtime/text_input.{h,cpp}`;
`src/session/session_open.cpp`, `session_controller.{h,cpp}`;
`src/characters/model_context.h`; `src/providers/{responses_api,google_api,
provider_client,tool_calls}.cpp` and associated headers where needed.

1. Rename the internal reference parser to an item-oriented name without changing
   its base62 alphabet or six-digit, whole-input grammar. Resolve item metadata
   before dispatch: preserve note/annotation behavior, attach a ready folder,
   and give the upload-first notice for a new unavailable folder. A ready folder
   can attach even if no current answering target is compatible. No generation
   occurs for attach or detach.
2. Add `/detach` with exactly one valid Ref ID and the specified usage error.
   Remove only a folder association; work without a current store, compatible
   target, saved key, or available folder name. Use the Ref ID when needed.
   Preserve expiry/busy guards, historical notices, and ordinary note attachments.
3. Add narrow item/store callbacks from `session_open`. Read attached IDs from
   the journal when dispatching ordinary answers, then current store records.
   Skip maintenance and auxiliary requests before store lookup. Resolve each
   target's effective connection with the shared eligibility helper. Deduplicate
   IDs and combine omissions into one concise turn notice with detach commands.
4. Add optional `FolderSearchAccess` to `GenerationRequest`. Capture immutable
   service/remote IDs for that logical answer; keep folder labels/refs in error
   context. Continuations reuse these IDs. Replacement uploads and configuration
   changes affect the next answer. A failure affects only its multicast target;
   never retry without stores, choose a subset, or copy note text into the prompt.
5. Append one Responses `file_search` tool to the existing tools. Preserve the
   initial auto/required choice and existing continuation behavior. Preserve
   hosted output needed for replay with `store: false`; do not treat File Search
   output as a local function call. Leave answer rendering and citations unchanged.
6. Append native Google `fileSearch`. Keep the existing model, endpoint, generation
   settings, streaming, cancellation, and usage behavior. Implement the design's
   small mixed-tool capability decision separately from credential matching:
   known unsupported combinations omit optional CHA functions with a warning;
   documented combinations use `VALIDATED` and server-side invocation support;
   unknown models surface rejections rather than silently losing tools. Verify
   current documented model support during acceptance.
7. Retain Google's returned model parts, hosted tool parts, IDs, and signatures
   through the existing loop; preserve `functionResponse.id`. Change Google
   tool insertion and availability inspection to find optional declaration
   groups instead of assuming every tool has declarations or the first entry
   contains them.
8. At the local function budget, Responses keeps only File Search with auto
   choice. Google retains File Search and continuation parts, uses mode `NONE`,
   and preserves server-side invocation support. Recompute web/folder availability
   separately and change final-round instructions so they disable CHA functions
   without forbidding hosted retrieval. Keep no-store budget behavior unchanged.
9. Add folder names and detach guidance to store-related provider errors. Do not
   introduce the speculative limit helper or preflight-error request field
   removed by the design review. If a current applicable limit is verified,
   implement only that rule at attachment and per-target dispatch, with tests.

**Verification:** extend `unit_text_input`, `unit_text_command`,
`unit_session_controller`, `unit_session_open`, and multicast tests for stable
refs, attach/detach, restart/Clear/cover, replacement stores, omissions, and target
isolation. Extend `unit_responses_api`, `unit_google_api`, and the existing
`unit_provider_client` harness for serialization and continuation, including
mixed streaming parts, signatures, function IDs, and budget exhaustion. Assert
maintenance/auxiliary paths never perform store lookup. Retain regression cases
for no-store requests, ordinary note refs, subscription/proxy endpoints, and
Google Chat Completions. Inject provider count/access/model rejection and check
detach recovery; add numeric-limit cases only when a limit is verified.

**Complete when:** an attached folder supplies only compatible hosted access to
ordinary answers and retains that access throughout supported continuations.

### Stage 6 — Native contract and UI

**Files:** `resources/dto.yaml`, generated `webapp/src/api/schema.d.ts`;
`src/bridge/{bridge_protocol,notes_dispatch}.{h,cpp}` as applicable;
`src/app/settings_operations.{h,cpp}` and application settings call sites;
`webapp/src/api/{client,nativeClient}.ts`; `webapp/src/components/{NotesTree,App}.tsx`;
new `webapp/src/components/FolderStoreDialog.tsx`.

1. Implement the seven methods in design section 11: `notes.storeChoices`,
   `storeUpload`, `storeRemove`, `storeOperation`, `storeCancel`, `storeClose`, and
   `notes.folderDelete`. Validate epochs and operation/record IDs. Start calls
   return promptly; status carries phases, counts, safe results, and remaining
   cleanup. Retry and Delete anyway use the original failed delete operation.
2. Add optional ready/incomplete store summaries to folder listing DTOs. Keep
   remote IDs out of normal menu payloads. Derive confirmation counts from the
   existing full listing; do not add a delete-info endpoint. Update native method
   policy tables, strict response guards, fixtures, and client methods together.
   Generate API types from the YAML source.
3. Add store usage to every API-key detail/list response that exposes `used_by`,
   including after key edits. Read current ownership rows and folder names;
   include incomplete records. Preserve rename, replace, and delete behavior.
4. Add Copy Ref ID for real folders using the existing formatter. Add Upload or
   Remove based on record existence, preserving current menu order. Exclude the
   synthetic root. Query operation status when reopening a menu/dialog so active
   or failed work is recovered rather than submitted again.
5. Implement the approved upload/removal states in `FolderStoreDialog`: backend
   service/key choices, single-key selection, actionable zero-key/preflight
   errors, upload/index progress, cancellation cleanup, failure/Close, and warning
   completion. Do not force an editor save before upload. Give the upload dialog
   its specified accessible name without a visible panel title or helper copy.
6. Extend the existing folder-delete dialog for store count, pending cleanup,
   failures with Retry/Cancel/Delete anyway, and uncertainty warning/Close. Empty
   folders with stores must still confirm. A count mismatch refreshes the listing
   and leaves deletion unstarted. Do not allow the UI to discard active cleanup.
7. Adapt `App.tsx`'s existing note-deletion callback and editor-save coordination
   to the operation lifecycle. Acceptance of a worker is not local deletion:
   dispose/close the affected editor only after confirmed local deletion. Retain
   the existing in-flight-save protection for each running delete attempt and
   release it on failure or cancellation. On recovered status, reconcile the
   editor with the completed deletion. Keep local-only deletion behavior intact.
8. Poll status only while the dialog is active and refresh listings on outcomes.
   A UI disconnect does not cancel work or release a failed-delete reservation.
   Explicit Close acknowledges terminal state; stale commands cannot affect a
   replacement. Invalidate local UI state on epoch change. Preserve focus return,
   keyboard behavior, and duplicate-submission prevention.

**Verification:** extend bridge protocol/router tests and wire fixtures;
`nativeClient.test.ts`, `NotesTree.test.tsx`, `Notes.test.tsx`, settings tests, and
add focused dialog tests. Test the choice matrix, folder/root menus, saved versus
unsaved content, preflight versus remote failure, cancellation, warning outcomes,
empty-folder confirmation, changed counts, retained reservations, UI reload,
stale IDs/epochs, and editor saves during failed/retried deletion. Check focus,
Escape/Close behavior in each phase, accessible names, and no permanent polling.

**Complete when:** desktop users can perform the approved flows and recover from
failure or UI reconnection without bypassing backend cleanup rules.

### Stage 7 — Integration and acceptance

Run focused tests during each stage. Once integrated, run the normal native and
frontend checks from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
npm --prefix webapp run api-types:check
npm --prefix webapp run typecheck
npm --prefix webapp test
make itest-local
npm --prefix webapp run build
npm --prefix webapp run build:chaweb
```

Run `npm --prefix webapp run api-types` when changing the DTO source. Use the
existing native UI harness for a focused dialog/editor smoke check. Exercise
the relevant desktop platform build paths when available; report any untested
platform instead of treating a frontend build as native-host coverage. For the
new worker races, use the existing sanitizer presets if deterministic lifecycle
tests leave unresolved concurrency concerns.

Keep paid provider checks separate from the default suite. Extend the existing
integration harness only where it helps repeat the small flows below; do not run
the unrelated full live/R2 suite merely to validate stores.

## 4. Acceptance coverage

| PRD criteria | Required evidence | Stages |
| --- | --- | --- |
| 1–2 | Stable folder refs; root exclusion; correct menus; exact service/key choices and deduplication | 2, 5, 6 |
| 3 | Consistent full saved subtree; editable notes included; empty/annotation/binary/summary/unsaved content excluded; unique filenames | 1, 6 |
| 4 | Preflight has no remote effect; every document indexed; duplicate work rejected; cancellation cannot publish readiness | 1–3, 6 |
| 5 | Local edits leave snapshot unchanged; re-upload refreshes it under the same folder ref | 1, 5, live flow |
| 6 | Attach/detach without generation; journal lifetime, restart, Clear, cover/uncover; note-ref regression | 1, 4, 5 |
| 7–8 | Per-target matching; isolated errors; both protocols; mixed-tool continuation and budget; maintenance/auxiliary exclusions | 2, 5, live flow |
| 9 | All owned resources cleaned; local notes retained; replacement resolved by existing attachments | 3, 5, live flow |
| 10 | Nested cleanup, offline/missing-key/access failures, Retry/Cancel/Delete anyway, warning atomicity, empty-folder confirmation | 4, 6 |
| 11–12 | Interruption/reopen, ambiguous allocation, stale workers, externally removed resources, changed keys, preserved unrelated requests | 2–5 |
| 13 | Unexpected provider-count errors and detach recovery; attachment/dispatch limits only for a verified applicable rule | 5, live validation |

Also require the design's vault-copy/password distinction, Welcome identity
isolation, not-found warning persistence, and UI reconnection cases. These protect
ownership and recovery even though they are not separate numbered PRD criteria.

## 5. Live checks and completion conditions

For each provider, use a small explicitly designated temporary test folder and an
eligible saved key. Record actual model IDs, protocol, streaming mode, results,
and cleanup outcome. Keep secrets and note bodies out of diagnostics.

1. Create nested notes with a distinctive fictional fact. Upload, wait for full
   indexing, submit the parent folder Ref ID, and ask an ordinary question that
   requires the fact. Upload success alone does not prove inference support.
2. Edit the local fact and verify the existing store remains the old snapshot.
   Remove and re-upload, then ask through the existing attachment to verify it
   resolves the replacement. Detach and verify the next request has no store.
3. Test OpenAI File Search alongside a CHA function continuation with the current
   `store: false` behavior. Verify the final budget round keeps only File Search
   and does not silently change persistence or discard required output items.
4. Test Google native streaming and non-streaming retrieval. On a supported
   mixed-tool model, exercise File Search and a CHA web function in the same
   answer, replay returned parts/signatures/IDs, and reach the function-budget
   round using `NONE`. Verify store-count lag keeps Indexing active. A provider
   rejection is a failed acceptance check for that model/path, not permission to
   drop stores, switch models, or change protocols.
5. Verify deletion responses for externally removed test resources and, where
   a suitable second project credential is available, wrong-project access.
   Preserve IDs in warning events on structured not-found and retain IDs on
   authorization failure. If this setup is unavailable, record the live check as
   unverified; fake responses do not establish the provider's actual behavior.
6. Check current official model/API documentation for applicable request-count
   limits and mixed-tool support. Do not transfer the historical deep-research
   limit to other models. Record known-limit cases as not applicable if no rule
   is verified, while still testing provider rejection and detach recovery.
7. Remove every test-created store and owned file. Record confirmed cleanup or
   the remaining manual-cleanup IDs/unknown outcomes. Do not leave a successful
   upload as the final test state.

Use the official links already collected in design sections 5, 7, 8, and 9 when
checking provider contracts. The unresolved checks are native Google model
support, mixed-tool `VALIDATED`/`NONE` continuation, Google operation/count
consistency, OpenAI hosted-output replay, and wrong-project deletion responses.
They need evidence from the implemented request path, not another design layer.

The implementation is complete when the automated acceptance cases pass, the
required live retrieval/continuation flows succeed on the recorded models, and
test-resource cleanup is confirmed. Unavailable live checks, provider limitations,
and unresolved cleanup must be reported as outstanding acceptance work, with
known IDs preserved for recovery. Review the final diff for unnecessary state,
retries, timers, and duplicated paths. Update this plan with implementation results
only when requested; do not change other `docs/` files as an incidental
implementation task.
