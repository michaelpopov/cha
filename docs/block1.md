# Block 1 — D1 foundation, transport, notes, and configuration

Status: Not started. Implementation instructions for the first of three
sequential agent sessions. No implementation or live checks are claimed by
this document.

## Suggested session prompt

```text
Implement docs/block1.md using the current code, specs/d1-alt-design.md,
and specs/d1-alt-plan.md. Follow AGENTS.md. Complete its validation and
handoff. You may update the Execution record section of docs/block1.md
with results and instructions for the next session. Leave other docs/
files unchanged. Ask if an actual conflict needs a design decision.
```

## 1. Objective and sequence

Build the storage and transport foundation, then complete outgoing and incoming
note/configuration records. Leave working, tested backend operations for the
chat and Worker integration in block 2. This covers phases 0–4 of
[the implementation plan](../specs/d1-alt-plan.md).

The complete sequence is:

| Session | Scope | Planned context use |
|---|---|---|
| [Block 1](block1.md) | Feasibility, credentials/schema, D1 client, notes, configuration | 300K–400K tokens |
| [Block 2](block2.md) | Chats, Worker, vault/R2/daemon lifetime, shutdown, backend credential operations | 300K–400K tokens |
| [Block 3](block3.md) | Bridge and approved UI, screen refresh, full feature validation | 250K–375K tokens |

These are planning estimates for source reads, implementation, tool output,
tests, and fixes together, not measured model token counts or guarantees.
They assume targeted reads of large source/test files. Each block leaves at
least about 100K tokens below the 500K context limit. Two blocks would combine
too much domain, runtime, and UI work with its testing; a fourth would add a
handoff without a necessary dependency boundary.

Read relevant functions and tests rather than dumping entire directories.
Keep test output concise. The next session needs the resulting code and the
execution record below, not a transcript of this session.

## 2. Prerequisites and authority

Before editing:

1. Read [AGENTS.md](../AGENTS.md), [CLAUDE.md](../CLAUDE.md), the complete
   [technical design](../specs/d1-alt-design.md), and plan sections 1–8.
   Design requirement identifiers below refer to that document.
2. Inspect the current working tree and preserve unrelated changes. Check the
   actual code before introducing a new path or helper. There is no preceding
   D1 block implementation to assume.
3. Use the existing CMake/Ninja and frontend test setup. Discover test names
   from the repository; names in this guide are entry points, not commands
   that imply a nonexistent target.
4. Identify a disposable D1 database for the live feasibility checks. Use
   available task-authorized credentials without printing them. If the test
   resource or access is missing, ask for that missing information. Never
   substitute a database containing real vault queues.

The completed design is approved. Do not request design approval again.
The design defines behavior; the plan supplies implementation detail; this
guide assigns work to sessions. Ask the user if an actual code constraint
requires changing an approved behavior. Do not expand the feature to resolve
an imagined operation or corner case.

The suggested session prompt explicitly includes permission to update this
file's execution record for the next session. Keep that handoff update narrow;
do not revise other documentation incidentally.

## 3. Decisions that must remain intact

- CHA is a personal application used by one person on one machine at a time.
  Accepted duplicate delivery, partial delivery, ID collisions, and the
  registration gap remain accepted; do not add reconciliation or exactly-once
  delivery machinery.
- The only new persistent synchronization data is D1 credentials in existing
  configuration storage, `sync_queue`, and `note_items.changed`. There are no
  delivery IDs, registry, recipient state, persistent receive cursor, cleanup
  flag, second outgoing buffer, or shared-chat flag.
- Incoming application creates no outgoing records and sets no note flags.
  Use direct storage operations or a call-scoped recording argument, not a
  process-wide receiving switch.
- Configuration import, merge, and received configuration records preserve
  the destination's D1 credential values, including their absence, and pending
  work (R56). Only its local D1 key ID may move to resolve a collision with
  another key. Ordinary key IDs/references and R2 behavior remain unchanged.
- Newly delivered vaults have their queue and note flags cleared before use
  (R57). This block supplies the cleanup operation; block 2 wires it into the
  existing download and desktop creation paths. Ordinary restarts retain work.
- Vault names already reject `/`. Machine names must also reject it. Do not
  implement support for slash-containing vault names or a Linux copy workflow.
- Use the current SQLite, curl, JSON, credential, runtime, and test facilities.
  The proposed `sync_storage` and `d1_client` units are small implementation
  locations, not permission to build a replication framework.

## 4. Code entry points

