# Workspace session database implementation plan

Status: Proposed

This plan implements `docs/design.md`. It is deliberately split into separate
deliveries so the migration tool can be built, tested, and run before normal
CHA starts using the workspace database.

The required order is:

1. Ship a migration build whose normal runtime still uses legacy per-session
   databases.
2. Stop CHA, back up the workspace, run migration, and manually remove the
   legacy database files.
3. Ship the runtime conversion to `workspace/sessions.sqlite3`.
4. After all required workspaces have been converted, remove the temporary
   migration command and importer while retaining legacy-layout detection.

Do not combine steps 1 and 3 into one deployment. The manual cutover is a hard
boundary and the phase-2 runtime must never start against a workspace that has
not completed it.

## Final invariants

The completed implementation must maintain these invariants:

- A workspace has one persistent `sessions.sqlite3` and one
  `sessions.sqlite3.cha-lock` companion.
- `(forum_id, session_id)` remains the public identity; `session_key` is an
  internal database key.
- Every persistent session has a foreign-key relationship to one forum row.
- Only one CHA process can own a workspace database.
- Repository calls use short-lived SQLite connections. A live actor owns its
  own connection, and no connection is shared between threads.
- Every transaction that may write starts with `BEGIN IMMEDIATE` before any
  reads.
- Welcome uses a private temporary file outside the workspace, but that file
  has the same schema and journal code path as the workspace database.
- Migration copies legacy data but never modifies, renames, moves, or deletes
  a legacy database.
- Normal startup never imports legacy files. It only detects them and refuses
  an incomplete cutover.
- Recoverable deletion is an `archived_at` update, not a filesystem move.
- Existing URLs, HTTP response shapes, backup, workspace reload,
  interrupted-turn recovery, and Markdown downloads remain compatible.

The implementation must not add automatic startup migration, source
fingerprints, a migration manifest, automatic legacy cleanup, multi-process
database sharing, session moves between forums, forum-rename propagation,
archive restore/purge UI, a new cross-session-history browser route, or live
filesystem backup of a WAL database.

## Expected source organization

Keep the new storage code in `src/session/`. The exact names may be adjusted
while implementing, but each responsibility should have one obvious home.

| Responsibility | Expected source area |
| --- | --- |
| Reusable SQLite connection, statement, binding, and transaction helpers | New internal `sqlite_storage.h/.cpp` extracted from `session_database.cpp` |
| Workspace path constants and permanent legacy-file detection | New `session_storage_layout.h/.cpp` |
| Workspace schema, validation, checkpoint, and session SQL | New `workspace_session_database.h/.cpp` |
| Temporary read-only legacy `.sqlite3` adapter | New `legacy_session_import.h/.cpp` |
| Temporary migration orchestration and summary | New `session_migration.h/.cpp` |
| Runtime catalog and session operations | Existing `session_repository.h/.cpp` rewritten around the workspace database |
| Actor journal and restore wiring | Existing `session_database.h/.cpp`, `session_controller.*`, and `workspace/session_open.*` |
| Temporary command-line mode | `web/application_config.*` and `web_main.cpp` |
| Backup, reload, and HTTP adaptation | `web/workspace_backup.*` and `web/lobby_routes.cpp` |

Add all new sources and tests to `CMakeLists.txt`. The SQLite wrappers are an
internal implementation detail of `cha_core`; do not expose `sqlite3` handles
through public application APIs.

## Phase 1: migration build

Phase 1 adds `chaweb --migration`. Normal server execution must remain on the
current per-session storage implementation throughout this phase.

### 1. Protect the current behavior

- [ ] Run the existing unit, web, stress, integration, and process suites and
      record a clean baseline.
- [ ] Add focused fixtures capable of creating current legacy schema version 3
      databases and supported version 2 databases without modifying the
      source during a test.
- [ ] Add fixture helpers that can create active and `deleted/` sources in
      multiple forums, duplicate public identities, earlier history epochs,
      and an unfinished turn.
- [ ] Keep the migration fixtures separate from the final workspace database
      fixtures so a test cannot accidentally validate a source with target
      code.

### 2. Extract the reusable SQLite primitives

