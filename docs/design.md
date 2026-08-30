# Workspace session database design

Status: Proposed

## Purpose

CHA currently stores every persistent session in a separate SQLite database
below its forum. The layout isolates sessions well, but produces an increasing
number of database and lock files and prevents simple workspace-wide session
queries.

This design replaces those files with one persistent SQLite database per
workspace. The transition is deliberately manual and occurs in two code phases:

1. Add a temporary offline `--migration` mode that copies existing session
   databases into a new workspace database. Normal application operation still
   uses the old storage design in this phase.
2. After the operator has run migration and manually removed the old files,
   replace normal application storage with the workspace database.

Migration never deletes, moves, renames, or modifies a legacy session database.
If it fails, it reports the error and leaves the workspace's existing session
data untouched. Cleanup is an explicit operator action.

The temporary migration command is removed after the required workspaces have
been converted. The final runtime retains a read-only legacy-layout detector
as a startup safety guard, but contains no legacy database reader, automatic
import, manifest, fingerprint, or cleanup machinery.

This document supersedes the storage layout, per-session lease, and physical
deletion sections of `docs/session-design.md`. The user-facing rename/delete
behavior and live-session maintenance reservation defined there remain
applicable.

## Goals

- Store all persistent sessions for one workspace in one SQLite database.
- Record and enforce the forum that owns each session.
- Keep `(forum ID, session ID)` as the durable external session identity.
- List and query sessions belonging to a forum without inspecting database
  files.
- Provide a repository-level durable-history read for one session while
  another session is active.
- Allow only one CHA process to own a workspace database.
- Provide a small, explicit, offline command that copies all legacy session
  content into a new workspace database.
- Preserve active sessions, archived sessions, labels, transcript IDs, turns,
  history epochs, timestamps, counters, and interrupted turns.
- Produce either a completely validated workspace database or no published
  workspace database.
- Keep legacy cleanup and rollback under direct operator control.
- Remove the migration-only code after the transition.

## Non-goals

- Automatically migrating sessions during normal application startup.
- Automatically deleting, moving, or archiving legacy database files.
- Resuming legacy-file cleanup after a crash.
- Merging legacy files into an existing workspace database.
- Sharing one workspace between multiple CHA processes.
- Running a workspace database on a network filesystem.
- Moving a session between forums.
- Renaming a forum and automatically changing stored session ownership.
- Storing forum display names, members, prompts, or defaults in SQLite.
- Making the process-local Welcome session persistent.
- Adding a new browser route or UI for browsing another session's history. The
  storage-level read needed by such a feature is in scope.
- Adding browser operations for restoring or permanently erasing archived
  sessions.
- Supporting downgrade after the operator has deleted the legacy files.

## Terms

- **Workspace database** is `sessions.sqlite3` at the workspace root.
- **Workspace lease** is the process-lifetime operating-system lock preventing
  a second new-version CHA process from using the same workspace database.
- **Legacy database** is one per-session `.sqlite3` file created by the
  current storage design.
- **Forum identity** is the stable URL-safe forum ID from the workspace.
- **Session identity** is the public pair `(forum_id, session_id)`.
- **Session key** is an internal SQLite integer key used by related tables. It
  is never exposed in URLs or application protocols.
- **Active stored session** has a null `archived_at`.
- **Archived session** remains durable but is excluded from ordinary listing,
  open, rename, and history operations.
- **Live session** is an active stored session currently owned by a
  process-local `LiveSession` actor.

## Transition strategy

### Phase 1: add migration mode

The first code change introduces:

- the new workspace schema;
- reusable schema creation and validation code;
- a read-only legacy database importer; and
- a temporary `--migration` command.

Normal CHA operation remains on the existing per-session database design.
This permits the migration build to be prepared and tested without changing
the running application's storage path.

The migration command must use the exact schema creation and validation
components that the final runtime will use. It must not carry a private copy of
the future schema that can drift before phase 2.

### Manual maintenance window

The operator performs the transition while CHA remains stopped:

1. Stop every CHA process using the workspace.
2. Create an ordinary backup of the complete workspace.
3. Run `cha --migration` against that workspace.
4. Review the success summary and retain the generated
   `sessions.sqlite3`.
5. Manually move or delete the legacy `.sqlite3` files and their legacy
   `.cha-lock`, `-journal`, `-wal`, and `-shm` sidecars.