| Area | Start here |
|---|---|
| Machine and vault configuration | `src/app/application_config.{h,cpp}`, `src/util/path_name.cpp` |
| Credentials and key allocation | `src/providers/credentials.h`, `src/providers/api_key_store.cpp`, `src/workspace/workspace.cpp`, `src/workspace/workspace_config_editor.cpp` |
| Database creation and opening | `src/storage/workspace_session_database.cpp`, `src/workspace/workspace_config_store.cpp` |
| Notes and imports | `src/storage/note_storage.{h,cpp}`, `src/storage/folder_store_storage.cpp`, `src/notes/notes_service.cpp` |
| Configuration commits | `commit_and_publish`, `commit_imported_rows`, and `merge` in `src/workspace/workspace_config_store.cpp` |
| Transport conventions | `src/app/r2_database_transfer.cpp`, `src/util/curl.{h,cpp}` |
| Refresh feasibility | `src/runtime/live_session_manager.cpp`, `webapp/src/useLiveSession.ts`, `webapp/src/chaweb/useChaweb.ts` |
| Tests/build | `CMakeLists.txt`, `tests/storage`, `tests/workspace`, `tests/app`, `tests/support/mock_http_server.h` |

The source of truth for record fields is design section 7.2. Keep one shared
format. Notes use path arrays, configuration uses row names, and chats will use
forum/session IDs. Do not invent a second representation for the next block.

## 5. Implementation scope

### A. Resolve the feasibility checks

Perform plan phase 0 before implementing behavior that depends on its results:

1. Send a REST batch with a bound 1,900,000-byte piece to multiple recipient
   tables, including representative UTF-8/JSON content.
2. Confirm the actual full-database error and that dropping a table permits
   another write. Prefer an available disposable Workers Free database;
   check the resource's actual capacity rather than assuming a fixed row count
   or filling a paid database as if its limit were 500 MB.
3. Observe batch behavior with a deliberately failing statement, and the
   reported row-read count for an empty queue. Uncertain delivery and duplicate
   retries remain accepted regardless of the result.
4. Exercise a chat reservation with reason `reloading`, then release it.
   Check desktop and ChaWeb separately, for idle and generating chats. Desktop
   has a stream recovery path; ChaWeb currently polls only during generation.
   The approved ChaWeb cadence is 30 seconds while idle and 1 second during
   a reply, using the same read timer. Hidden pages remain paused; existing
   immediate reads on opening/returning to the page and failure retries remain.
   Identify the smallest change to each existing path that makes R35 work.
   Record evidence and the required production changes for blocks 2 and 3.
   Do not add a second polling subsystem or new visible UI for this probe.

Use a small test/probe where needed and remove disposable probe artifacts.
If a live check cannot run, finish independent local work and record the
blocker, but do not call the feasibility gate or the whole block complete.
Do not silently replace a failed design assumption with another service.

### B. Configuration, credentials, and local storage

Add optional `machine` loading and pass it through existing application/store/
journal construction paths. A non-string, empty value, or value containing `/`
is ignored with a warning. The application still starts and treats the machine
as unnamed. A vault records only when it has both valid machine identity and
D1 credentials; a Worker stopped for a missing table is still connected.

Store account ID, database ID, and API token through the existing database-backed
credential pattern. Preserve the saved token when an update omits it, and expose
token presence rather than its value in detail results. D1 credential changes
create no change record. Keep the key allocation counter valid.

Install the additive schema through existing schema-ensure paths:

```sql
CREATE TABLE IF NOT EXISTS sync_queue (
    record TEXT NOT NULL
) STRICT;

-- Add only when absent, using the repository's migration pattern.
ALTER TABLE note_items ADD COLUMN changed INTEGER NOT NULL DEFAULT 0
    CHECK (changed IN (0, 1));
```

Use SQLite `rowid` for outgoing order. Supply small operations to append within
the caller's transaction, read records, remove sent records, and clear queue and
flags together. New/existing and protected vaults use the same schema behavior.
Keep whole records locally; splitting belongs to the Worker in block 2.

### C. D1 REST client

Implement the query endpoint with bound parameters, statement-result checking,
and the existing curl conventions: 10-second connection timeout and 30-second
low-speed timeout. Keep network work outside SQLite transactions and shared
application/storage locks.

The remote schema is:

```sql
CREATE TABLE IF NOT EXISTS "q/<vault>/<machine>" (
    piece INTEGER NOT NULL,
    pieces INTEGER NOT NULL,
    record TEXT NOT NULL
);
```

Provide create/list/read/insert-batch/delete-handled-prefix/count/drop operations.
Discover tables from `sqlite_master`. Parse the three `/`-separated components,
compare vault/machine identity with ASCII case-insensitive equality, and exclude
the current machine. Do not use `LIKE`; `%` and `_` can occur in valid names.
Double embedded quotes in SQL identifiers. One piece uses one REST batch with
one insert for each recipient; payload text is a bound parameter.

Report enough error detail for block 2 to distinguish full storage, oversized
results, missing tables, authentication, rate limits, and daily quotas. Only the
specified full-storage error allows eviction. This client does not own a Worker,
retry scheduler, local queue, or UI.

### D. Notes

Complete both production record creation and incoming application:

- Create, rename, delete, read-only, and both compression commit paths record
  immediately in their storage transaction. Capture paths before mutations.
- Text saves and content imports into existing notes set `changed = 1`.
  Scroll-only saves do not. Preserve the existing save algorithm and 12 MiB
  uncompressed note limit.
- Book/folder imports create one record per imported item, parents first.
  New summary notes include `summary_of`; summary replacement sets the flag.