The current `Database`, `Statement`, and `Transaction` implementations are
private to `session_database.cpp`. Extract them into a small internal storage
module before adding the second schema.

- [ ] Move connection ownership, open modes, statement preparation, parameter
      binding, row access, error reporting, `changes()`, and PRAGMA reads
      without changing behavior.
- [ ] Preserve read-only, read-write, and read-write-create modes.
- [ ] Preserve `SQLITE_OPEN_NOMUTEX`; each handle continues to be confined to
      one thread.
- [ ] Configure every connection with `PRAGMA foreign_keys = ON` and
      `PRAGMA busy_timeout = 5000`.
- [ ] Keep `Transaction` unconditionally based on `BEGIN IMMEDIATE`, automatic
      rollback on destruction, and explicit commit.
- [ ] Keep legacy database creation, restore, journal, and migration tests
      passing after the extraction. This step must be behavior-only and should
      not introduce the workspace schema yet.

### 3. Implement the workspace schema module

- [ ] Define a workspace-specific SQLite `application_id` distinct from
      legacy `CHA1`; use one named constant and freeze schema `user_version` at
      1.
- [ ] Implement the `forums`, `sessions`, `turns`, and `entries` schema exactly
      as specified in `docs/design.md`, including composite keys, foreign keys,
      checks, and partial indexes.
- [ ] Create `active_sessions_by_update` as a partial index led by
      `updated_at`; rely on the unique `(forum_key, session_id)` index for
      identity resolution and per-forum listing.
- [ ] Put schema creation in one reusable function used by both migration and
      final runtime. Do not copy the schema text into the migration command.
- [ ] Put database identity and schema-version validation in the same module.
- [ ] Add full target validation: required schema objects, session state
      invariants, one prompt per turn, composite relationships,
      `PRAGMA foreign_key_check`, and `PRAGMA integrity_check`.
- [ ] Implement only schema version 1 creation and validation now. Reject every
      other workspace schema version; do not add a migration registry for
      hypothetical versions. When a real version 2 exists, its upgrade must run
      under the workspace lease and update `user_version` last.
- [ ] Add helpers for rollback-journal target creation, WAL runtime
      initialization, read-only final validation, and
      `PRAGMA wal_checkpoint(TRUNCATE)`.

### 4. Add permanent storage-layout helpers

This small module survives Phase 3.

- [ ] Define the workspace database path as `workspace/sessions.sqlite3`, the
      unpublished migration path as
      `workspace/.sessions.sqlite3.migrating`, and the lease companion derived
      from the workspace database path.
- [ ] Scan each configured persistent forum for regular files directly under
      `forums/<forum>/sessions/*.sqlite3` and
      `forums/<forum>/sessions/deleted/*.sqlite3`.
- [ ] Do not treat nested files, lock files, rollback journals, WAL/SHM
      sidecars, or the Welcome database as legacy sources.
- [ ] Return sources in deterministic sorted-path order and record whether
      each source is active or archived.
- [ ] Expose a cheap `has_legacy_session_databases()` operation for final
      startup. It must inspect paths only and never open a legacy database.
- [ ] Test all accepted and ignored path forms, including an absent sessions
      directory.

### 5. Extend workspace lease diagnostics

- [ ] Extend the existing lease acquisition helper to accept an explicit busy
      description while retaining the current one-argument behavior needed by
      phase-1 per-session leases.
- [ ] Make both migration and final runtime report
      `Workspace already in use: '<workspace-root>'` rather than deriving
      `sessions` from the database filename.
- [ ] Keep lease acquisition non-blocking and keep using the fixed companion
      file and operating-system lock.
- [ ] Test same-process and second-process conflicts, release after clean exit,
      release after forced exit, stale unlocked companion files, and the exact
      workspace-oriented diagnostic.

### 6. Implement the read-only legacy adapter

This module is temporary and is only for migration of discovered `.sqlite3`
files.

- [ ] Open every source with SQLite read-only flags. Never call the existing
      in-place `migrate_session_database()` on a source.
- [ ] Support exactly the legacy versions already accepted by the application:
      version 2 without `entries.created_at` and version 3 with timestamps.
      Reject every other application ID or version.