6. Install or build the phase-2 application that uses the workspace database.
7. Start CHA.

`cha --migration` denotes the intended installed command. If the executable
continues to be named `chaweb`, the concrete invocation is
`chaweb --migration`.

The old normal application must not run after step 3. It does not write to the
new workspace database and could change or create legacy session files after
their contents were copied.

### Phase 2: switch normal storage

The second code change replaces filesystem session catalogs, per-session
databases, per-session leases, and file-based deletion with the workspace
database design described below.

During this phase, normal startup enforces the cutover state:

| Workspace database | Legacy databases | Behavior |
| --- | --- | --- |
| Present | Absent | Open the workspace database |
| Absent | Absent | Create an empty workspace database for a new workspace |
| Absent | Present | Fail and state that offline migration is required |
| Present | Present | Fail and state that manual legacy cleanup is incomplete |

This prevents the final runtime from silently hiding legacy sessions or
silently using an incomplete conversion.

### Phase 3: remove migration mode

After required workspaces have been converted:

- remove the `--migration` argument;
- remove legacy database readers, validation adapters, and import code;
- retain the read-only legacy-layout detector and incomplete-cutover startup
  checks; and
- retain the workspace schema, workspace database validation, and normal
  runtime storage.

The runtime never needs a legacy-import manifest because it never performs
legacy migration. The surviving detector only checks for regular `.sqlite3`
files at the two legacy source locations. It does not open, validate, import,
move, or delete them.

## Durable identity

Session IDs are not globally unique. Two forums may contain the same
timestamp-derived session ID. The database therefore enforces uniqueness on
`(forum_key, session_id)`, not on `session_id` alone.

An internal `session_key` prevents every turn and entry row from repeating
the forum and session text. Resolving a public identity is one indexed query:

```sql
SELECT s.session_key
FROM sessions AS s
JOIN forums AS f USING (forum_key)
WHERE f.forum_id = ?1
  AND s.session_id = ?2
  AND s.archived_at IS NULL;
```

The forum relationship is immutable after session creation. No ordinary
operation changes a session's `forum_key`.

The `forums` table stores durable IDs only. The filesystem workspace remains
authoritative for forum existence, display name, members, prompts, defaults,
and all other configuration. This avoids creating a second configuration
authority in SQLite.

## Filesystem layout

After cutover, persistent session storage is:

```text
workspace/
  sessions.sqlite3
  sessions.sqlite3.cha-lock
  forums/
    <forum>/
      config.toml
      FORUM.md
      members/
```

SQLite may create the fixed runtime sidecars `sessions.sqlite3-wal` and
`sessions.sqlite3-shm`. They belong to the one workspace database rather than
to individual sessions. Graceful shutdown and workspace backup checkpoint the
WAL.

The empty workspace lock file may remain after shutdown. Its existence does not
mean the database is busy; the held kernel lock does.

## Database identity and schema version

The workspace database uses a new SQLite `application_id`; it must not reuse
the legacy per-session `CHA1` identifier. The first workspace schema uses
`user_version = 1`.

Every open validates both values before reading application tables. Unknown
application IDs or schema versions are fatal.

Future workspace-schema migrations, unrelated to the one-time legacy import,
run once at normal startup while the process holds the workspace lease and
before any live-session connection exists. They execute transactionally and
advance `user_version` only as the final schema write.

## Workspace database schema

The logical schema is:

```sql
CREATE TABLE forums (
    forum_key INTEGER PRIMARY KEY,
    forum_id TEXT NOT NULL UNIQUE CHECK (forum_id <> '')
) STRICT;

CREATE TABLE sessions (
    session_key INTEGER PRIMARY KEY,
    forum_key INTEGER NOT NULL REFERENCES forums(forum_key),
    session_id TEXT NOT NULL CHECK (session_id <> ''),
    label TEXT NOT NULL CHECK (label <> ''),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    archived_at INTEGER CHECK (archived_at IS NULL OR archived_at >= 0),
    history_epoch INTEGER NOT NULL CHECK (history_epoch > 0),
    next_entry_id INTEGER NOT NULL CHECK (next_entry_id > 0),
    next_request_id INTEGER NOT NULL CHECK (next_request_id > 0),
    UNIQUE (forum_key, session_id)
) STRICT;

CREATE INDEX active_sessions_by_update
    ON sessions (updated_at DESC, forum_key, session_id)
    WHERE archived_at IS NULL;

CREATE TABLE turns (
    session_key INTEGER NOT NULL
        REFERENCES sessions(session_key) ON DELETE CASCADE,
    request_id INTEGER NOT NULL CHECK (request_id > 0),
    epoch INTEGER NOT NULL CHECK (epoch > 0),
    state INTEGER NOT NULL CHECK (state BETWEEN 0 AND 3),
    PRIMARY KEY (session_key, request_id)
) STRICT;

CREATE UNIQUE INDEX one_started_turn_per_session
    ON turns (session_key)
    WHERE state = 0;

CREATE TABLE entries (
    session_key INTEGER NOT NULL
        REFERENCES sessions(session_key) ON DELETE CASCADE,
    entry_id INTEGER NOT NULL CHECK (entry_id > 0),
    epoch INTEGER NOT NULL CHECK (epoch > 0),
    request_id INTEGER,
    kind INTEGER NOT NULL CHECK (kind BETWEEN 0 AND 3),
    participant_id TEXT NOT NULL,
    display_name TEXT NOT NULL CHECK (display_name <> ''),
    addressed_to TEXT NOT NULL DEFAULT '',
    addressed_to_name TEXT NOT NULL DEFAULT '',
    text TEXT NOT NULL,
    status INTEGER NOT NULL CHECK (status IN (0, 2, 3)),
    created_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_key, entry_id),
    FOREIGN KEY (session_key, request_id)
        REFERENCES turns(session_key, request_id),
    CHECK (kind NOT IN (0, 1) OR participant_id <> ''),
    CHECK (kind <> 0 OR (addressed_to <> '' AND addressed_to_name <> '')),
    CHECK (kind = 0 OR (addressed_to = '' AND addressed_to_name = '')),
    CHECK (kind <> 3 OR status = 3),
    CHECK (kind NOT IN (0, 2) OR status = 0),
    CHECK (kind <> 1 OR status <> 3),
    CHECK (kind <> 1 OR status <> 0 OR text <> '')
) STRICT;

CREATE INDEX entries_by_session_and_epoch
    ON entries (session_key, epoch, entry_id);

CREATE UNIQUE INDEX one_prompt_per_session_turn
    ON entries (session_key, request_id)
    WHERE kind = 0 AND request_id IS NOT NULL;
```

`history_epoch`, `next_entry_id`, and `next_request_id` move from each
legacy database's singleton `state` row into the corresponding `sessions`
row. They are session state, so a separate one-to-one table is unnecessary.

All uniqueness constraints that were implicitly per-session because each
session had its own database are explicitly scoped by `session_key`.

The unique `(forum_key, session_id)` index supports identity resolution and
per-forum listing. The partial `active_sessions_by_update` index has
`updated_at` as its leading column so one cross-forum query can produce
Workspace Recent in update order while ignoring archived sessions. A
forum-leading four-column index would not serve that query.

The existing numeric values for turn state, entry kind, and entry status are
preserved.

## Offline migration mode

### Command-line behavior

`--migration` is a top-level execution mode, not a server option. It may be
combined with the existing workspace-selection argument or configuration, but
does not start:

- the HTTP listener;
- live-session actors;
- provider workers;
- browser assets; or
- the process-local Welcome session.

It writes progress and the final result to the console and exits with:

- status 0 after successful publication; or
- nonzero status after any validation, import, filesystem, or SQLite error.

Host, port, and other server-only options have no effect in migration mode and
should preferably be rejected to make mistaken invocations visible.

### Preconditions

Migration requires:

- every CHA process using the workspace is stopped;
- the workspace loads and validates normally;
- no `sessions.sqlite3` already exists; and
- at least one legacy session database exists; and
- the operator has enough free disk space for the legacy files and the new
  database at the same time.

The command acquires `sessions.sqlite3.cha-lock` so two migration commands or
a migration command and a phase-2 runtime cannot operate concurrently. A
phase-1 old runtime does not know about this lock, so stopping it remains an
operator precondition rather than something migration can fully enforce.

The command opens legacy databases read-only. It does not run an in-place
legacy schema migration or recover a source by writing to it. A source that
cannot be read consistently in read-only mode causes the command to fail.

### Source discovery

Migration scans configured workspace forums for regular files at:

```text
forums/<forum>/sessions/*.sqlite3
forums/<forum>/sessions/deleted/*.sqlite3
```