- Collect each flagged note's current path/content/fields into `note.update`
  and clear its flag in the same transaction. A later edit sets it again.
- Incoming updates write directly, including to read-only notes. Missing
  parents cause a warned skip; name collisions receive the approved suffix;
  `note.update` creates a missing note. Keep design section 10.2's rules.
- Incoming updates remove audio and its pending attempt in the transaction.
  Reuse the summary-replacement pattern, existing abandoned-audio interruption,
  and `note_audio_finish`'s refusal of a removed attempt.
- Rebuild folder ANNOTATIONS with local IDs. A missing summary source chat
  leaves the note unlinked. ANNOTATIONS and managed forum-folder changes produce
  no outgoing records or flags.

Return enough information for the caller to know that notes changed. The
production receive-group notification is wired by block 2 and displayed by
block 3; no separate persistent notification state is needed.

### E. Configuration

Use a shared changed-row helper at both existing save paths:
`commit_and_publish` for Settings/merge and `commit_imported_rows` for import.
Import must capture the old rows before replacement. Write one `config.edit`
containing changed `set` rows and `delete` names in the operation's transaction.
Filter D1 credential rows separately out of the old and new sets before finding
the delta. Do not suppress the union of their path names: an ordinary key that
now occupies a former D1 path must be included. A D1-only action emits nothing.

Before validating import/merge, ignore source D1 rows with a warning and retain
the destination's D1 values, including retaining none. If a source non-D1 key
takes the destination D1 key's `system/keys/<id>/` path, keep the source key
there and move only the local D1 rows to a free ID using the existing key
allocator. Preserve all D1 values, connection, and pending records/flags.
No provider or other ordinary key reference is remapped. With no collision,
retain the D1 ID. Reuse merge's maximum-counter rule and advance the counter
above all retained/allocated IDs before validation. Preserve ordinary API-key
and R2 behavior.

On receipt, overlay the change and apply the same local-D1 preservation rule
before validation. An incoming ordinary key can move the local D1 rows to a
free ID; a deletion at that path cannot remove the local D1 setup. Preserve
credential absence too. The local relocation creates no outgoing work.
Validate the complete candidate once and commit/publish once with recording
disabled. In that transaction prune sessions for every removed forum using
`prune_orphan_sessions`, and keep managed-folder
updates. Use existing live-session maintenance/refresh behavior through an
internal apply path that a Worker can call without stopping or joining itself.
An invalid candidate is one warned, skipped record. Add no deleted-forum field.

## 6. Validation and completion criteria

Register source/tests in the existing CMake targets as they are added. Configure
and build with `cmake --preset ninja` and `cmake --build --preset ninja`. Run
focused existing/new tests for the touched code; discover exact CTest names
with `ctest --test-dir build/ninja -N`. Use loopback HTTP and temporary databases
for routine failures, including protected databases. Reserve the full platform
regression run for block 3.

Required evidence:

- Migration and reopen preserve existing content; an aborted operation also
  rolls back its outgoing record. Disconnected operations add no records/flags.
- Invalid machine settings do not prevent unrelated work. Credential/token
  persistence and key allocation work without exposing secrets.
- REST requests, response failures, valid special-character names, recipient
  exclusion, and the required live feasibility behavior are checked.
- Note imports, summaries, structural operations, flag collection, large content,
  audio removal, annotations, missing paths/collisions, and no echo are covered.
- Configuration import and merge preserve destination D1 setup with absent,
  matching, and different source credentials. Existing pending work survives.
  Multi-row changes and multi-forum removal apply atomically with no echo.
- A source ordinary API key and its provider reference keep their ID when it
  collides with the destination D1 ID. Only D1 moves; its values and pending
  work survive and the counter remains valid. The emitted record includes the
  ordinary key at the old D1 path and excludes D1 rows. Receiving that record
  on a second colliding vault preserves its local connection with no echo.
  A received deletion at the local D1 path also preserves its D1 setup.
- The application still builds and works without D1 configured. There is no
  production Worker or new D1 screen yet; those belong to later blocks.

Block 1 is complete when these checks pass and the real storage/apply/transport
operations are available to block 2. A mock-only implementation or unresolved
required live feasibility check is not completion.

## 7. Handoff instructions

Review the diff for accidental scope growth. Remove temporary probes and unused
helpers. Fill in the execution record below with concise evidence and exact
implemented entry points. State missing work honestly; do not transfer assigned
unfinished scope to block 2 merely to declare completion. Leave unrelated user
changes intact. The final response should identify the code revision/working
tree state, tests, any blocker, and whether block 2 may start.

### Execution record — to be filled by the implementing session

- Status: Not started.
- Code revision / relevant working-tree files: Pending.
- Implemented queue, credential, record, note/config apply, and D1 client entry points: Pending.
- Live feasibility results and test resource disposition, without secrets: Pending.
- Desktop/ChaWeb reservation findings and remaining integration work: Pending.
- Validation commands and outcomes: Pending.
- Decisions, deviations, or unresolved blockers: Pending.
- Block 2 ready to start: No.