- [ ] For version 2, expose `created_at = 0` while copying rather than adding a
      column to the source.
- [ ] Read and validate embedded session ID, forum ID, label, durable counters,
      history epoch, all turns, and every entry from every epoch.
- [ ] Validate that the embedded forum matches the containing forum directory
      and that the embedded session ID matches the filename.
- [ ] Validate positive IDs, enum values, prompt/turn relationships, one
      started turn, and all other invariants currently enforced by legacy
      restore.
- [ ] Capture source row counts and file modification time. Convert the file
      time to the target's Unix-seconds `updated_at`.

### 7. Implement migration preflight

Perform all checks before creating the unpublished target.

- [ ] Load and validate the filesystem workspace normally so configured forum
      IDs are authoritative.
- [ ] Acquire the workspace lease before checking or creating target paths.
      Document that a phase-1 server does not honor this lease and therefore
      must already be stopped.
- [ ] Refuse an existing `sessions.sqlite3` without opening or modifying it.
- [ ] Refuse an existing `.sessions.sqlite3.migrating`; require the operator to
      inspect and remove stale generated output.
- [ ] Discover and sort all active and archived sources. Refuse an empty source
      set without creating a target.
- [ ] Validate every source before target creation. Report the exact source
      path on failure.
- [ ] Reject a duplicate `(forum_id, session_id)`, including an active/deleted
      duplicate, and name both source paths.
- [ ] Capture source counts and planned target counts for final verification.

### 8. Build and publish the migration target

- [ ] Create `.sessions.sqlite3.migrating` in rollback-journal mode with
      foreign keys and full synchronous behavior. Do not enable WAL before
      publication.
- [ ] Start one `BEGIN IMMEDIATE` target transaction.
- [ ] Insert every configured persistent forum with `INSERT OR IGNORE`.
- [ ] For each sorted source, insert its session row, obtain `session_key`, and
      copy counters, turns, and entries with that key.
- [ ] Set `updated_at` from the source database modification time.
- [ ] Set `archived_at` to the migration time for a source under `deleted/` and
      to null for an active source.
- [ ] Preserve labels, current history epoch, next IDs, earlier epochs,
      interrupted turns, entry timestamps, and all request/entry IDs.
- [ ] Compare source and target counts for sessions, turns, and entries inside
      the transaction.
- [ ] Run relationship and foreign-key validation, then write the application
      ID and schema version as the final transaction changes.
- [ ] Commit, close, reopen read-only, and run complete application and SQLite
      integrity validation.
- [ ] Publish with the existing `archive_without_replacement()` helper. Map a
      destination conflict to a migration target error. Do not implement a
      second no-replace rename path; the existing helper already supplies the
      WSL/9p hard-link fallback.
- [ ] Read the committed published database to calculate the final summary.
      Print target path and active, archived, turn, and entry counts.

On an ordinary caught failure, roll back, close, and remove only the
unpublished target and its sidecars when that cleanup is safe. Never remove a
published target and never touch a source. If forced termination leaves the
temporary path, the next run must refuse it and explain the manual action.

### 9. Add the temporary command-line path

- [ ] Add an execution mode to `ApplicationConfig` and accept flag-form
      `--migration` without a value.
- [ ] In migration mode, accept the existing root/config/workspace selection
      inputs but do not require host, port, or backup-directory settings.
- [ ] Reject explicitly supplied server-only flags with `--migration`; server
      settings already present in `app.toml` may be ignored.
- [ ] Update usage text and argument tests for flag position, duplicates,
      missing workspace, incompatible options, and ordinary server parsing.
- [ ] In `web_main.cpp`, branch to migration immediately after resolving the
      workspace and before constructing `SessionRepository`, providers,
      `LiveSessionManager`, HTTP routes, or browser assets.
- [ ] Run workspace loading and migration, print the summary to stdout, print
      a source-specific error to stderr on failure, and return status 0 or
      nonzero.
- [ ] Ensure migration creates no Welcome database and binds no HTTP listener.

### 10. Test and release the migration build