Direct children of `sessions/` become active sessions. Direct children of
`sessions/deleted/` become archived sessions. The latter preserves the
current recoverable-delete behavior at negligible additional runtime
complexity.

Other extensions, nested directories, temporary files, and SQLite sidecars are
not independent sources. Discovery sorts paths before validation and import so
errors and generated internal keys are deterministic.

If discovery finds no legacy database, migration exits nonzero and creates no
target. Phase 2 creates an empty database automatically for a genuinely new or
empty workspace, so an offline migration with no input is unnecessary and is
more likely to be an operator mistake.

The built-in Welcome session is temporary and has no source to import.

### Source validation

Before creating the published target, migration validates every source:

- it is a supported legacy per-session database;
- its embedded forum ID matches its containing forum directory;
- its embedded session ID matches its filename;
- its label and durable state are present;
- its counters, turns, prompts, entries, epochs, kinds, and statuses satisfy
  existing session-database invariants; and
- its public identity does not duplicate another active or deleted source.

The same session ID in two different forums is valid.

Migration accepts every legacy schema version for which an explicit read-only
adapter is implemented. A version missing entry timestamps supplies the same
zero/default timestamp used by the current migration. An unknown version fails
with the source path and version; migration never guesses.

Validation reads raw durable rows, not only the current restore projection.
Rows from previous clear-history epochs are copied so the new database
preserves all content present in the source. A started turn is copied as
started and is repaired by the final runtime when that session is first opened,
using the existing interrupted-turn behavior.

### Target construction

Migration refuses to overwrite either an existing `sessions.sqlite3` or an
existing `.sessions.sqlite3.migrating`. A stale temporary target is generated
output, but the command still reports it and requires the operator to remove it
explicitly before retrying.

The command creates:

```text
workspace/.sessions.sqlite3.migrating
```

Target construction proceeds as follows:

1. Create the workspace schema with foreign keys enabled.
2. Begin one target transaction.
3. Insert every configured persistent forum ID.
4. For each validated source, insert one session row and obtain its internal
   `session_key`.
5. Copy its state counters, every turn, and every entry while adding that
   `session_key`.
6. Use the legacy database file modification time as `updated_at`.
7. For a source under `deleted/`, set `archived_at` to the migration time. A
   legacy filesystem rename does not reliably record deletion time in the
   database file's modification timestamp.
8. Compare source and target counts for sessions, turns, and entries.
9. Run target relationship and foreign-key validation.
10. Set the workspace application ID and schema version as the final writes.
11. Commit and close the target.
12. Reopen it read-only and run SQLite integrity and application validation.
13. Publish it as `sessions.sqlite3` with the existing
    `archive_without_replacement()` helper.

That helper already provides atomic no-replace publication and the hard-link
fallback needed by WSL/9p mounts. Migration maps its destination-conflict error
to an existing-target error. No new filesystem publication primitive is
needed.

The import streams rows from one source at a time. It does not load all
workspace transcripts into memory.

The migration target uses SQLite's rollback-journal mode. It does not enable
WAL before publication, which ensures the complete result is contained in the
single temporary database file. The phase-2 runtime enables WAL on its first
exclusive open.

### Failure behavior

Any failure aborts the command. The target transaction rolls back, the
published `sessions.sqlite3` remains absent, and every legacy source remains
unchanged.

If the command can safely remove its own unpublished temporary database and
sidecars, it does so. If process termination or a filesystem error leaves the
temporary path behind, the error message or next invocation tells the operator
to remove only that generated temporary output before retrying.

There is no resumable migration state. Rerunning migration reads all legacy
sources again and builds a fresh target.

Migration never:

- deletes, moves, renames, or writes a legacy database;
- removes a legacy sidecar;
- overwrites or merges an existing workspace database;
- skips an invalid source and publishes a partial result; or
- starts normal application service after publication.

### Success output

Success reports enough information for manual verification:

```text
Migration completed.

Forums:             3
Active sessions:   42
Archived sessions:  7
Turns:           1,284
Entries:         2,611
Database: /absolute/workspace/sessions.sqlite3

Legacy session files were not changed.
Do not run the old CHA application after this migration.
```

The actual counts are computed from the committed target and compared with the
validated source totals before they are printed.

### Manual cleanup and rollback

The command does not decide when legacy files are safe to erase. After success,
the operator should:

1. Retain the pre-migration workspace backup.
2. Optionally inspect the target with a read-only tool or a small verification
   command.
