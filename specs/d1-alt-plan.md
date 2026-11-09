# D1 queue synchronization implementation plan

Date: November 9, 2026

Status: implementation plan. No implementation or live checks have been done
as part of this plan.

Source: [D1 queue synchronization: alternative design](d1-alt-design.md).
Requirement numbers and section numbers below refer to that document.

## 1. Scope and completion criteria

Implement the approved design for macOS, Windows, and `cha-daemon` with
ChaWeb. Each application keeps its local vault database. A Worker sends local
change records to the other machines' D1 tables and applies its own incoming
queue. The normal use case remains one person working on one machine at a
time.

Use the current design decisions throughout:

- Configuration import has its own record-writing hook. Both configuration
  commit paths use a small helper to produce one changed-row record.
- Configuration import and vault merge preserve the destination's D1
  credentials, including their absence (R56). Full vault copies and R2
  downloads still carry source credentials.
- Every newly delivered vault has its local send queue and note `changed`
  flags cleared before use (R57). This applies to every delivery route,
  including replacement of an existing vault. Ordinary restarts keep
  unsent local work.
- Incoming configuration edits prune sessions for all removed forums. The
  record contains only `set` and `delete`; it has no deleted-forum field.
- Chat eligibility uses the label and existing `next_request_id`/turn data.
  Clear and turn deletion do not require a new shared-state column.
- A chat note copy with a deleted source is omitted, as specified by R55.
- Incoming chat writes reserve and stop the live chat before changing its
  database rows. Reservation failure leaves the record for a later loop.
- The send step checks for this machine's queue before sending anything.

Completion means that all record kinds in design section 7.2 work through
creation, transport, application, and screen refresh; the vault/R2 lifecycle
works; and the checks in section 12 of this plan pass. The accepted risks in
design section 13 remain accepted behavior.

The mockup copy and error texts recorded in design sections 5.4 and 16 are
approved. Implement those without another design-approval step. Add no UI
text just for this plan. If implementation needs additional text or screen
states, including loading/busy text, obtain visual approval under
[AGENTS.md](../AGENTS.md) before adding them. Keep the approved UI.
Do not add a status panel, legends, explanatory copy, or other controls.
Leave `docs/` unchanged.

## 2. Code organization

Use the existing SQLite, curl, JSON, credentials, runtime reservation, and
bridge code. Keep storage work in storage/workspace code and application
lifetime work in the application layer.

Three small new C++ units are sufficient starting points. Their names are
proposed implementation locations, not existing files:

| Proposed unit | Responsibility |
|---|---|
| `src/storage/sync_storage.h/.cpp` | Local queue schema and operations, shared record helpers, and the common configuration-delta helper. Domain operations retain their own record construction and apply logic where they already own the data. |
| `src/app/d1_client.h/.cpp` | D1 REST requests, response parsing, table discovery and quoting, queue reads/writes/deletes, and full-database cleanup requests. |
| `src/app/d1_sync.h/.cpp` | One Worker, one thread, the loop, piece splitting/assembly, and calls into the existing application and storage owners. |

Keep the record format from design section 7.2 in one place. Ordinary JSON
builders/parsers and a direct dispatch by `kind` are enough. Use the existing
domain types when possible. Do not build a generic replication framework.

The relevant existing code is:

| Area | Main files |
|---|---|
| Machine and vault configuration | [application_config.h](../src/app/application_config.h), [application_config.cpp](../src/app/application_config.cpp) |
| Stored credentials | [credentials.h](../src/providers/credentials.h), [api_key_store.cpp](../src/providers/api_key_store.cpp), [workspace.cpp](../src/workspace/workspace.cpp), [workspace_config_editor.cpp](../src/workspace/workspace_config_editor.cpp) |
| Configuration commits and note access | [workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp) |
| Database schema and note operations | [workspace_session_database.cpp](../src/storage/workspace_session_database.cpp), [note_storage.cpp](../src/storage/note_storage.cpp), [folder_store_storage.cpp](../src/storage/folder_store_storage.cpp) |
| Chat persistence and lifecycle | [session_database.cpp](../src/storage/session_database.cpp), [session_repository.cpp](../src/storage/session_repository.cpp), [session_controller.cpp](../src/session/session_controller.cpp), [live_session_manager.cpp](../src/runtime/live_session_manager.cpp) |
| Application and maintenance | [application_internal.h](../src/app/application_internal.h), [application.cpp](../src/app/application.cpp), [vault_operations.cpp](../src/app/vault_operations.cpp), [notes_service.cpp](../src/notes/notes_service.cpp) |
| Settings and native API | [settings_operations.cpp](../src/app/settings_operations.cpp), [bridge_protocol.cpp](../src/bridge/bridge_protocol.cpp), [bridge_router.cpp](../src/bridge/bridge_router.cpp), [dto.yaml](../resources/dto.yaml) |
| Desktop screens and refresh | [R2StorageSettings.tsx](../webapp/src/components/R2StorageSettings.tsx), [Settings.tsx](../webapp/src/components/Settings.tsx), [route.ts](../webapp/src/state/route.ts), [NotesScreen.tsx](../webapp/src/components/NotesScreen.tsx), [useLiveSession.ts](../webapp/src/useLiveSession.ts) |
| Linux startup and chat refresh | [main.cpp](../src/daemon/main.cpp), [ChaWeb client](../webapp/src/chaweb/client.ts), [useChaweb.ts](../webapp/src/chaweb/useChaweb.ts) |

The new persistent sync data is limited to the credentials in existing
configuration storage, `sync_queue`, and `note_items.changed`. Do not add
delivery IDs, cleanup flags, recipient state, a registry, persistent receive
cursors, a second outgoing buffer, or a shared-chat flag. Keep piece assembly
and its read position in memory for the current receive pass only.

## 3. Implementation order

Each phase includes its focused checks. Complete those checks before moving
to the dependent work; reserve the complete regression run for the final
phase.

| Phase | Work | Depends on | Main requirements |
|---|---|---|---|
| 0 | Resolve the design's feasibility checks | — | R23, R35, R46, R53 |
| 1 | Machine name, credentials, schema, record helpers | 0 | R22, R34, R36, R38, R44, R47, R54, R56 |
| 2 | D1 REST client | 1 | R3, R10, R11, R23, R36, R40, R46 |
| 3 | Note record creation and application | 1 | R13, R15, R16, R18, R32, R42, R47, R49 |
| 4 | Configuration record creation and application | 1 | R12, R14, R18, R31, R38, R48, R50, R56 |
| 5 | Chat record creation, reservation, and application | 1, 3, 4 | R18, R27–R29, R35, R48, R52, R54, R55 |
| 6 | Worker, application lifecycle, R2, and daemon | 2–5 | R1–R8, R17, R19–R24, R26, R37, R40, R41, R46, R51, R53, R57 |
| 7 | Approved Settings screen and screen refresh | 6 | R35, R39, R43–R45 |
| 8 | End-to-end and platform validation | 0–7 | Complete design |

## 4. Phase 0: verify the design's external assumptions

Do the checks in design section 15 before implementing the dependent
behavior. Use a disposable D1 database and temporary vaults for destructive
or size-limit checks. Record the observed request shape, result, and relevant
limits without credential values or user content.

1. Send a REST `batch` with a bound 1,900,000-byte piece copied into multiple
   recipient inserts. Test representative UTF-8/JSON content, not only ASCII.