Add focused tests, preferably in new
`tests/session/unit_workspace_session_database.cpp` and
`tests/session/unit_session_migration.cpp`, plus command-level cases in the
existing process-test target.

- [ ] Verify workspace schema creation, identity/version rejection, foreign
      keys, partial indexes, and integrity validation.
- [ ] Import active and deleted sessions from several forums.
- [ ] Import the same session ID in different forums and reject duplicates
      within one forum.
- [ ] Verify version 2 and version 3 sources, previous history epochs,
      timestamps, counters, and unfinished turns.
- [ ] Reject malformed databases, unsupported versions, identity/path
      mismatches, missing prompts, invalid enums, and broken relationships.
- [ ] Verify that one bad source prevents publication of every source.
- [ ] Hash or byte-compare every source before and after both successful and
      failed migration.
- [ ] Refuse existing published and temporary targets without changing them.
- [ ] Simulate transaction, validation, and publication failures and verify
      that no target is published.
- [ ] Verify deterministic ordering and target-derived summary counts.
- [ ] Verify the workspace lease blocks a concurrent migration and reports the
      workspace path.
- [ ] Run all existing tests to prove normal phase-1 server behavior is
      unchanged.
- [ ] Run `chaweb --migration` on a disposable copy of a representative real
      workspace and inspect `integrity_check`, `foreign_key_check`, row counts,
      and several restored transcripts.

Phase 1 is complete only when the migration build can be installed and run
without changing the normal server's storage path.

## Manual cutover

These are operator steps, not application startup behavior.

### 11. Prepare the maintenance window

- [ ] Install the migration build but do not install the phase-2 runtime yet.
- [ ] Stop every CHA process that can access the workspace and verify the HTTP
      process is gone.
- [ ] Create an ordinary full-workspace backup and confirm that the archive can
      be opened.
- [ ] Ensure free disk space is sufficient for all legacy files, the backup,
      and the new workspace database at the same time.

### 12. Run and verify migration

- [ ] Run `chaweb --migration --workspace <path>` once.
- [ ] Save the command output and verify its active, archived, turn, and entry
      counts against the expected workspace contents.
- [ ] Confirm `sessions.sqlite3` exists and
      `.sessions.sqlite3.migrating` does not.
- [ ] Open the result read-only and run `PRAGMA integrity_check` and
      `PRAGMA foreign_key_check` if independent verification is desired.
- [ ] Spot-check sessions from multiple forums, an archived session, an old
      history epoch, timestamps, and an interrupted turn.

If migration fails, leave the legacy files in place, correct the named problem,
remove only a confirmed unpublished `.sessions.sqlite3.migrating` if needed,
and retry. Do not proceed to cleanup after a failed or unverified run.

### 13. Perform manual cleanup and cross the rollback boundary

- [ ] While CHA remains stopped, manually move or delete only legacy
      `.sqlite3` files under each forum's `sessions/` and `sessions/deleted/`.
- [ ] Remove their legacy `.cha-lock`, `-journal`, `-wal`, and `-shm` sidecars.
- [ ] Preserve every non-session workspace file.
- [ ] Re-run the same discovery rules manually or with a small diagnostic to
      confirm no legacy `.sqlite3` file remains in either scanned location.
- [ ] Keep the original workspace backup until the phase-2 runtime has been
      validated.

Before legacy cleanup, rollback is possible by moving the new
`sessions.sqlite3` aside and running the old application. After legacy cleanup,
rollback requires restoring the complete workspace backup. Never run the old
runtime after migration and before cleanup because it can create or modify data
that is absent from the migrated target.

## Phase 2: workspace-database runtime

Phase 2 replaces normal storage only after the manual cutover has succeeded.
The temporary migration command may remain compiled during this phase, but it
must refuse the already existing target.

### 14. Enforce startup ownership and cutover state

- [ ] Make `SessionRepository` acquire the process-lifetime workspace lease
      before creating, opening, migrating, or inspecting
      `sessions.sqlite3`.
- [ ] Hold the lease until the HTTP server and all live actors have stopped and
      the repository is destroyed.