3. Remove or move aside the legacy `.sqlite3` files.
4. Remove their now-unused legacy lock and SQLite sidecar files.
5. Confirm that no legacy databases remain before installing the phase-2
   runtime.

Until the old files are removed, rollback consists of deleting or moving
`sessions.sqlite3` and running the old application against the untouched
legacy files. After manual legacy deletion, rollback requires restoring the
workspace backup.

## Runtime workspace ownership

### Workspace lease

The phase-2 `SessionRepository` owns one workspace lease for its complete
lifetime. It acquires the fixed companion
`workspace/sessions.sqlite3.cha-lock` before creating, opening, migrating the
schema of, or inspecting the workspace database. It releases the lease only
after the HTTP server and all live-session actors have stopped.

A companion file is preferable to locking the database bytes directly because
it:

- exists before a new database is created;
- does not interfere with SQLite's own byte-range locking;
- stays stable while SQLite sidecars appear or disappear; and
- behaves consistently across Unix and Windows.

Acquisition is non-blocking. A second process fails before binding its HTTP
listener and reports that the workspace is already in use. A forced process
exit releases the kernel lock automatically; a stale empty companion file is
harmless.

The existing lease helper's default error derives a session name from the
database filename, which would incorrectly report a session named `sessions`.
The helper therefore accepts an explicit conflict description for workspace
leases. Migration mode and phase-2 startup both pass
`Workspace already in use: '<workspace-root>'`; neither uses the default
session-oriented message.

### SQLite connections

Each persistent live-session actor owns one SQLite connection to the workspace
database. The Welcome actor similarly owns one connection to its private
temporary database. Each connection remains confined to its actor thread and
may continue using `SQLITE_OPEN_NOMUTEX`.

Every `SessionRepository` catalog or maintenance operation opens a short-lived
connection and closes it before returning. The repository owns no SQLite
connection and needs no connection mutex. Its methods can therefore remain
`const`, matching the existing `shared_ptr<const SessionRepository>` ownership.
No SQLite handle is shared concurrently between threads.

Every connection enables:

```sql
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
```

The phase-2 runtime initializes the database with:

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
```

Every transaction that can write starts with `BEGIN IMMEDIATE` before issuing
any read. This includes journal transitions, create, rename, archive, forum
synchronization, schema migration, and the migration target transaction. The
existing `Transaction` helper already has this behavior and is reused by the
workspace implementation. Read-only transactions may remain deferred.

Starting write-capable transactions as deferred and reading before the first
write is unsafe with several actors: another writer can invalidate the reader's
WAL snapshot, and the later upgrade fails with `SQLITE_BUSY_SNAPSHOT` without
being retried by `busy_timeout`. `BEGIN IMMEDIATE` acquires the write intent at
the start, where ordinary busy handling can wait.

WAL allows catalog and history readers to proceed while a journal connection
writes. SQLite still serializes writers. This is acceptable because CHA allows
at most eight live sessions by default and journal transactions cover only
short durable transitions. A provider request never holds a transaction while
waiting for model output.

## Runtime session operations

Every operation resolves `(forum_id, session_id)` to `session_key`. Every
journal statement includes `session_key`; omitting it is a cross-session data
isolation defect because request and entry IDs remain session-local.

### Forum synchronization

At repository initialization and after a successful candidate workspace load,
CHA inserts every configured persistent forum ID with `INSERT OR IGNORE`.
Opening or creating a session still requires the forum to exist in the
currently published filesystem `Workspace`; a row in `forums` is not
sufficient.

Removing a forum directory does not delete its database row or sessions. They
become inaccessible through ordinary APIs but remain durable. Reintroducing the
same forum ID makes its active sessions visible again. Renaming a forum is
equivalent to removing one ID and adding another; sessions do not move
automatically.

### Create

Creation validates the current workspace forum, resolves its `forum_key`, and
inserts a `sessions` row with initial counters and history epoch in one
transaction. A unique-key collision causes the timestamp-based ID generator to
try its next suffix.

Active and archived sessions share the same unique constraint, so an archived
ID cannot be reused.

`updated_at` is an explicit Unix timestamp in seconds. Creation, transcript
journal changes, clear, rename, and archive update it in the same transaction
as the corresponding durable change.

### List and inspect

Forum listings query `sessions` by `forum_key` with
`archived_at IS NULL`. Workspace Recent is one database query joined with
`forums` and filtered to the currently published workspace forums.

`StoredSession` no longer exposes a per-session database path or filesystem
timestamp. It carries public identity, label, and integer `updated_at`.

### Open and restore

Open reads session metadata, counters, current-epoch entries, and interrupted
turns in one consistent read transaction. `PreparedSession` carries the
selected database path, `session_key`, public identity, label, and restore
state. Persistent sessions select the workspace database; Welcome selects its
private temporary database. It does not hold a per-session operating-system
lease.

`LiveSessionManager` remains the in-process authority preventing two actors
from owning the same public identity. The workspace lease supplies
cross-process exclusion.

Interrupted-turn lookup is scoped to one `session_key`. A started turn in one
session cannot affect another session's restore.

### Journal

`SessionJournal` receives the prepared database path and `session_key`. Every
write includes the key and updates the session's `updated_at` before commit.
This is the same journal construction path for persistent and Welcome
sessions.

Start and terminal transitions remain separate short transactions. Provider
execution occurs between them without an open SQLite transaction. Clear
increments only the selected session's `history_epoch`; rows from previous
epochs remain durable as they do today.

### Rename

Rename updates one active session's `label` and `updated_at`. A live rename
continues through its actor owner queue so database state, descriptor,
snapshots, and SSE consumers change in order. An inactive rename uses a
repository transaction.

### Cross-session durable-history read

The repository exposes a durable-history read distinct from restore. It returns
one active session's current-epoch entries in `entry_id` order.

It does not synthesize an interrupted-turn error for a started turn because the
target session may currently be live and generating. Durable history also does
not contain partial streaming response text. A feature requiring the latest
in-memory partial response must obtain a live actor snapshot instead.

This repository operation is part of phase 2 and satisfies the storage-level
cross-session access requirement. Adding a browser route, selection UI, or new
authorization policy on top of it is outside this design.

## Recoverable deletion

Normal Delete archives a session in place:

```sql
UPDATE sessions
SET archived_at = ?1,
    updated_at = ?1