2. Prefer a disposable database on an available Workers Free account for
   this test: its database limit is 500 MB, while Workers Paid permits 10 GB
   ([D1 limits](https://developers.cloudflare.com/d1/platform/limits/)). Check
   the account plan before filling it. If no Free test account is available,
   select the test resource before running a full-capacity probe; do not
   assume a paid database will stop at 500 MB. Fill until the actual size-limit
   error, rather than assuming a fixed row count. Confirm the exact error,
   then confirm that `DROP TABLE` works and permits another write. This test
   must not use a user's vault queues.
3. Send a batch with a deliberately failing statement and inspect its tables
   to record whether the request committed partially. The implementation must
   still tolerate uncertain delivery and duplicate retries as designed.
4. Read an empty queue and inspect the reported row-read count.
5. Exercise the chat reservation with shutdown reason `reloading` separately
   on desktop (`useLiveSession.ts`) and ChaWeb (`useChaweb.ts`). Hold it while
   each screen attempts to read/reopen, then release it. Cover an idle open
   chat and a running reply, and confirm that each screen eventually shows
   incoming changes without manual navigation. ChaWeb currently schedules
   further reads only while generation is active; desktop reconnect behavior
   does not prove that an idle ChaWeb chat refreshes. Use a small test or
   temporary prototype to select the necessary changes to the existing
   ChaWeb read/recovery loop before implementing the Worker. Keep one read
   loop per client; do not add a second polling subsystem.

**Exit condition:** the transport-size and full-database assumptions are
supported by actual results, and both clients' refresh/reconnect paths are
understood. If a required D1 operation fails, record the constraint and resolve
that specific design point before building code that depends on it. Do not
silently skip oversized records or add a different remote service.

## 5. Phase 1: configuration and local storage

### Machine name and credentials

- Add the optional `machine` setting to configuration loading and the
  application command/context. Ignore an invalid value, including a
  non-string, an empty string, or a name containing `/`, and log a warning.
  Treat it as no machine name: the application still starts, its vaults are
  not connected, and Save uses the approved missing-machine-name error.
  Use ASCII case-insensitive comparison for valid machine/table identity.
- Extend the existing R2-style credential path with a D1 credential type:
  account ID, database ID, and API token. Keep it in the vault's existing
  database-backed configuration storage. Full vault copies, protected vaults,
  and R2 transfer carry it naturally. Export may include the credential rows,
  but configuration import and merge must preserve the destination's D1 rows,
  including preserving their absence (R56).
- Follow the existing secret-detail pattern: expose token presence in
  Settings details, and preserve the stored token when an update omits it.
  Do not log tokens or complete record bodies.
- Carry the machine setting through the existing vault/store/journal
  construction paths. Determine whether to record from the current vault's
  credentials and machine name. A Worker stopped because its table is missing
  does not make the vault disconnected.
- Keep a vault with no D1 setup usable. Missing or unused D1 settings must not
  prevent unrelated operations that can succeed.

### Schema and record operations

- Create `sync_queue(record TEXT NOT NULL) STRICT` and add
  `note_items.changed INTEGER NOT NULL DEFAULT 0 CHECK (changed IN (0, 1))`.
  Use the existing schema-ensure pattern so opening an older schema-v2 vault
  preserves its data and initializes flags to zero.
- Ensure both new-vault creation and existing-vault opening install the
  schema before any producer runs, including daemon and protected-vault
  paths. Keep these additions compatible with existing database validation.
- Provide small operations to append a whole record within a caller's
  transaction, read outgoing records in `rowid` order, delete a sent record,
  and clear the queue/flags together on disconnect or new vault delivery.
- Establish record serialization for every kind in design section 7.2.
  Notes use path arrays; chats use forum/session IDs; configuration uses row
  names. Preserve configuration IDs and entry content/metadata.
- Keep incoming application below outgoing hooks. Where a shared function
  needs suppression, use an explicit argument scoped to that call. Do not
  use a process-wide “receiving” flag that could suppress an unrelated save.

**Checks:** migrate an existing populated vault and reopen it twice; exercise
the same path with a protected vault; prove transaction rollback also removes
an appended record. Check disconnected operations create neither records nor
flags. Test missing and invalid machine values without preventing startup or
unrelated operations. Round-trip credential storage and the actual record
payloads.

## 6. Phase 2: D1 REST client

- Use curl and the design's connection/low-speed timeouts. Reuse the project's
  transport conventions without changing the R2 signing or transfer code.
- Implement the query endpoint and bound parameters. Check the API response
  and statement results before reporting a request as successful; keep
  transport errors and SQL/API errors available to the Worker.
- Implement create, list, read, insert batch, handled-prefix delete, row
  counts, and table drop. Quote each `"` in an SQL identifier as `""`.
- Discover queues from `sqlite_master`. Match `q/<vault>/<machine>` using the
  three components from section 6.1, ASCII case-insensitive comparison, and
  no `LIKE` matching. Existing vault validation already forbids `/` in a
  vault name; keep that validation.
- Supply one insert per recipient in a REST batch. One piece is one request;
  use bound record text, not record text embedded in SQL.
- Distinguish the full-database error from rate limits, daily quotas, missing
  tables, authentication errors, and the result-size error. Only the specified
  full-database error permits eviction.
- Keep network work outside SQLite transactions and application/storage
  locks. Use the existing cancellation convention for lifecycle shutdown.

**Checks:** use [MockHttpServer](../tests/support/mock_http_server.h) for
request inspection, malformed responses, failure results, timeouts, and
retries. Cover valid vault names containing `%` or `_`, quoted machine names,
and differing ASCII letter case. Verify that the local table and other
vaults are not recipients.
Use the phase-0 live results to cover real D1 behavior that mocks cannot prove.

## 7. Phase 3: notes

### Outgoing notes

- Add immediate records to create, rename, delete, read-only, and compression
  commits. Capture the old path before rename/delete changes it.
- Cover both compression commit paths and all existing content-load paths.
  Text saves and loads into existing notes set `changed = 1` in their current
  transaction. Scroll-only saves do not set it.
- Add one `note.create` per imported item in the book/folder import
  transaction, with parents before children and all defined content fields.
- New summary notes carry `summary_of`. Replacing an existing summary sets
  its flag, including the updated read-only, scroll, and compressed-text
  values in the eventual update.
- For each flagged note, read its current path and fields, enqueue
  `note.update`, and clear the flag in one transaction. A later save must
  remain observable through a new flag.
- Exclude ANNOTATIONS and managed forum-folder changes from outgoing hooks.

### Incoming notes

- Resolve paths locally. Apply the missing-parent and name-suffix rules from
  section 10.2, including creation when `note.update` finds no note.
- Apply updates directly in one transaction, including to read-only notes.
  Preserve the 12 MiB note limit; D1 piece size must not become a save limit.
- Remove note audio and its pending audio attempt in the incoming update's
  transaction, as the existing summary-replacement path does. The existing
  `note_audio_finish` already refuses results for a removed attempt; reuse
  that behavior and the existing abandoned-audio interruption path.
- Apply summary links when the referenced chat exists. Keep the specified
  unlinked result when it does not.
- Rebuild the containing folder's ANNOTATIONS through the existing function
  using local IDs. Incoming writes and derived-note rebuilding must not
  create outgoing records or flags.
- Report note changes to the caller for the single event sent after a receive
  group, including note changes caused by configuration application.

**Checks:** imports with parent/child ordering; text-save coalescing; a save
after flag collection; edit followed by read-only or rename; scroll-only
changes; summary create/replace; missing paths and collisions; unconditional
audio removal; annotation links; no echo; large content round trips.

## 8. Phase 4: configuration

- Add the common old-row/new-row delta helper at `commit_and_publish` for
  Settings and merge, and inside `commit_imported_rows` for import. Import
  must read the old rows before replacing them.
- Before validating an import/merge candidate, exclude the source D1
  credential rows and retain the destination's D1 rows unchanged. If the
  destination has none, keep it that way. Log a warning when source D1 rows
  are ignored. Reuse merge's existing maximum-key-counter rule; on import,
  keep the counter above the IDs of any retained destination keys.
  Apply the normal validation and delta calculation to this final candidate.
  These operations do not clear the local queue/flags or change the D1
  connection; only their ordinary configuration changes add records.
  Preserve the existing behavior for ordinary API keys and R2 credentials.
- Emit one `config.edit` in the operation's transaction. Its `set` and
  `delete` lists contain only changed rows. Identify D1 credential rows in
  both row sets and exclude their additions, changes, and deletions. A
  D1-only change produces no record. Ordinary API and R2 keys remain included.
- On receipt, overlay both lists on the current configuration, validate the
  complete candidate once, and commit/publish it once with recording disabled.
- In that same commit transaction, run the existing orphan-session pruning
  against the candidate forums. Retain managed-folder synchronization. Do
  not reintroduce a deleted-forum field or emit extra folder records.
- Use the existing application maintenance/session-release behavior for
  configuration changes that remove or reload live forums. Arrange this
  through an internal apply path that does not ask the receiving Worker to
  stop or join itself.
- Publish the configuration and its normal session/presentation effects only
  after a successful commit. An invalid incoming candidate is warned about
  and skipped as one record.

**Checks:** create/delete a multi-row character or forum; change a referenced
provider with its dependent rows in one action; import and merge; import
removing two forums with chats; recreate a removed forum; transaction failure;
invalid candidate; API/R2 key inclusion; D1 credential exclusion; no echo.
For both import and merge, cover a destination with and without D1 credentials
against a source with no credentials, matching credentials, and different
credentials. Include pending records and flags: the destination credentials
and existing pending work must survive, and a disconnected destination must
remain disconnected. Verify ignored-row warnings and valid key allocation
after preserving a destination D1 key with a higher ID than the source keys.

## 9. Phase 5: chats and runtime reservations

### Outgoing chats

- Centralize the eligibility predicate from section 8.3: a non-temporary
  label, `next_request_id > 1`, and no running turn with request ID 1.
- Check before and after each operation. On the first transition, emit the
  initial records once. Otherwise, emit that operation's record when the
  post-operation chat is shared. Chat deletion uses its pre-delete state.
  Treat initial publication and ordinary publication as exclusive branches,
  so the triggering turn is not appended twice.
- Produce the initial finished turns, self-notes, attachments, and character
  settings from the existing stored data in the specified order. Capture the
  initial records in the operation's transaction.
- Put terminal-turn hooks in `SessionJournal::complete_turn`, `cancel_turn`,
  and `fail_turn`, within their storage transaction/common finish path. This
  also covers interrupted-turn repair through `journal_.fail_turn` when a
  chat reopens. A repaired first turn must run the same eligibility check and
  initial publication as an ordinary finished first turn. Support the
  existing cancelled-turn form with no response entry. Never send an
  unfinished prompt on its own.
- Cover `@-`, rename, delete, clear, turn delete, note/file/folder attachment,
  folder detach, and character overrides, including removal of an override.
  Cover journal operations and repository operations on unopened chats.
- Calculate the turn's one-based position before deleting it. Do not send
  its local response-entry number.
- Skip copied notes whose source no longer exists (R55). Exclude Welcome and
  standalone notices/errors as specified. Preserve the accepted temporary
  label behavior rather than adding lifecycle state to change it.

### Incoming chats

- Extend or reuse the existing session maintenance reservation so the Worker
  can reserve with reason `reloading`, stop the controller, and wait for its
  release. Apply one record in its transaction, then release the reservation.
  Process the next record in queue order.
- Preserve normal cancellation persistence for a running reply before the
  incoming mutation. Do not write with stale controller counters.
- Distinguish a reservation failure from an invalid record. Reservation
  failure pauses receipt and leaves this record and following rows in D1.
  A record that cannot be applied after reservation is warned about and
  handled according to R41.
- Create a missing chat with the carried forum/session identity and label.
  Use the existing zero defaults for `recent_pending` and `discardable`.
  Skip records for missing forums. Allocate receiver-local entry and request
  numbers and update the existing next-number counters. Preserve the carried
  terminal state and entry metadata.
- Resolve turn positions and attachment paths locally, and apply each
  operation in its own transaction with outgoing recording disabled.
- Release the reservation and use the existing reconnect path. Complete the
  small reconnect adjustment from phase 0 if necessary, including a reconnect
  attempt made while the reservation is still held.

**Checks:** title before/after first response; all terminal states, including
interrupted-first-turn repair; initial publication exactly once; an incoming
record creating a missing chat; self-note-only chats; clear and last-turn
deletion followed by restart and further edits; same-label chats with different IDs;
deleted-source omission; positional turn deletion with local notices present;
incoming writes during generation; reservation failure and reconnect; no echo.

## 10. Phase 6: Worker and vault lifetime

### Loop and transport progress

- Own one Worker from `Application::Impl`. Give it one thread and the fixed
  30-second loop. Keep a single serialized loop implementation usable for
  startup, periodic work, send-only flushes, and deterministic tests.
- Collect flagged notes first. If the outgoing queue is nonempty, list the
  vault's tables and check that this machine's table exists before sending.
  Keep queued records if that check fails.
- Send records in local `rowid` order. Split serialized UTF-8 at character
  boundaries into pieces of at most 1,900,000 bytes. Delete the local record
  only after all its piece requests succeed, or when there are no recipients.
- On ordinary send failure, warn, stop sending for this loop, and retry from
  the first piece in the next loop. Add no retry count or backoff policy.
- On the full-database error, count eligible queue rows across the D1
  database, exclude this machine's tables, and drop at most one table with
  the largest count. Stop sending until the next loop. If discovery/drop
  fails or no eligible table exists, retain the record and log the failure.
- Receive in `rowid` order, initially requesting 50 rows. On the specified
  result-size error, halve the request size down to one; reset to 50 in the
  next loop.
- Keep a temporary read position and the contiguous handled prefix separate
  while assembling a split record across groups. A record's rows become
  handled only when the complete record is applied or deliberately skipped.
- At an incomplete final set, leave its rows and end the receive pass. At a
  broken sequence followed by another record, warn and skip the incomplete
  set, then handle the new row normally. Retain no assembly state between
  loops.
- Delete only the handled prefix with one request per handled group. Do not
  acknowledge the record whose reservation failed or an unfinished piece set.
  Stop the receive pass on a transport/read/delete failure; accepted replay
  behavior handles an uncertain acknowledgement on the next loop.
- Continue until no more rows can be handled. A missing local table stops
  the Worker with the design's recovery error.

### Application and maintenance ordering

- At vault open, initialize storage and credentials and run the first loop
  before exposing the screens. If D1 is unreachable, log and continue opening.
- Integrate stop/flush/restart with the existing lifecycle and context epoch.
  Stop receiving before vault teardown, close user admission, and let current
  controllers/jobs finish or cancel while their outgoing hooks still work.
  Then collect flags and make the final send attempt before releasing the
  outgoing vault's database and password.
- Apply that final send to application close, vault close, and vault switch.
  Stop and join the Worker before a database is closed, replaced, or
  retargeted. Restart it for the selected vault after maintenance succeeds or
  the previous vault is restored after a failure.
- At application close, let the existing native shutdown thread wait for
  the full D1 send pass (R37). Both native hosts currently impose a 10-second
  deadline; adjust their existing shutdown/join path so that it does not
  cut this pass short. Keep the request timeouts and stop sending at the
  first failure, retaining unsent records. Add no overall D1 drain timer or
  separate shutdown scheduler.
- Avoid lock cycles: do not join a Worker while holding a lock it needs, or
  wait for a runtime reservation while holding a SQLite write transaction.
  Keep network waits outside lifecycle/store locks. Deliver notifications
  through the existing epoch-aware callbacks after releasing those locks.
- Serialize disconnect and credential replacement with Worker activity so
  an old request cannot resume work against a replacement vault or cleared
  queue. Use the existing lifecycle controls; add no parallel scheduler.

### R2 and daemon integration

- In the existing R2 upload preparation/maintenance window, run flag
  collection and the send step once. Check for remaining records/flags before
  producing the upload snapshot, while local writers are still excluded.
  Use the approved refusal text if work remains. Preserve existing R2 ETag
  and overwrite behavior.
- Stop the Worker before an R2 download replaces the database. In both
  `download_database_from_r2` and `download_new_database_from_r2`, validate
  the temporary database, ensure the sync schema, and clear its `sync_queue`
  and note flags in one transaction before `publish_downloads` installs it
  (R26, R57). Complete and close SQLite writes before installation. Cleanup
  failure uses the existing download failure path. Keep the downloaded
  content and D1 credentials (R56), and preserve incoming records in D1.
- After replacement, create this machine's D1 queue when connected, then run
  the normal vault-open loop. The replacement path already has the protected
  vault's password. New-vault download currently rejects protected databases
  without a password; preserve that behavior. Cleanup needs no saved flag,
  first-unlock hook, or restart recovery process.
- In the existing desktop `create_vault(copy_from)` path, clear the new
  destination's queue and note flags before returning it. Keep the source's
  pending work. Do this in vault creation, not in the shared low-level
  database-copy helper, which also serves database maintenance.
- Keep the explicit Save-credentials recovery path for a copied vault, an
  unopened downloaded vault, or a changed machine name.
- On `cha-daemon` startup only, create a missing table before starting its
  Worker. Use the existing daemon host identity, not the operating system,
  for this exception. A table lost during execution still stops the Worker
  until the next start. Creating that table must not clear this machine's
  pending work. ChaWeb gets no Settings or R2 transfer UI, and this feature
  adds no Linux vault-copy or manual-installation workflow.

**Checks:** scripted send/retry/receive failures, split sets across pages and
loops, partial writes, result-size fallback, reservation deferral, missing
table before send, eviction limited to one eligible table, no-recipient send,
startup offline, final flush after reply cancellation, vault switch, failed
maintenance recovery, protected download, upload blocking, and daemon restart.
For both download paths, use a fixture with queued records and note flags;
verify that cleanup completes before installation and the content and D1
credentials stay intact. Cover replacement of an existing protected vault
and the current refusal of a new protected download without a password.
For desktop creation from another local vault, verify that the destination
is clean and the source's pending work remains. Verify that normal restart
and missing-table recovery preserve the daemon's own unsent work.

## 11. Phase 7: API and approved screens

- Add D1 credential get/save/disconnect methods through the existing settings
  application layer, native method enum/name/policy tables, DTO schema, and
  TypeScript clients/guards. Keep wire fixtures consistent.
- Add `D1SyncSettings.tsx` next to the R2 screen, the API Keys row, and the
  corresponding desktop route. Use the approved fields, buttons, confirmation,
  placeholders, row description, and error texts from sections 5.4 and 16 of
  the design. Follow the UI scope rule in section 1.
- Keep Save usable with unchanged valid credentials: it is also the explicit
  way to recreate a missing queue. Do not copy R2's dirty-only Save condition.
- Save creates the target queue first, saves locally only after D1 succeeds,
  then starts/restarts the Worker. Missing machine name uses the approved
  error. A failed request leaves the stored credentials unchanged.
- Disconnect first stops Worker access, then removes local credentials,
  empties the queue, and clears flags together. Make one remote drop attempt
  using the captured old credentials; a remote failure only logs a warning.
- Add the scoped `notes.changed` event through application, bridge, and
  frontend subscription handling. Refresh the desktop tree and open note
  once per affected group. Reuse the `noteRequest` reload in `NotesScreen`:
  it replaces the editor, and `useNoteSave` already clears pending save state
  when the editor changes. Keep the save algorithm unchanged (R32); add no
  synchronization-specific save protocol. Incoming content may replace
  unsaved edits as approved.
- Implement the desktop and ChaWeb refresh/reconnect adjustments selected in
  Phase 0 through each client's existing path. Verify idle and generating
  chats on both. ChaWeb notes continue to refresh when the note page opens
  again.

**Checks:** credential load/save failure, missing name, Save without edits,
offline disconnect, confirmation cancel, reset, retained secret, route/client
contracts, old-epoch event rejection, open-note refresh with a pending edit,
and chat reconnection after a reservation.

Regenerate the DTO declarations with:

```sh
npm --prefix webapp run api-types
```

## 12. Phase 8: validation and delivery

### Focused tests and registration

Register new source and test files in [CMakeLists.txt](../CMakeLists.txt).
Keep shared sync tests in the existing backend/application targets so they
run in the daemon test subset too. Extend the existing note, session,
workspace, runtime, bridge, and frontend tests rather than creating duplicate
test frameworks. Use the loopback HTTP server and temporary databases for
routine failure tests; keep live D1 tests opt-in and outside `itest-local`.

Tests should check durable data, outgoing requests, and visible behavior.
They should not prescribe private helper names or create contracts for
accepted corner cases that the design does not promise to repair.

### End-to-end acceptance

Use two temporary copies of one vault with different machine names. Include
a protected-vault run. Confirm these user-visible flows:

1. Connect, edit notes/configuration/chats on A, close or switch its vault,
   then open B. B shows the changes after its first loop.
2. Import notes and a configuration that creates dependent rows and removes
   multiple forums. Check content, managed folders, and removed chats on B.
   Import and merge sources with different D1 settings and verify that the
   destination keeps its credentials or their absence, with pending work
   still available for its normal send path.
3. Send a record larger than one piece. Also exercise a record spanning more
   than one receive group, an interrupted set, and a retry from piece 1.
4. Disable D1 access, make local changes, and verify that they persist and
   block R2 upload. Restore access and verify that delivery clears the block.
5. Receive a change to an open note and an open chat. Confirm the approved
   text replacement/cancellation behavior and successful reconnection.
6. Clear a shared chat, restart, and make another change. Check that it still
   synchronizes. Verify the R55 omission without attempting to restore the
   deleted source or convert its attachment.
7. Remove a machine's queue, then open its vault with pending local records.
   Verify that it stops before sending. Exercise desktop explicit recovery
   and the daemon startup exception separately.
8. Perform R2 bootstrap/download, vault switch, and offline disconnect. Check
   that no Worker retains access to an old database, queue, or password.
9. Close the application with a large local queue, including a recent book
   import and split records, over a slow connection and with D1 unreachable.
   Verify that each native host waits for the complete send pass, including
   a pass longer than its former 10-second shutdown limit. On failure, verify
   that sending stops and unsent work remains on reopen. The close duration
   follows the send attempt; there is no additional drain timeout (R37).

Confirm that audio and event history do not appear in records, API/R2 keys do,
and D1 credential edits do not produce records. Accepted duplicates must not
break the loop, but exactly-once behavior is not an acceptance condition.

### Build and regression commands

After focused checks pass, run the repository's normal checks on macOS:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make web-check
make itest-local
```

On Linux, build and run the shared backend/daemon checks with
`make test-daemon` and the daemon integration flow with `make itest-daemon`.
On Windows, use the existing Windows build script and run the available CTest
and frontend checks. Exercise the Settings and reconnect flows in both native
hosts. Record any platform that could not be validated.

Run the isolated live D1 acceptance checks separately. Do not run the broad
live-provider/R2 suite just to test D1. Repeat or broaden tests only when a
failure, new change, or unresolved concern justifies it.

### Delivery check

- Every design record kind has an outgoing path and an incoming path.
- The two configuration commit paths and all note/chat mutation paths are
  covered, including repository operations on closed chats.
- Schema migration, transaction boundaries, echo suppression, reservation
  deferral, and Worker shutdown have passed their focused checks.
- The approved UI and all three application platforms are accounted for.
- Report the phase-0 live results, validation performed, remaining platform
  limits, and accepted design risks. Remove temporary probe artifacts and
  unnecessary abstractions before completion. Leave `docs/` unchanged.