- [ ] After acquiring the lease, perform the permanent path-only legacy scan
      and implement all four startup states:

  | Workspace database | Legacy database | Startup action |
  | --- | --- | --- |
  | Present | Absent | Validate and open the workspace database |
  | Absent | Absent | Create a new empty workspace database |
  | Absent | Present | Refuse and require offline migration |
  | Present | Present | Refuse and require manual legacy cleanup |

- [ ] Run this guard before creating Welcome, binding HTTP, or modifying the
      workspace database.
- [ ] Validate application ID and schema version before reading application
      tables. Run any future schema migration while holding the lease and
      before actors exist.
- [ ] Enable WAL and full synchronous behavior for runtime databases.
- [ ] Test every startup combination, invalid database identity/version,
      second-process conflict, and stale unlocked companion file.

### 15. Rewrite `SessionRepository` around workspace SQL

Keep the repository constructible as
`shared_ptr<const SessionRepository>`.

- [ ] Store the workspace root, database path, workspace lease, and Welcome
      temporary-storage information. Do not store a repository SQLite
      connection or connection mutex.
- [ ] Open and configure one short-lived connection inside each catalog or
      maintenance operation and close it before returning.
- [ ] On construction and after successful workspace reload, insert every
      configured persistent forum ID with `INSERT OR IGNORE`.
- [ ] Keep old forum rows and their sessions when a forum disappears from the
      current filesystem workspace. Ordinary APIs must still require the forum
      to be currently configured.
- [ ] Replace `SessionCatalog` filesystem scans with repository SQL for list,
      inspect/validate, create, rename, archive, prepare/restore, Recent, and
      durable-history read.
- [ ] Resolve public identities through the `forums` join and carry
      `session_key` internally. Never assume `session_id` alone is unique.
- [ ] Generate timestamp IDs as today. In one `BEGIN IMMEDIATE` transaction,
      retry a suffix on `(forum_key, session_id)` collision; archived IDs remain
      reserved by the same unique constraint.
- [ ] Change `StoredSession.updated_at` to Unix seconds and remove its
      filesystem database path and per-file validation error fields. Update all
      callers that currently convert `file_time_type`.
- [ ] Add one workspace-wide Recent query filtered to currently configured
      forum IDs and ordered by `updated_at DESC`. Do not loop over forums and
      sort in C++.
- [ ] Add the repository-level durable-history operation. It returns one
      active session's current-epoch durable entries in `entry_id` order while
      another session may be active. It does not expose partial streaming text
      or synthesize interruption errors.
- [ ] Make rename and archive affect exactly one active row and update
      `updated_at` in the same transaction.

### 16. Convert restore and actor journaling

- [ ] Change `PreparedSession` to carry database path, `session_key`, identity,
      label, and restore state. Remove the per-session lease.
- [ ] For a persistent session, set the prepared database path to the workspace
      database. For Welcome, set it to its private temporary database.
- [ ] Change `SessionJournal` construction to
      `SessionJournal(database_path, session_key)`.
- [ ] Change `SessionController::from_workspace()` and
      `workspace/session_open.cpp` to pass `session_key` and no per-session
      lease.
- [ ] Scope every restore and journal statement by `session_key`, including
      state counters, current history epoch, turns, entries, clear, and live
      rename.
- [ ] Update `sessions.updated_at` in the same transaction as create, journal
      writes, clear, rename, and archive.
- [ ] Preserve separate short start and terminal transactions. Never hold a
      SQLite transaction across provider execution or streaming.
- [ ] Use `BEGIN IMMEDIATE` before any read in every write-capable operation so
      a second writer waits at transaction start instead of failing a deferred
      WAL snapshot upgrade.
- [ ] Keep interrupted-turn lookup and repair scoped to the selected
      `session_key`.
- [ ] Keep one long-lived SQLite connection per live actor and never share it
      with repository calls or another actor.

### 17. Convert Welcome to the shared schema and code path

- [ ] Keep the existing repository-owned private temporary directory outside
      the workspace.
- [ ] Create a file-backed temporary database; do not use plain `:memory:`.
- [ ] Initialize it with the exact workspace application ID, version, schema,
      PRAGMAs, and validation code.