WHERE session_key = ?2
  AND archived_at IS NULL;
```

The update must affect exactly one row. Turns and entries remain in their
existing tables. Separate active and archive table sets are deliberately
avoided.

The live deletion sequence remains:

1. Reserve the public identity in `LiveSessionManager`.
2. Reject new open and reattach attempts for that identity.
3. Stop and join an existing actor under the delete deadline.
4. Ensure its controller and journal connection are destroyed.
5. Archive the session in one repository transaction.
6. Release the maintenance reservation.

A failed transaction leaves the session active and visible. A successful
transaction excludes it from list, inspect, open, rename, and ordinary history
queries. The unique identity remains reserved.

A future restore operation can clear `archived_at` after validating the
forum. A future permanent purge can delete the `sessions` row and rely on
`ON DELETE CASCADE`. Neither operation is part of this change.

## Temporary Welcome session

Welcome remains process-local and is excluded from the workspace database,
offline migration, forum synchronization, and persistent backup semantics.

It always uses a private temporary SQLite database in the repository-owned
temporary directory outside the workspace. It must not use plain `:memory:`:
the connection that initializes such a database and the later actor connection
would see different databases.

The temporary database uses the exact workspace application ID, schema
version, and schema. Initialization inserts one `forums` row for the built-in
Entrance forum and one active `sessions` row for Welcome. Preparation returns
that database's path and Welcome's `session_key`, so
`SessionJournal(path, session_key)` and all restore/journal SQL use the same
code and schema as persistent sessions. The repository removes its private
temporary directory at destruction. Welcome remains non-renamable and
non-archivable.

## Backup, transport, and workspace reload

### Backup and transport

The supported manual transport procedure is:

1. Stop CHA cleanly.
2. Copy or archive the complete workspace.
3. Start CHA against the copied workspace.

Copying only `sessions.sqlite3` moves conversations but not forum definitions,
characters, personas, or provider settings. The complete workspace remains the
application portability unit.

The current workspace backup stops live sessions before creating its tar
archive. The phase-2 repository additionally completes its transactions and
runs:

```sql
PRAGMA wal_checkpoint(TRUNCATE);
```

The resulting archive contains a self-sufficient main database. Copying a live
database with ordinary filesystem operations is unsupported; a future live
backup must use `sqlite3_backup` or `VACUUM INTO`.

### Workspace reload

`POST /api/v1/workspace/reload` retains its global maintenance boundary:

1. Block new session opens.
2. Stop and join every live actor.
3. Checkpoint the workspace database.
4. Create the workspace backup.
5. Load and validate the candidate filesystem workspace.
6. Insert newly configured forum IDs.
7. Publish the candidate workspace.
8. Release the reload reservation.

Candidate failure leaves the old workspace published. Removing a forum from
the candidate never deletes its sessions.

## Component changes

### Phase 1

- Application argument parsing gains temporary `--migration` mode.
- Workspace database schema creation and validation are added as reusable
  storage components.
- A read-only legacy importer copies complete session databases into a supplied
  workspace database transaction.
- Target publication reuses the existing `archive_without_replacement()`
  helper and its WSL/9p fallback.
- Normal `SessionRepository`, `SessionCatalog`, `SessionJournal`, and
  per-session leases remain unchanged.

### Phase 2

- `SessionRepository` owns the workspace database path and process-lifetime
  workspace lease, and opens short-lived connections for repository operations.
- Filesystem catalog operations become indexed database queries.
- `SessionJournal` uses the prepared database path plus `session_key`.
- `PreparedSession` carries the selected database path and `session_key`
  instead of a per-session lease.
- `StoredSession` carries integer `updated_at` instead of a filesystem path
  and modification time.
- Production per-session `SessionLease` is replaced by the workspace lease.
- Welcome uses a private temporary database with the workspace schema.
- Physical archive helpers are removed from normal deletion.
- Public HTTP routes and response shapes remain unchanged.

The inactive-session Markdown download reads the selected session from the
workspace database. A live download continues to use its actor snapshot.

### Phase 3

- Remove `--migration`, legacy database readers, source validation adapters,
  import code, and migration-only tests.
- Retain read-only legacy-layout detection, its startup safety tests, workspace
  schema creation/validation, and all other phase-2 runtime code.

## Error behavior

### Migration mode

| Condition | Behavior |
| --- | --- |
| Workspace fails validation | Exit nonzero without target publication |
| Workspace database already exists | Refuse to overwrite it |
| Workspace lease held | Exit because another new process owns the workspace |
| No legacy databases found | Exit without creating an empty target |
| Legacy database invalid or unsupported | Name source and exit |
| Legacy identity mismatches its path | Name source and exit |
| Duplicate active/deleted identity | Name both sources and exit |
| Import, count, or integrity validation fails | Roll back target and exit |
| Atomic target publication fails | Leave sources unchanged and report target state |

### Final runtime startup guard

| Condition | Behavior |
| --- | --- |
| Workspace lease held | Fail startup before binding HTTP |
| Workspace database identity/version invalid | Fail without modifying it |
| Workspace database absent and legacy files present | Fail; in phase 2 run offline migration, and in phase 3 use the archived migration build |
| Workspace database and legacy files both present | Fail; manual cleanup required |
| New workspace has neither format | Create an empty workspace database |

The legacy-layout detector behind the last three rows is retained permanently.
It performs only the cheap path scan defined under Source discovery. Legacy
database contents are never opened by the final runtime.

Runtime SQLite failures retain the current durable-state rule: in-memory state
is updated only after persistence succeeds, and an actor terminates when it can
no longer guarantee journal consistency.

## Testing strategy

### Migration command tests

- Refuse an existing target without modifying it.
- Refuse an empty source set without creating a target.
- Import active sessions from several forums.
- Import deleted sources as archived sessions.
- Allow the same session ID in different forums.
- Reject duplicate public identities.
- Preserve labels, counters, history epochs, turns, entries, timestamps, and
  interrupted turns.
- Copy rows from earlier history epochs.
- Import each explicitly supported legacy schema version read-only.
- Reject invalid databases, unsupported versions, and path/metadata identity
  mismatches.
- Fail the complete import when any one source is invalid.
- Leave every legacy database byte-for-byte unchanged on success and failure.
- Publish no target after transaction, count, integrity, or disk failure.
- Refuse concurrent migration through the workspace lease.
- Print target-derived summary counts and exit status 0 on success.
- Start no HTTP listener, provider, actor, or Welcome session.

### Schema and runtime storage tests

- Create two sessions with the same session ID in different forums.
- Reject duplicate IDs within one forum, including archived sessions.
- Enforce one started turn per session while allowing simultaneous started
  turns in different sessions.
- Keep request/entry counters and history epochs independent.
- Scope restore, journal, clear, rename, archive, and history SQL by
  `session_key`.
- Verify `updated_at` changes in the same transaction as each durable change.
- Validate foreign keys and complete database integrity.
- Initialize Welcome with one forum and one session in the workspace schema,
  then restore and journal through a separately opened actor connection.

### Repository, archive, and history tests

- List only active sessions belonging to the requested forum.
- Build Recent across forums without filesystem database scans.
- Archive inactive and live sessions and retain all child data.
- Exclude archived sessions from normal operations.
- Prevent reuse of archived identities.
- Preserve sessions when their forum is removed from configuration.
- Read one session's durable history while another is active.
- Do not interpret another live session's started turn as interrupted.

### Concurrency and process tests

- Run several live journals against the same WAL database.
- Verify every write-capable path begins with `BEGIN IMMEDIATE` and a competing
  writer waits within `busy_timeout` instead of failing a deferred snapshot
  upgrade.
- Read history while another session commits.
- Verify no data crosses session boundaries.
- Reject a second process using the same workspace.
- Make the lease conflict identify the workspace path rather than a session
  named `sessions` in migration and normal startup.
- Release the workspace lease after clean and forced exits.
- Recover WAL and reopen sessions after forced termination.

### Cutover and web tests

- Enforce all four phase-2 startup format combinations.
- Checkpoint WAL before workspace backup.
- Preserve list, open, Markdown download, rename, delete, reload, and restart
  HTTP behavior.
- Keep live Markdown download on the actor snapshot path.

## Implementation sequence

### Phase 1

1. Define the workspace schema and validation API.
2. Implement read-only adapters for supported legacy database versions.
3. Implement complete legacy-to-workspace row copying.
4. Add target count, foreign-key, and integrity validation.
5. Reuse `archive_without_replacement()` to publish the validated target.
6. Add the temporary `--migration` execution path and console summary.
7. Test success, all-or-nothing failure, and source immutability.

### Manual cutover

1. Stop CHA and back up the workspace.
2. Run migration and verify its summary.
3. Remove legacy databases and sidecars manually.
4. Build or install phase 2 without restarting the old runtime.

### Phase 2

1. Add the process-lifetime workspace lease, explicit workspace conflict
   message, and startup format checks.
2. Replace catalog and repository operations with short-lived workspace SQL
   connections using `BEGIN IMMEDIATE` for write-capable transactions.
3. Change prepared-session and journal wiring to database path plus
   `session_key`, and give Welcome a workspace-schema temporary database.
4. Replace physical deletion with transactional archival.
5. Add the repository-level cross-session durable-history read.
6. Adapt backup checkpointing and tests.
7. Remove obsolete production per-session storage code.

### Phase 3

1. Remove the migration command, legacy readers, and import code.
2. Remove migration-only tests and CLI documentation.
3. Retain the read-only legacy-layout detector, incomplete-cutover tests, final
   workspace database documentation, and runtime tests.

## Acceptance criteria

### Migration phase

- `cha --migration` creates one complete `sessions.sqlite3` from every
  valid active and deleted legacy session.
- It never modifies or removes a legacy session database.
- It refuses an existing target.
- It refuses to publish an empty target when no legacy source exists.
- Any source or target error produces no published database.
- The success summary is computed from and validated against the committed
  target.
- The operator can retry after removing only an unpublished generated target.

### Final runtime

- A new workspace creates one persistent session database and one fixed lease
  companion.
- Creating sessions creates no additional persistent SQLite or lease files.
- Welcome uses one private temporary database with the workspace schema and
  the same journal path as persistent sessions.
- Every stored session has an enforced forum relationship and retains its
  existing public URL identity.
- Sessions can be listed by forum, and the repository can read one session's
  durable history while another session is active, through the one database.
- The const repository uses short-lived connections; live actors never share a
  SQLite connection.
- Every write-capable transaction begins with `BEGIN IMMEDIATE`.
- Multiple live actors journal without cross-session data leakage.
- A second CHA process cannot open the same workspace.
- Startup refuses an incomplete manual cutover.
- Delete archives a session transactionally without moving transcript rows.
- Backup, reload, restart, Markdown download, rename, and
  interrupted-turn behavior remains valid.
- The final code retains read-only legacy-layout detection but contains no
  legacy database reader, automatic migration, or cleanup machinery.
