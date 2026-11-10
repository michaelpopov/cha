# Block 2 — Chats, Worker, and application integration

Status: Not started. Implementation instructions for the second of three
sequential agent sessions; requires completed block 1.

## Suggested session prompt

```text
Implement docs/block2.md using the current code, specs/d1-alt-design.md,
and specs/d1-alt-plan.md. Verify block 1's code and handoff first. Follow
AGENTS.md. Complete validation and handoff. You may update the Execution
record section of docs/block2.md with results and instructions for the
next session. Leave other docs/ files unchanged. Ask if an actual conflict
needs a design decision.
```

## 1. Objective and prerequisites

Complete the backend synchronization feature: chat records, the Worker, vault
and R2 integration, daemon startup, native shutdown, and backend D1 credential
operations. This covers plan phases 5–6 and the application-layer portion of
phase 7. Block 3 owns the bridge/DTO/frontend portion of phase 7 and final
feature validation.

Read [AGENTS.md](../AGENTS.md), [CLAUDE.md](../CLAUDE.md), the complete
[design](../specs/d1-alt-design.md), and
[plan](../specs/d1-alt-plan.md), especially sections 9–12. Read block 1's
[execution record](block1.md#execution-record--to-be-filled-by-the-implementing-session)
and inspect the actual code it delivered. No previous chat is needed.

Confirm these prerequisites before dependent changes:

- The schema, machine setting, stored D1 credentials, whole-record local queue,
  and atomic queue/flag cleanup exist and are tested.
- The D1 client supports discovery, table creation, batch inserts, ordered reads,
  handled-prefix deletion, counts, and drops, with distinguishable error results.
- Note and configuration producers/apply operations are real code, not stubs.
  Incoming application creates no outgoing work. Import and merge preserve
  destination credentials and pending records. On import, merge, or receipt,
  a collision with an ordinary key moves only the local D1 key to a free ID;
  ordinary key IDs and references remain intact, and receipt creates no echo.
- Required live feasibility checks passed. Desktop/ChaWeb reservation findings
  are recorded. Resolve any blocking missing prerequisite before building on it.

Inspect the current diff and preserve unrelated changes. Reuse implemented
interfaces rather than recreating block 1 under new names. Fix integration
defects where they belong, but keep the approved design.

Plan for 300K–400K tokens including implementation, tests, and fixes, leaving
about 100K below the 500K context window. This is an estimate, not a guarantee.
Read focused sections of large files and the relevant tests. Do not load all
provider or UI code. Block 3 should be able to consume your application APIs and
validation record without rereading this session.

## 2. Settled decisions

- One person uses one machine at a time. Duplicates, partial remote writes,
  registration gaps, and configured-ID collisions remain accepted.
- Keep one Worker thread and the 30-second loop. No registry, delivery IDs,
  receipt ledger, persistent receive cursor, second outgoing buffer, retry
  scheduler, or shared-chat flag.
- Received changes never enqueue records or set note flags. Keep suppression
  scoped to the operation; do not suppress unrelated local saves globally.
- Reserve, apply, and release each incoming chat record. Do not add batching
  of reservations for consecutive records or a map of held reservations.
- Ordinary restarts and recreation of a missing daemon D1 table retain pending
  local work. New deliveries clear queue/flags in their existing creation or
  download operation. No cleanup marker or deferred first-unlock workflow.
- **Application close waits for the full final send attempt.** This is an
  explicit user decision. Retain per-request timeouts and stop the send pass
  at its first failure, preserving unsent records. Add no overall drain timer.
- The completed design is approved. Ask only about a real conflict or missing
  information; do not reopen settled decisions.

## 3. Code entry points

| Area | Start here |
|---|---|
| Journals and closed chats | `src/storage/session_database.{h,cpp}`, `src/storage/session_repository.{h,cpp}` |
| Chat repair and mutation | `src/session/session_controller.cpp`, `src/session/session_open.cpp` |
| Reservations and live runtime | `src/runtime/live_session_manager.{h,cpp}`, `src/runtime/live_session.cpp` |
| Worker ownership and shutdown | `src/app/application.{h,cpp}`, `src/app/application_internal.h`, `src/app/background_jobs.{h,cpp}` |
| Vault and R2 maintenance | `src/app/vault_operations.cpp`, `src/app/r2_database_transfer.{h,cpp}` |
| Configuration and notes | Block 1 apply operations, `src/app/workspace_operations.cpp`, `src/notes/notes_service.cpp` |
| Credential application operations | `src/app/settings_operations.{h,cpp}` and their application call sites |
| Native hosts | `packaging/host/runtime_bridge.{h,cpp}`, `packaging/macos/main.swift`, `packaging/windows/main.cpp` |
| Daemon | `src/daemon/main.cpp` |
| Tests | `tests/storage`, `tests/session`, `tests/runtime`, `tests/app`, `tests/native` |

The proposed `src/app/d1_sync.{h,cpp}` is a small Worker implementation, not a
new runtime framework. Keep database ownership and existing lock boundaries
explicit. Never wait on network or runtime release while holding a SQLite
write transaction.

## 4. Implementation scope

### A. Outgoing chats

Use the predicate from design section 8.3: a non-temporary label,
`next_request_id > 1`, and no running turn with request ID 1. Clear and turn
deletion do not reset the counter. Derive eligibility from stored data.

Check eligibility before and after the operation. On the first transition to
shared, emit the initial finished turns, `@-` self-notes, attachments, and
character settings in the specified order. Otherwise emit the operation's
record if shared. These are exclusive branches; do not duplicate the trigger's
turn. Deletion checks pre-delete state.

Place completion/cancellation/failure hooks in the `SessionJournal` transaction
path, including `complete_turn`, `cancel_turn`, and `fail_turn`. This covers
interrupted-turn repair when a chat reopens. A finished first turn can be
complete, cancelled, or failed. Preserve the existing cancelled turn with no
response entry; never send an unfinished prompt alone.

Implement every chat record in design section 7.2: entries, rename, delete,
clear, positional turn delete, note/file/folder attachment, folder detach, and
character override changes/removal. Include repository operations on closed
chats. Every record carries the forum/session identity and label. Entries have
no sender-local numbers. Calculate a deleted turn's one-based position before
deleting it. A deleted source note makes its copied chat attachment ineligible
for transmission (R55). Welcome and standalone notices/errors do not move.

### B. Incoming chats

Extend the existing maintenance reservation to use reason `reloading`. It must
stop an open controller and wait for its release before database mutation, so
the controller cannot write using old entry counters. Preserve normal saving of
a cancelled local reply. Apply the received record in its own transaction with
recording disabled, then release the reservation.

A reservation failure pauses the receive pass and retains the record and all
following rows in D1. An invalid/unapplicable record after successful reservation
is warned about and skipped under R41. Do not treat these as the same outcome.

Skip missing forums. Create a missing chat with the carried identity and label,
using existing zero defaults for `recent_pending` and `discardable`. Allocate
receiver-local request/entry numbers, update existing counters, and preserve
terminal state and metadata. Resolve attachments by local path and turn deletes
by position. Reopening a chat must read the new data and counters.

Backend reservation correctness belongs here. Production frontend recovery
changes belong to block 3; pass it the tested runtime behavior and block 1's
client findings.

### C. Worker loop

Own one Worker in `Application::Impl`. Use a single serialized loop for startup,
periodic work, and send-only flushes; tests may call it without sleeping.

1. **Collect flags.** Convert flagged notes to outgoing records through block 1.
2. **Send.** If work exists, discover this vault's tables and check that this
   machine's table exists before sending anything. Send local records in
   `rowid` order. Split serialized UTF-8 at character boundaries into pieces
   of at most 1,900,000 bytes. One piece is one batch to all other recipients.
   Delete the local record only when all pieces succeed, or when there are no
   recipients. On ordinary failure, retain it and stop sending in this loop;
   the next loop retries from the first piece.
3. **Handle full storage.** Only for the identified full-database error, count
   eligible queue rows, exclude all tables of this machine, drop the largest
   eligible table, and stop sending until the next loop. Drop at most one
   table per loop. Discovery/drop failure or no eligible table retains work.
4. **Receive.** Read in `rowid` order, initially 50 rows. On the specific
   result-size error halve the read size down to one; reset to 50 next loop.
   Apply complete records in queue order, each in its own local transaction.
5. **Acknowledge.** Delete the contiguous handled prefix in one request per
   handled group. Continue until no further rows can be handled. A read/delete
   transport failure ends the pass; uncertain acknowledgement may replay.

Assemble split records across read groups in memory for this pass. Keep the
temporary read position separate from the last handled row. An incomplete set
at queue end remains in D1 and is reread next loop. A broken set followed by
another record is warned about and skipped, then the new row is handled
normally. Retain no assembly state between loops. Do not acknowledge incomplete
pieces or a record whose reservation failed.

Use block 1's note/configuration operations and the chat operations above.
After a receive group changes notes, issue one application-level change
notification through existing epoch-aware callback patterns. Block 3 owns the
bridge event and screen subscriber, not another backend notification mechanism.

### D. Application lifetime and shutdown

At vault open, initialize storage/credentials and run the first loop before
screens load. Network failure logs a warning and permits opening. A missing
own D1 table stops the Worker with the design's recovery error.

On close/switch/maintenance, stop receiving and close user admission. Let
controllers/jobs finish or cancel while outgoing hooks still have database
access. Then collect flags, attempt the final send, and join before database or
password release. Restart the selected vault's Worker after successful
maintenance, or the restored old vault after a failed operation.

Use the existing lifecycle and context epoch. Do not join a Worker under a lock
it needs or call a maintenance wrapper that makes the Worker join itself.
Serialize disconnect/credential replacement with Worker activity. Deliver
notifications after releasing relevant locks.

Both native hosts currently impose a 10-second shutdown wait. Adjust their
existing background shutdown/join path, including the shared runtime bridge as
needed, so the final D1 send pass can finish beyond that limit. Keep the normal
request timeouts. A failed request ends sending; unsent records remain. Do not
add a second shutdown scheduler or substitute a larger arbitrary drain timeout.

### E. R2, new vaults, and daemon

- During existing upload preparation, collect flags and attempt sending once.
  While local writers remain excluded, refuse the upload if records or flags
  remain, before producing its snapshot. Use the approved error: "Changes are
  not sent to D1 yet. Try again when D1 can be reached." Preserve R2 ETag and
  overwrite behavior.
- In both `download_database_from_r2` and `download_new_database_from_r2`,
  validate the temporary database, ensure sync schema, clear queue/flags in
  one transaction, and destroy the cleanup statements and database handle.
  Then reuse `checkpoint_workspace_session_database` with the temporary path
  and its password. Its non-passive mode uses `wal_checkpoint(TRUNCATE)` and
  rejects a busy checkpoint. Let its statements/handle close before the
  existing `remove_sidecars` and `publish_downloads` calls. Failure aborts
  installation through the existing download failure path. Preserve downloaded
  content/D1 credentials and records waiting remotely in D1.
- Existing protected-vault replacement already has its password. New-vault
  download currently refuses protected databases without a password; keep that
  behavior. Cleanup is immediate and requires no first-unlock state.
- After replacement of the open vault, create its own D1 queue if connected,
  then open/run normally. Preserve explicit Save-credentials recovery for a
  newly downloaded unopened vault, a locally created copy, or a changed machine
  name, as design section 12.3 specifies.
- In existing desktop `create_vault(copy_from)`, clean the destination before
  returning it; retain source pending work. Do not change the generic database
  copy helper used for maintenance/password protection.
- Only `cha-daemon` startup creates a missing own D1 table automatically. A
  table lost during execution stops its Worker until restart. Table recreation
  must not clear local work. Add no ChaWeb Settings/R2 UI or Linux copy workflow.

### F. Backend credential operations for block 3

Deliver real application operations for get, save, and disconnect; use the
existing credential storage and expose token presence, not the raw token.

Save accepts unchanged valid credentials because it is also table recovery.
Require the machine name, create the target D1 table first, save credentials
only after that succeeds, and start/restart the Worker. Failure leaves stored
credentials unchanged. An omitted token retains the current token.

Disconnect stops Worker access, then removes local credentials and clears the
queue/flags together. Using the captured old credentials, try once to drop this
machine's remote table. Remote failure only logs a warning; offline local
disconnect succeeds. D1 credential edits create no outgoing record.

Block 3 will expose these operations through method policies, DTOs, typed clients,
and the approved Settings screen. Do not build those UI contracts twice.

## 5. Validation and completion criteria

Register tests in existing targets. Build with `cmake --preset ninja` and
`cmake --build --preset ninja`, and run focused storage/session/runtime/app/native
tests using discovered CTest names. Use temporary vaults and loopback HTTP for
failure cases. Do not make the routine test suite depend on live D1.

Required coverage:

- First chat publication with either title/response order, every terminal state,
  interrupted-turn repair, `@-`, attachments/overrides, and unopened chat edits.
  Clear or last-turn deletion followed by restart does not lose eligibility.
- Missing chat creation, local numbering, positional deletion, deleted-source
  omission, reservation during generation, deferred receipt, and no echo.
- Send failure/retry, no recipients, missing own table before send, split sets
  across pages/loops, partial writes, oversized results, invalid records,
  acknowledgement failure, and eviction of at most one eligible table.
- Real application wiring for offline open, switch, failed maintenance recovery,
  final flush after cancellation, and no access to an old database/password.
- R2 upload refusal; cleanup before installation on both download paths;
  existing protected replacement; unchanged protected-new-download refusal;
  clean desktop copy with source work retained; daemon restart retains work.
  For download cleanup, reopen the installed database through a fresh handle
  after temporary sidecars are gone and assert an empty queue and zero flags.
  Include a WAL-mode fixture and checkpoint failure/busy rejection; the
  existing destination must not be replaced when cleanup/checkpoint fails.
- Credential get/save failure, unchanged Save, omitted token, and offline
  disconnect, tested through application operations before the UI exists.
- A send pass longer than 10 seconds is allowed to finish by the native shutdown
  path. Sending failure stops the pass and preserves unsent work on reopen.
  Record which native platforms were actually exercised.

Block 2 is complete when the backend can synchronize two temporary vaults through
the real application/Worker path with the scripted transport, and all assigned
checks pass. Screens and bridge exposure may still be absent. Carry unavailable
platform execution as an explicit validation limit for block 3; do not claim
those platforms were tested.

## 6. Handoff instructions

Review for unnecessary state, scheduling, abstractions, and unassigned UI work.
The suggested session prompt explicitly includes updating this guide's execution
record. Limit the handoff documentation edit to that section. Preserve unrelated
edits and leave other documentation unchanged. Report any real blocker instead
of declaring a partial backend complete.

Give block 3 exact application APIs, event/callback payloads, error results,
token-update semantics, and shutdown integration points. Also list tests and
platform gaps. These implemented interfaces are the handoff, not hypothetical
names that force block 3 to add adapters.

### Execution record — filled by the implementing session

- Status: Complete.
- Code revision / relevant working-tree files: HEAD f9f27e59 plus uncommitted block2 work. New: src/storage/sync_chat.{h,cpp}, src/app/d1_sync.{h,cpp}, tests/storage/unit_sync_chat.cpp, tests/app/unit_d1_sync.cpp. Modified: CMakeLists.txt, src/storage/session_database.{h,cpp}, src/storage/session_repository.{h,cpp}, src/session/session_controller.{h,cpp}, src/session/session_open.cpp, src/runtime/live_session_manager.{h,cpp}, src/runtime/protocol.{h,cpp}, src/app/application.{h,cpp}, src/app/application_internal.h, src/app/vault_operations.cpp, src/app/r2_database_transfer.cpp, src/app/settings_operations.{h,cpp}, src/providers/api_key_store.{h,cpp}, src/providers/credentials.h, src/workspace/workspace_config_store.{h,cpp}, tests/app/unit_r2_database_transfer.cpp. Leave changes uncommitted.
- Block 1 prerequisite check and any integration fixes: Verified block1 foundation (ensure_sync_schema, sync_queue, D1Client, note/config producers/apply, sync_machine, apply_sync_config_edit, record/apply hooks). Preserved block1 interfaces; extended them without renames. Removed now-unused finish_turn helper after inlining sync-aware turn hooks. Added WorkspaceConfigStore::sync_machine getter, SessionJournal::set_sync_machine, SessionController sync plumbing (machine at construction for repair), SessionRepository::set_sync_machine.
- Chat apply, Worker, credential operation, and notification entry points: Outgoing via SessionJournal (complete/cancel/fail with prompt lookup, record_entry for @- only, add_note/file/folder, detach, character, rename, clear, delete_turn with pre-delete position) plus SessionRepository rename/delete for closed chats; shared rule via label/next_request_id/no running req-1; initial publication (turns, @-, attachments with R55 skip, character) on transition, exclusive branches. Incoming via sync_apply_chat_record (forum skip, create missing with carried identity/label and receiver-local numbers, positional delete, path resolution with skip, character upsert/delete) in its own transaction with no echo. Worker in src/app/d1_sync.{h,cpp} (collect flags, send in rowid order with 1.9MB UTF-8 pieces, no-recipient delete, missing own table halts with recovery error, full-DB eviction at most one excluding own machine, receive 50 halving to 1 with cross-group assembly and broken-set skip, contiguous ack, reservation-failure pause, invalid skip). Reservation via LiveSessionManager::reserve_for_sync (reloading reason) per chat and global for config; config apply via apply_sync_config_edit with held global and retry handling. Notes-changed via on_notes_changed (bump epoch + dispatch context_changed). Credentials: ApiKeyStore d1/d1_info/save_d1/remove_d1, settings get/save/delete with token presence only, Application get/save/disconnect (Save requires machine, creates table first then saves and restarts Worker, omitted token retains, failure leaves unchanged; Disconnect stops Worker, clears queue/flags then removes creds, drops remote with old creds best-effort, offline succeeds; D1 edits create no records).
- Vault/R2/daemon and native shutdown behavior delivered: First loop synchronously at vault open before screens (warn and continue on failure); daemon auto-creates missing own table (chaweb_host) retaining work, desktop halts until Save/R2; 30-sec periodic thread; close/switch/maintenance stops receiving, lets controllers finish, flushes send-only, joins before DB release; final D1 send unbounded (per-request timeouts only). R2 upload refuses while pending with approved message after drain flush; both downloads validate, ensure schema, clear queue/flags in one transaction, checkpoint TRUNCATE (busy rejects, aborts install, destination untouched), remove sidecars, publish, preserve D1 creds; replacement creates own queue if connected; new-download protected refusal unchanged; desktop copy cleans destination retaining source. Native: no host changes required — existing bridge/host path waits for join call without timeout; backend final send unbounded, bounded joins still use grace. macOS/Windows host paths not exercised; carry as block3 limit.
- Validation commands, platforms, and outcomes: cmake --preset ninja; cmake --build --preset ninja (clean); ctest --test-dir build/ninja -j8 --exclude-regex "itest|live" → 1753/1753 pass (2 live skipped) on macOS arm64. Focused: ctest -R "ChatSyncTest|D1WorkerTest|D1CredentialsTest|D1AppWiringTest|D1R2Test|D1VaultTest|D1ShutdownTest|Sync|D1Client|R2DatabaseTransfer" all pass. Covers first publication (title/response order, all terminal states, repair, @-, attachments/overrides, closed edits), clear/delete eligibility across restart, missing creation/local numbering/positional delete/R55 skip/reservation pause/deferred/no-echo, send retry/no-recipients/missing-halt/split-across-pages/partial-retry/oversized-halving/invalid-skip/ack-failure/evict-one, offline open/switch/failed-recovery/close ordering, R2 refusal/cleanup (fresh-handle empty queue/zero flags, WAL, busy rejection, protected paths, copy), credentials (save failure/unchanged/omitted/offline), slow 11-sec flush finishes (native-allowed). R2 upload full Application path (start_database_upload with pending) hangs in harness; verified via pending check + approved-message code path, full path deferred. No live D1 dependency; loopback/scripted transport only.
- Frontend work remaining from the reservation/refresh probes: Block3 owns bridge/DTO/screens: D1 Settings screen (get/save/disconnect via new Application ops, token-presence DTO, Save recovery, Worker halt error surfacing), notes.changed bridge event and screen reload (backend calls on_notes_changed → bump epoch + context_changed), ChaWeb 30-sec idle / 1-sec reply cadence, chat reopen-after-reload UX for reloading reservations. Backend APIs: D1SyncWorker::run_once/flush_send/start/stop_and_flush, Application::get/save/disconnect_d1_storage + set_d1_transport_for_tests, LiveSessionManager::reserve_for_sync, sync_chat.h (is_shared/identity/enqueue/initial/parse/apply), sync_make/split/piece_bytes, note_apply_record, apply_sync_config_edit outcome (applied/retry/notes_changed).
- Decisions, deviations, or unresolved blockers: One machine at a time, one Worker/30-sec, no registry/delivery IDs/receipts/cursors/buffers/schedulers/shared flags; received changes never enqueue/flag (scoped); reserve/apply/release per chat; restarts retain work, new deliveries clear queue/flags in creation/download; close waits for full final send (per-request timeouts, stop at first failure, no overall drain). No second shutdown scheduler or larger timeout; final send unbounded. No new UI contracts. Blocker: R2 upload full-path test hangs in harness (verified via pending check + code path); carry full-path exercise as block3 limit alongside native platforms. No other blockers.
- Block 3 ready to start: Yes.

COMPLETED