- [ ] Insert exactly one `forums` row for Entrance and one active `sessions`
      row for Welcome, then retain Welcome's `session_key`.
- [ ] Make Entrance listing and Welcome preparation query that temporary
      database while create, rename, and archive remain rejected.
- [ ] Restore and journal Welcome through a separately opened actor connection
      using `SessionJournal(path, session_key)`.
- [ ] Remove the private directory at repository destruction after all actors
      have stopped. Keep it outside backup and migration semantics.

### 18. Replace filesystem deletion with transactional archival

- [ ] Keep the existing `LiveSessionManager` maintenance reservation and
      stop/join sequence for live deletion.
- [ ] After the actor and journal connection are gone, run one
      `BEGIN IMMEDIATE` update that sets `archived_at` and `updated_at` only
      when the row is active.
- [ ] Require exactly one changed row. On failure, leave the session active and
      release the reservation with existing error behavior.
- [ ] Exclude archived rows from list, inspect, open, rename, Markdown download,
      and ordinary history operations.
- [ ] Keep their turns and entries in place and keep their public identities
      reserved.
- [ ] Remove production use of per-session file moves, sidecar moves, and
      per-session leases. Do not add restore or permanent-purge UI in this
      change.

### 19. Adapt backup and workspace reload

- [ ] Add a repository checkpoint operation and call
      `PRAGMA wal_checkpoint(TRUNCATE)` after all actors have stopped and before
      `backup_workspace()` invokes `tar`.
- [ ] Keep the complete workspace, not only `sessions.sqlite3`, as the backup
      and transport unit.
- [ ] Change workspace reload to this order: reserve reload, stop/join actors,
      checkpoint, back up, load and validate the candidate workspace, insert
      new forum rows, publish the candidate, and release the reservation.
- [ ] Ensure candidate failure leaves the old filesystem workspace published.
- [ ] Ensure removing a forum from configuration does not delete its row or
      sessions.
- [ ] Document that copying a live WAL database with ordinary filesystem tools
      is unsupported.

### 20. Adapt web and application integration

- [ ] Update `web_main.cpp` construction order so the repository obtains the
      workspace lease and passes startup checks before the HTTP listener is
      bound.
- [ ] Replace `recent_sessions()`'s per-forum loops with the repository's one
      database query.
- [ ] Update forum session listings to consume integer `updated_at` directly.
- [ ] For inactive Markdown download, read the selected session from the
      workspace database without creating a live actor. Keep live download on
      the actor snapshot path.
- [ ] Preserve the existing HTTP routes, response JSON, status codes, initial
      Welcome selection, live rename ordering, and deletion reservation.
- [ ] Update `src/README.md`, `src/session/README.md`,
      `src/workspace/README.md`, and `src/web/README.md` to describe workspace
      ownership, short-lived repository connections, actor connections, and
      temporary Welcome storage.

### 21. Replace and expand runtime tests

- [ ] Replace per-file `SessionCatalog` and repository expectations with
      workspace-row expectations. Retain only legacy fixtures needed by the
      temporary migration tests.
- [ ] Test the same `session_id` in different forums and duplicate rejection
      within one forum, including archived rows.
- [ ] Test forum synchronization, removal/reintroduction of a forum, and
      rejection of ordinary access to an unconfigured forum.
- [ ] Test independent counters, epochs, turns, entries, clear, rename,
      archive, restore, and interrupted-turn repair for multiple
      `session_key` values.
- [ ] Test Workspace Recent across forums and verify ordering and exclusion of
      archived or currently unconfigured forums.
- [ ] Test durable history read while another session is active and ensure a
      started turn in the other session is not interpreted as interrupted.
- [ ] Test Welcome's one-forum/one-session temporary database by initializing
      it on one connection and restoring/journaling it on an actor connection.
- [ ] Run several live journals against the same WAL database and verify no
      row, counter, or interrupted turn crosses session boundaries.
- [ ] Add a deterministic competing-writer test proving the second write waits
      within `busy_timeout` and does not fail with `SQLITE_BUSY_SNAPSHOT`.
- [ ] Test clean and forced process exit, WAL recovery, workspace lease release,
      and startup refusal before HTTP binding.
- [ ] Test inactive and live Markdown downloads, rename, archive, reload,
      backup checkpointing, and restart.
- [ ] Update stress tests that currently assert one lease/database per session;
      instead assert one persistent database/lease and concurrent actor
      isolation.
- [ ] Run all unit, web, stress, integration, and process suites under the
      normal build and available sanitizer configurations.

Phase 2 is complete only when the final-runtime acceptance criteria in
`docs/design.md` pass against a manually converted workspace and a newly
created empty workspace.

## Phase 3: remove migration-only code

Do this only after every workspace that must be retained has been converted and
backed up.

### 22. Remove the temporary execution mode

- [ ] Remove `--migration` from argument parsing, `ApplicationConfig`, usage
      text, `web_main.cpp`, and command-level documentation.
- [ ] Remove `session_migration.*`, the legacy `.sqlite3` adapter, and their
      migration-only fixtures and tests.
- [ ] Remove source-list validation, migration summaries, unpublished-target
      handling, and migration-specific error types that have no runtime use.
- [ ] Remove `archive_without_replacement()` only if it has no remaining caller;
      verify with a repository-wide reference search first.
- [ ] Remove obsolete legacy database creation/journal/catalog code only when
      no supported operation still uses it.
- [ ] Remove now-unused sources from `CMakeLists.txt`.

### 23. Retain the permanent incomplete-cutover guard

- [ ] Keep `session_storage_layout.*` and its cheap scan of the two legacy
      locations.
- [ ] Keep all four startup-state checks and their tests.
- [ ] Keep errors for “offline migration required” and “manual cleanup
      incomplete.” After Phase 3, the first error should direct the operator to
      an archived migration build.
- [ ] Keep the detector path-only: it must never open, validate, import, move,
      or delete a legacy file.

### 24. Final cleanup and verification

- [ ] Remove stale comments and documentation referring to normal per-session
      databases, per-session leases, filesystem deletion, or the temporary
      command.
- [ ] Confirm the final schema has no migration manifest, source fingerprint,
      or cleanup state.
- [ ] Confirm startup with legacy files still refuses before creating or
      opening application state beyond the workspace lease.
- [ ] Confirm a new workspace creates exactly one persistent database and one
      lock companion, while Welcome creates only private temporary files.
- [ ] Re-run the complete test matrix and a backup/restore smoke test on the
      converted workspace.
- [ ] Retain the migration build and the pre-cutover workspace backup until the
      converted runtime has been used and verified for an agreed period.

## Recommended commit boundaries

Keep commits small enough that each can be reviewed and tested independently:

1. SQLite helper extraction with no storage behavior change.
2. Workspace schema, validation, layout detection, and unit tests.
3. Read-only legacy adapter and import tests.
4. Migration orchestration, publication, CLI, and process tests.
5. Workspace lease and startup format guard.
6. Repository queries and data-type conversion.
7. Prepared session, actor journal, and Welcome conversion.
8. Transactional archive and cross-session history read.
9. Backup, reload, and web integration.
10. Runtime concurrency, process, and stress-test conversion.
11. Migration-mode removal after operational cutover.

Every commit should build and pass its directly affected tests. The migration
release, runtime release, and final cleanup each require the complete test
suite.

## Definition of done

Implementation is complete when:

- Phase 1 produced a validated `sessions.sqlite3` without changing any legacy
  source.
- The operator completed and recorded the manual backup, migration,
  verification, and legacy cleanup.
- Phase 2 runs both converted and new workspaces with one persistent database,
  correct forum ownership, transactional archival, concurrent actor isolation,
  and compatible web behavior.
- Welcome uses the same schema and `SessionJournal(path, session_key)` path from
  a private temporary file.
- Repository operations remain `const` and use short-lived connections.
- All write-capable transactions use `BEGIN IMMEDIATE`.
- Backups contain a checkpointed main database and restore cleanly on another
  machine.
- Phase 3 has removed the importer and temporary CLI while retaining permanent
  read-only legacy-layout detection.
- All acceptance criteria in `docs/design.md` and all project test suites pass.
