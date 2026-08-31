# Unified CHA database design

Status: Proposed

## Summary

CHA currently has two persistent authorities:

- workspace configuration is a directory tree of TOML, Markdown, and `.env`
  files; and
- sessions are stored in one SQLite workspace database.

This design makes the SQLite database the only persistent authority for both
kinds of data. The existing session tables remain unchanged. A new `config`
table stores every configuration file as a workspace-relative name and its
text content:

```sql
CREATE TABLE config (
    name TEXT PRIMARY KEY CHECK (
        name <> ''
        AND substr(name, 1, 1) <> '/'
        AND instr(name, char(92)) = 0
        AND instr('/' || name || '/', '/../') = 0
    ),
    content TEXT NOT NULL
) STRICT;
```

There is no configuration generation, revision, type, or control table.
SQLite transactions already make replacement of the complete configuration
atomic.

The existing `Workspace::load(directory)` implementation remains the one
workspace parser. At runtime, CHA materializes the committed `config` rows into
a private temporary directory and loads that directory with the existing
parser. This avoids implementing a second directory-discovery, TOML,
Markdown, and template-expansion path for SQLite.

Configuration is edited outside CHA by exporting it to a directory, editing
the files, stopping CHA, and importing the directory again. The few narrow
configuration changes already supported by the running application continue
to work: character provider/style selection and forum default character/persona
selection update the materialized files, validate them with `Workspace::load`,
and transactionally replace the `config` rows.

### Relationship to `session-design.md`

This document supersedes the storage layout, per-session lease, physical
deletion/relocation, and database-lifecycle sections of
`docs/session-design.md`. In particular, sessions now live as rows in the
unified workspace database and deletion is represented by the current archived
session state; no per-session database is moved to a `deleted/` directory.

The user-facing rename/delete behavior, session-label policy, actor-aware
coordination, and live-session maintenance concepts in `session-design.md`
remain applicable where they agree with the current code. Implementation must
also add a short notice to that document pointing here for the superseded
storage sections, so the two documents cannot be read as competing layouts.

## Goals

- Store all durable session and configuration data in the same SQLite
  database.
- Make the database selected by `--data` sufficient to start CHA.
- Preserve every existing session when importing configuration into the
  current version-1 workspace database.
- Make import and the existing runtime configuration mutations atomic.
- Provide simple directory export/edit/import configuration maintenance.
- Reuse the existing workspace parser and its validation behavior.
- Preserve the existing character provider/style and forum default
  character/persona behavior.
- Keep API keys in the database, SQLite sidecars, and materialized `.env`
  private to the operating-system user.
- Refuse import from a workspace that still contains legacy per-session
  databases, preventing a configuration import from silently publishing an
  empty session database.
- Keep the implementation small, direct, and readable.
- Stop using the old workspace directory as a runtime source of truth.

## Non-goals

- Running CHA directly from a workspace directory.
- Backward-compatible support for `--workspace` or for `--config` naming an
  `app.toml` file.
- Online import or export while the service is running.
- Retaining configuration history or multiple configuration generations.
- Adding a general configuration editor, SQL editor, or new configuration UI.
- Storing executable files, static web assets, logs, SQLite sidecars, lock
  files, or temporary Welcome-session data in the `config` table.
- Eliminating SQLite's `-wal` and `-shm` sidecars while the application runs.
- Supporting multiple CHA processes against one database.
- Preserving the directory archive created by the old workspace-reload route.

## Terminology and authority

**CHA database** is the SQLite file passed to `--data`. It contains the
session schema and the `config` table.

**Configuration source directory** is the directory passed to `--import`.
It is read only for the duration of import and is never used by normal runtime.

**Configuration export directory** is the directory passed to `--export`.
It contains an editable representation of the committed `config` rows.

**Process-private root** is the one private temporary directory owned by a
normal CHA process. It contains `workspace/` for materialized configuration and
`welcome/` for the temporary Welcome-session database.

**Materialized workspace** is the `workspace/` child created from the committed
`config` rows. It is an implementation detail, not a durable source of truth.

**Database lease** is the existing operating-system lock held on
`<database>.cha-lock`. Runtime, import, and export all use the same lease.

The authority rule is simple: committed SQLite content wins. An exported
directory changes nothing until it is successfully imported. Materialized
files are allowed to differ from SQLite only while one serialized runtime
mutation is being validated and committed.

## Command line

`--data` is mandatory in every mode and names the CHA database. The old
`--config` option, which named an `app.toml` file, is removed.

Normal runtime:

```text
chaweb --data DATABASE [--root PATH] [--host HOST] [--port PORT]
```

Offline import:

```text
chaweb --data DATABASE --import DIRECTORY
```

Offline export:

```text
chaweb --data DATABASE --export DIRECTORY
```

The hidden `--test-idle-grace-ms` argument remains available in normal runtime
for browser process tests.

The command-line rules are:

- `--import` and `--export` are mutually exclusive.
- Both options require `--data`.
- Import and export acquire the same non-blocking database lease as runtime.
  A running service therefore makes either operation fail immediately with a
  clear "database is in use" error.
- Import and export do not bind a port or start any providers or HTTP threads.
- `--root`, `--host`, `--port`, and `--test-idle-grace-ms` are runtime options
  and are rejected with import or export rather than silently ignored.
- `--workspace` is removed.
- Database and directory paths are resolved to absolute normalized paths
  before use.

`--root` continues to identify installed application assets such as `web/`.
It is not workspace data and defaults to the executable directory as it does
today.

## Stored configuration tree

The configuration tree includes:

- required `app.toml`;
- required `workspace.toml`;
- all regular files whose names end in `.toml`;
- all regular files whose names end in `.md`; and
- optional root `.env`.

Files with other names are not imported. In particular, the database itself,
its `-wal` and `-shm` sidecars, its `.cha-lock` file, logs, backups, and editor
temporary files are not configuration.

All matching TOML and Markdown files are stored, including files not directly
known to the current parser. This preserves template includes whose targets
are themselves stored configuration files and permits the workspace file
vocabulary to grow without changing the storage schema.

Template includes can technically name a regular file with any extension, but
the durable configuration set intentionally contains only TOML, Markdown, and
root `.env`. An include target must therefore be in that set. Import validates
the collected rows after materialization, so an include such as
`shared/snippet.txt` is rejected before commit rather than succeeding during
import and failing at the next startup. Rename such an included text file to a
stored `.md` name or move its contents into a stored configuration file.

Names are stored relative to the configuration root with `/` separators:

```text
app.toml
workspace.toml
.env
system/providers/openai/config.toml
characters/rus/shurik/CHARACTER.md
forums/lobby/members/shurik/character.toml
```

There is no artificial `root/` prefix. The table belongs to one logical root,
so such a prefix would be redundant.

Import and every database read validate stored names before using them. A name
must:

- be non-empty and relative;
- use `/` as its separator;
- contain no empty, `.` or `..` component;
- contain no backslash;
- end in `.toml` or `.md`, or equal `.env`; and
- be unique, as enforced by the primary key.

Import does not follow symbolic links. A symbolic link that would otherwise
match the import set is rejected. This keeps names relative to the selected
root and avoids platform-dependent link behavior.

The contents are read and written as text without newline conversion. Syntax
and template validation for files consumed by the current application is
performed by the existing parsers, not by the table layer. Unknown TOML and
Markdown files are preserved but are not assigned new validation semantics.

### Empty directories

The table stores files, not directories. Before writing rows, the materializer
and exporter create the small required workspace skeleton:

```text
system/providers/
personas/
characters/
forums/
```

Every other meaningful directory is implied by a stored file path. Optional
empty directories do not carry configuration and need not round-trip.

## Application configuration

`app.toml` moves into the stored configuration tree. It contains the settings
needed to run the web service:

```toml
host = "127.0.0.1"
port = 8080
```

`--host` and `--port` remain runtime overrides. Import requires `app.toml` and
validates its stored values so that the database can start without extra
configuration arguments.

The old `workspace` setting is removed because the database itself identifies
the workspace. The old `backup_dir` setting is removed with directory-based
workspace reload and backup.

The optional `.env` file is stored in `config`. Startup applies it after
materialization, preserving the current rule that inherited process
environment variables take precedence.

Import must also make its values visible while `Workspace::load` validates
providers: provider validation reads configured API-key variables through the
process environment. Import therefore parses the materialized `.env`, applies
only variables that are absent from the inherited environment as a scoped
overlay, validates the workspace, and restores the previous environment when
validation ends. This gives import and runtime the same provider-key lookup
semantics without leaking import-only environment changes into an in-process
caller or later test.

This requires separating dotenv parsing from application in the existing
utility and adding a small scoped application helper. The parser must preserve
the current ordering and non-overwrite behavior, including repeated names and
inherited variables whose values are empty.

`workspace.toml` continues to configure logging. A relative logging path is
resolved against the parent directory of the CHA database, not the private
materialization directory. Absolute paths retain their current meaning.

## Secret-bearing file permissions

Once `.env` is stored in SQLite, the main database, its WAL/SHM sidecars, and
the materialized tree all contain secrets. They must not be created with the
bundled SQLite default mode of `0644`.

On POSIX systems the rules are:

- Compile bundled SQLite with `SQLITE_DEFAULT_FILE_PERMISSIONS=0600`. A new
  database is therefore owner-readable and owner-writable from the instant
  SQLite creates it, rather than being created broadly and tightened later.
- After acquiring the CHA lease and before opening an existing database,
  change the main database and any existing `-wal`, `-shm`, or `-journal`
  sidecar to `0600`, then verify the result. This happens before version-1
  import writes `.env` into the database. Failure to secure any existing file
  is fatal.
- Reject a database or sidecar path that exists but is not a regular file.
- SQLite creates WAL, rollback-journal, and SHM files with the associated main
  database's mode on POSIX. Keeping the main database at `0600` therefore makes
  newly recreated sidecars `0600`; runtime verifies the existing sidecars
  again after WAL initialization rather than relying only on that behavior.
- Create one normal-runtime process-private root atomically with
  `mkdir(path, 0700)`, then create its `workspace/` and `welcome/` children
  inside that already-private root. Import creates its own short-lived private
  validation root the same way. Do not call a default-permission
  `create_directory` followed by `permissions`, because that exposes a window
  in which another local user can traverse the directory.
- Create exported `.env` atomically with mode `0600`; do not write it with a
  default mode and chmod it afterward.

Files under the materialized workspace and the temporary Welcome database need
no separate directory-level protection because the process-private root is not
traversable by another user. The existing companion lock is already created
with mode `0600`.

On Windows, owner-only means an equivalent private security descriptor/DACL
rather than POSIX mode bits. Database creation, existing-file tightening,
sidecars, materialization directories, and exported `.env` must not grant
ordinary access beyond the current user and the operating-system principals
needed to administer that user's files. Failure to establish the platform's
private permissions is fatal rather than a warning.

Permission enforcement is performed by a small platform helper used by the
top-level runtime/configuration owner. `SessionRepository` receives its already
private `welcome/` child instead of creating a second random temporary
directory. This closes the existing create-then-chmod window and leaves one
cleanup path and at most one orphaned runtime root after a crash.

## Database schema and version

The existing workspace database application ID is retained. The schema version
advances from `PRAGMA user_version = 1` to `2`.

Version 2 adds `config` to the existing session schema:

```sql
CREATE TABLE config (
    name TEXT PRIMARY KEY CHECK (
        name <> ''
        AND substr(name, 1, 1) <> '/'
        AND instr(name, char(92)) = 0
        AND instr('/' || name || '/', '/../') = 0
    ),
    content TEXT NOT NULL
) STRICT;
```

The table check is defense in depth for the highest-risk path forms. It rejects
an absolute POSIX name, every backslash, and any `..` path component even if a
future caller bypasses application validation. Application validation remains
authoritative for the complete portable-name policy, including empty and `.`
components, supported filenames/extensions, normalized separators, and
platform-specific absolute-path forms.

The database validator treats `config` as a required table. Startup also
requires at least `app.toml` and `workspace.toml`, materializes all rows, and
validates the resulting application and workspace configuration.

There is deliberately no:

- `generation` column;
- `type` column;
- `config_control` table; or
- durable revision counter.

SQLite transactions already give a set-wide transition. Within one
transaction, deleting all old rows and inserting all new rows is invisible to
other connections until commit. They observe either the complete old set or
the complete new set.

`PRAGMA user_version` describes the database schema only. It is not changed
when configuration content changes.

## Database creation and upgrade

Normal runtime does not create or upgrade a database. The operator must run
`--import` first.

The supported states are:

| Mode | Database state | Behavior |
| --- | --- | --- |
| import | missing, source has no legacy session databases | create the complete version-2 database and import configuration |
| import | valid version 1, source has no legacy session databases | add `config`, preserve all sessions, import configuration, then set version 2 |
| import | valid version 2, source has no legacy session databases | replace configuration and preserve all sessions |
| import | source contains legacy session databases, target missing | fail; archived session migration was never run |
| import | source contains legacy session databases, target present | fail; migration cleanup is incomplete |
| import | another ID/version or invalid contents | fail without modifying the database |
| export | valid version 2 | export the committed configuration |
| export | missing/version 1/invalid | fail |
| runtime | valid version 2 | start |
| runtime | missing/version 1/invalid | fail and instruct the operator to run import |

The version-1 upgrade is supported only by import. This is the intentional
one-time bridge that preserves current sessions; it is not support for running
from the old directory configuration.

Import retains the existing path-only legacy-session detector and runs it
against the source directory before creating or modifying the target database.
It scans `forums/*/sessions/` and `forums/*/sessions/deleted/` for regular
`.sqlite3` files without opening or changing them. If it finds any, import
fails with the existing actionable distinction:

- a missing target database means the archived per-session-to-workspace
  migration was never run; and
- a present target database means legacy cleanup after migration is
  incomplete.

Runtime cannot perform this check because it intentionally has no durable
workspace directory. Import is the correct boundary: it is the only operation
that converts an old workspace tree into the new authority and the only place
where silently creating an empty session database would hide legacy sessions.

For a version-1 database, `CREATE TABLE config`, all configuration inserts, and
the final `PRAGMA user_version = 2` execute in one transaction. A failure rolls
back to a valid version-1 database.

For a missing database, import creates the existing session tables and the new
configuration table using the authoritative schema creation code. If initial
creation fails, it removes the newly created database and SQLite sidecars so a
later import can retry cleanly.

## Database lease and WAL

The existing companion lock path and operating-system locking implementation
remain unchanged:

```text
DATABASE.cha-lock
```

The lease must move to the top-level database/configuration owner. It is
acquired before permission enforcement, schema inspection, configuration
materialization, dotenv loading, or workspace parsing. Runtime retains it until
all providers, sessions, logging, and materialized files have shut down.

`SessionRepository` no longer owns or acquires a second lease. It receives the
already-locked database path from the composition root.

The lock file is intentionally separate from SQLite locking. SQLite coordinates
connections within the one CHA process; the companion lease enforces the
application rule that runtime, import, and export cannot overlap.

SQLite continues to use WAL mode. `DATABASE-wal` and `DATABASE-shm` may exist
while CHA runs. They are storage-engine sidecars, not a second application data
authority. Existing checkpoint behavior on graceful shutdown remains.

## Import

Import is an offline, all-or-nothing configuration replacement.

The operation is:

1. Parse and validate command-line paths.
2. Require the source to be a directory and the database parent to exist.
3. Acquire `DATABASE.cha-lock` non-blockingly.
4. Run the path-only legacy-session detector against the source. If it finds a
   legacy database, fail with the migration-required or cleanup-incomplete
   message based on whether the target database exists.
5. Recursively collect matching regular files in deterministic name order.
6. Reject unsafe names, matching symbolic links, duplicate normalized names,
   and a missing `app.toml` or `workspace.toml`.
7. Create a fresh atomically private validation root with a `workspace/`
   child, then materialize the collected rows there with the same materializer
   used by normal runtime.
8. Parse materialized `app.toml` and `.env`.
9. Apply materialized `.env` entries as a scoped, non-overwriting environment
   overlay.
10. Call `Workspace::load(materialized_root, database_parent)` to validate the
   exact file set that runtime will load while resolving durable relative
   output paths correctly.
11. Restore the environment overlay after workspace validation.
12. Secure an existing database and sidecars to owner-only access.
13. Open and validate the database, create it privately if absent, or prepare
    the version-1 schema upgrade.
14. Start an immediate SQLite transaction.
15. Create `config` when upgrading version 1, or delete every existing
    version-2 `config` row.
16. Insert the collected `(name, content)` rows with bound parameters.
17. Set `user_version = 2` when creating or upgrading.
18. Commit, verify any SQLite sidecar permissions, and report the number of
    imported files.

Candidate validation precedes any target database open, permission change, or
content write. It runs against a byte-identical materialization of the rows
that will be committed, not against the source directory. Source-only files
excluded by the import rules therefore cannot accidentally satisfy template
includes or other filesystem reads.

Malformed application or consumed workspace TOML, an absent or excluded
template include, a missing required character, a bad dotenv entry, a missing
provider key after `.env` overlay, or another workspace error leaves the
existing database untouched.

The source directory may contain the target SQLite database, as the current
workspace layout normally does. It is safe because only `.toml`, `.md`, and
root `.env` are collected.

Import never changes, deletes, or recreates session rows in an existing
database. Forum synchronization occurs during the next normal startup after
the imported workspace has been loaded.

## Export

Export writes only configuration; it is not a session or database backup.

The operation is:

1. Resolve paths and acquire `DATABASE.cha-lock`.
2. Secure the existing database and sidecars to owner-only access.
3. Open and completely validate the version-2 database.
4. Read all `config` rows ordered by `name`.
5. Validate every name before creating output.
6. Require the destination to be absent or an existing empty directory.
7. Create the destination, the known workspace skeleton, and required parent
   directories.
8. Write every row without newline conversion, creating `.env` atomically with
   owner-only permissions.
9. Report the number of exported files.

Export never overwrites an existing file. A non-empty destination fails before
any write. If an I/O failure occurs after output begins, the database is
unaffected; the command reports that the destination may be incomplete and
must be emptied before retrying.

## Runtime startup

Normal startup follows this order:

1. Parse the command line and require `--data`.
2. Acquire the process-lifetime database lease.
3. Secure the existing database and SQLite sidecars to owner-only access.
4. Validate the database identity, version-2 schema, and contents, initialize
   WAL, and verify sidecar permissions.
5. Create one process-private temporary root atomically with owner-only access,
   then create its `workspace/` and `welcome/` children.
6. Materialize the committed `config` rows and known workspace skeleton under
   `workspace/`.
7. Load and apply `.env` without overwriting inherited variables.
8. Load stored `app.toml` and apply `--host` and `--port` overrides.
9. Call `Workspace::load(materialized_root, database_parent)`.
10. Initialize logging from the loaded workspace.
11. Construct `SessionRepository` with the explicit database path, the stable
    materialized root, and the private `welcome/` directory for its temporary
    database.
12. Synchronize configured forums and start providers and the web server.

The process-private root stays alive until shutdown. `Workspace` eagerly owns
all parsed values, but its existing narrow write methods retain paths to
character and forum TOML files. Keeping one stable `workspace/` child permits
those methods and template validation to remain unchanged. The `welcome/`
child contains the process-local session database that `SessionRepository`
already uses.

The root owner removes the complete tree recursively on ordinary shutdown,
after `SessionRepository` has closed the Welcome database. A crash may leave
one orphaned private root in the system temporary location. It contains a copy
of configuration already present in the database and the non-durable Welcome
database, and can be removed manually or by ordinary temporary-directory
cleanup.

No normal runtime read consults the old imported or exported directory.
Deleting or changing that directory after import cannot affect the service.

## Workspace loading and relative paths

`Workspace::load` remains the only implementation of:

- workspace directory discovery;
- TOML parsing;
- Markdown loading;
- character, persona, provider, style, and forum validation;
- prompt variable overlay;
- template include expansion and containment; and
- construction of the immutable published workspace.

It gains a small overload or additional argument separating two concepts:

- `root`: the physical materialized directory used to find configuration and
  template files; and
- `relative_path_base`: the durable base used for configured output paths such
  as `logging.file`.

Template includes continue to resolve and enforce containment under the
materialized configuration root. Only durable output paths use the database
parent as their base.

`Workspace::root()` continues to return the materialized root. This minimizes
changes to its file writers and existing identity checks. `SessionRepository`
therefore receives both the materialized root and explicit database path
instead of deriving the database path from `Workspace::root()`.

## Runtime configuration mutations

The following existing mutations remain supported:

- changing a non-built-in character's provider or style;
- changing a forum's default character; and
- changing a forum's default persona.

They continue to use `Workspace::write_character_settings`,
`write_forum_default_character`, and `write_forum_default_persona`. Those
functions write the materialized TOML files exactly as they do today.

A single process-local configuration mutex serializes the complete mutation
workflow. The application-level database lease does not serialize threads in
one process, and SQLite write serialization alone would not order publication
of two candidate `Workspace` objects.

Under that mutex, a runtime mutation performs:

1. Obtain the current published workspace.
2. Apply one existing narrow file writer to the materialized tree.
3. Load and validate a complete candidate with `Workspace::load`.
4. Collect the complete materialized configuration file set.
5. In one SQLite transaction, delete all `config` rows and insert the complete
   candidate set.
6. Commit the transaction.
7. Publish the candidate through the existing `loadws(Workspace)` overload.
8. Request the same affected live-session reloads as the current behavior.

Replacing the complete configuration is intentional. The configuration is
small, and one shared implementation for import and runtime commit is easier
to reason about than changed-file tracking.

If file rewriting, candidate validation, row collection, or database commit
fails before commit, the published workspace and committed database remain
unchanged. The store rematerializes the committed `config` rows to discard the
failed temporary edits, then propagates the error through the existing caller.

If the process terminates, or candidate publication allocation fails, after the
SQLite commit but before publication, the database still contains one complete
valid configuration. The next startup loads it. This rare safe recovery
boundary does not justify another publication overload solely to move the
existing `shared_ptr` allocation ahead of the commit.

Runtime mutation does not reload `.env` or `app.toml`. There is no runtime UI
for either file, and external changes require offline import followed by a
normal start.

## Workspace reload and backup removal

`POST /api/v1/workspace/reload` is removed. There is no persistent directory
to reload, offline import cannot run while the database lease is held, and
runtime mutations validate and publish their own candidate workspace.

The reload route's directory `.tar.gz` backup is also removed, together with:

- `backup_dir` application configuration;
- `WorkspaceBackup` production code;
- reload-specific route wiring and tests; and
- the reload endpoint and client call in the OpenAPI/web client sources.

Backing up the unified database is a separate concern. Copying a live WAL
database naively is unsafe, so a future database backup command should use the
SQLite backup API or an offline copy. It is not part of this change.

## Failure and crash consistency

The important failure boundaries are:

| Failure | Durable result |
| --- | --- |
| import finds legacy session databases | target database is not created or changed |
| import validation fails | old database and configuration unchanged |
| database/sidecar permissions cannot be secured | no configuration containing secrets is committed |
| version-1 upgrade/import transaction fails | valid version-1 database remains |
| version-2 replacement transaction fails | old complete configuration remains |
| runtime candidate validation fails | database and published workspace remain old; temporary tree is restored |
| runtime database commit fails | database and published workspace remain old; temporary tree is restored |
| process dies during SQLite transaction | SQLite rolls back or recovers to one committed configuration |
| process dies or publication allocation fails after runtime commit | new configuration is durable and is loaded at restart |
| export fails | database unchanged; destination may be incomplete |

There is no separate generation pointer that can disagree with the file rows.
The SQLite transaction is the sole durable commit point.

## Required code changes

The names below are concrete recommendations, not a requirement to introduce
more abstraction than the implementation needs.

### New workspace configuration store

Add `src/workspace/workspace_config_store.h` and
`src/workspace/workspace_config_store.cpp` to own the new behavior:

- acquire and retain the database lease;
- enforce private database and sidecar permissions before opening SQLite;
- scan an import/materialized directory into sorted `(name, content)` rows;
- validate and safely convert relative names;
- read and replace the `config` table;
- materialize rows into a private directory using one path shared by import
  validation and runtime startup;
- export rows into a new or empty directory;
- create the known empty workspace skeleton;
- own one normal-runtime private root with `workspace/` and `welcome/`
  children;
- own the runtime configuration mutex;
- coordinate existing narrow file writers, validation, database replacement,
  restoration, and workspace publication; and
- clean up the complete process-private root after its `workspace/` and
  `welcome/` users have shut down.

Keep row representation private and small, for example a structure containing
only `std::string name` and `std::string content`. Do not add a generic virtual
filesystem or storage interface.

### Private filesystem helper

Add one small platform helper for private files and directories and use it from
the top-level configuration/runtime store:

- atomically create a private directory (`mkdir(path, 0700)` on POSIX);
- atomically create a private ordinary file when CHA, rather than SQLite,
  creates it;
- tighten and verify an existing database or SQLite sidecar as owner-only;
- create exported `.env` without a default-permission window; and
- provide equivalent private ACL behavior on Windows.

Remove `SessionRepository`'s current default-permission
`create_directory`-then-chmod sequence. The repository receives the
store-created private `welcome/` child instead. Do not change the process-wide
umask as a substitute; it is global process state and runtime has multiple
threads.

### `src/session/workspace_session_database.*`

- Bump the schema version from 1 to 2.
- Add `config` to schema creation and required-object validation.
- Add the import-only version-1-to-version-2 transaction.
- Keep the existing application ID, session tables, integrity checks, foreign
  key checks, WAL initialization, and checkpoint functions.
- Make normal runtime validation reject version 1 with an actionable import
  message.

The upgrade should reuse the same `config` table definition as new database
creation so the two paths cannot drift.

### `src/session/session_repository.*`

- Change construction to accept the database path explicitly, in addition to
  the stable materialized workspace root used by current workspace matching.
- Stop deriving the database path as `<workspace>/workspace.sqlite3`.
- Remove ownership/acquisition of `SessionLease`; the top-level configuration
  store already holds it.
- Remove legacy directory-database detection from runtime construction, which
  sees only a materialized configuration tree.
- Preserve session repository operations, temporary Welcome-session storage,
  forum synchronization, maintenance fencing, and shutdown checkpointing.
- Accept the already-private `welcome/` directory from the composition root;
  create the temporary database there, but do not own or recursively remove
  the process-private root.

Retain `has_legacy_session_databases()` as a path-only import preflight and its
actionable messages. `workspace_session_database_path()` can be removed after
explicit database-path injection. Rename or reduce
`src/session/session_storage_layout.*` only if that makes the surviving guard
clearer; do not remove the guard.

### `src/workspace/workspace.*`

- Extend `Workspace::load` with the durable relative-path base while retaining
  the current one-argument overload for focused directory-based unit tests.
- Resolve a relative `logging.file` against that base.
- Keep every existing workspace parser and narrow TOML writer.
- Keep the existing `loadws(Workspace)` publication path; its small
  post-commit allocation window is recoverable at restart and does not warrant
  another overload.

### `src/util/environment.*`

- Factor dotenv parsing into a side-effect-free operation usable by import.
- Add scoped application for import validation: set only previously absent
  variables, expose them to `std::getenv` while `Workspace::load` runs, and
  restore the previous environment afterward.
- Keep startup application of parsed variables and the rule that inherited
  variables, including inherited empty values, are not overwritten.
- Preserve entry ordering so repeated dotenv names behave as they do today.
- Preserve current parsing rules and error text where practical.

### `src/web/application_config.*`

- Separate command-line parsing from loading stored `app.toml`.
- Add `--data` as the required database path.
- Add mutually exclusive `--import` and `--export` directory options.
- Remove `--config` and `--workspace`.
- Remove the `workspace`, `config_file`, and `backup_dir` runtime fields that
  represented old filesystem authority.
- Load `host` and `port` from materialized `app.toml`, then apply CLI
  overrides.
- Retain `root` for installed web assets and the hidden test idle-grace option.
- Produce mode-specific usage and clear missing/invalid database errors.

Do not preserve the old parser by fabricating a synthetic argument vector. A
small direct refactor is easier to read than translating the new command into
the old command internally.

### `src/web_main.cpp`

- Dispatch import, export, or normal runtime immediately after command-line
  parsing.
- Construct the configuration store/lease before opening the database or
  loading configuration.
- Use its stable materialized root for dotenv and `Workspace::load`.
- Pass the explicit database path into `SessionRepository`.
- Pass the one process-private root's `welcome/` child into
  `SessionRepository` rather than letting the repository create another random
  directory.
- Pass the runtime configuration store to the few components that persist
  narrow configuration changes.
- Remove `backup_dir` from `run_web_server` and route construction.
- Keep the store/private-root owner alive until after server, provider,
  session, and logging shutdown so it performs the one recursive cleanup last.

### `src/workspace/session_open.*`

- Give the production session opener access to the runtime configuration
  store.
- Route default-character and default-persona persistence through its
  serialized transactional mutation operation instead of directly writing
  and reloading the temporary directory.
- Preserve the existing callbacks and live-session behavior.

### `src/web/lobby_routes.*`

- Give character provider/style updates access to the same runtime
  configuration store.
- Preserve validation and affected-session reload behavior.
- Remove `POST /api/v1/workspace/reload` and `backup_dir` state.

### Backup and API surface

- Remove `src/web/workspace_backup.*` and its unit test.
- Remove the reload operation and every reload reference from
  `resources/cha.yaml`, including bootstrap text.
- Remove now-unreachable `workspace_reloading` from the shutdown-reason enum
  and `workspace_reload_failed` from the error-code enum in C++, OpenAPI, and
  generated TypeScript.
- Remove `WorkspaceReloadReservation`, `WorkspaceReloadResult`,
  `reserve_workspace_reload`, the manager's global reload state, their protocol
  mappings, and their focused tests. Character-setting changes continue to use
  the distinct `reloading` reason.
- Regenerate the web client schema; remove its reload method, reload-specific
  error allowlist entry, UI cases/comments, and tests.
- Remove reload process/unit tests while retaining tests for the three narrow
  runtime mutations.

### Documentation

Update all user and architecture documentation in the same implementation
change; leaving directory/reload instructions behind would make the new CLI
unsafe to operate:

- `README.md`: replace `--workspace`, config-file `--config`, workspace reload,
  `backup_dir`, and durable directory instructions with the unified database
  and offline export/import workflow.
- `docs/tutorial.md`: rewrite startup ordering, configuration authority,
  materialization, database lease ownership, import-time legacy guard, and the
  route list; consistently name the database `workspace.sqlite3` in examples.
- `docs/users.md`: remove reload as a global operation, describe configuration
  publication through offline import/restart, and replace reload-preservation
  tests with failed-import/runtime-mutation preservation tests.
- `docs/session-design.md`: add the supersession notice described above.
- `docs/web-ui/api-requirements.md`: remove reload requirements and describe
  the remaining narrow character-setting mutation.
- `src/README.md`, `src/web/README.md`, `src/workspace/README.md`, and other
  source-layer notes: replace filesystem authority, reload, and old lease
  ownership descriptions.
- Generated/package/sample command documentation: use mandatory database
  `--data`, remove `--config` and `--workspace`, and include the one-time import
  step.

Documentation that uses `workspace/` merely as the C++ source-module name or
as an example logical configuration tree may retain that term; documentation
must not imply that normal runtime reads a durable workspace directory.

### Build and test support

- Add the new store source to `cha_core` in `CMakeLists.txt`.
- Compile bundled SQLite with `SQLITE_DEFAULT_FILE_PERMISSIONS=0600`.
- Add focused store tests to the core test target.
- Update test workspace helpers so they can produce an import directory and a
  unified database independently.
- Update `WebServerProcess` to run with `--data DATABASE`; process fixtures
  must import configuration before starting the server.
- Keep direct `Workspace::load(directory)` unit tests. They test the parser,
  not the durable storage mode, and remain useful.

## Test plan

### Configuration store unit tests

- Create a version-2 database from a valid directory.
- Upgrade a populated version-1 database and prove every session/turn/entry
  remains unchanged.
- Replace version-2 configuration without changing session rows.
- Round-trip import to export and compare every stored file byte-for-byte.
- Include nested TOML and Markdown plus root `.env`.
- Import a provider whose required API key exists only in `.env`, with that
  variable absent from the importing process.
- Prove an inherited provider-key variable takes precedence over `.env` during
  import, including the current behavior for an inherited empty value.
- Prove scoped import validation restores variables that it temporarily added.
- Accept a template include whose target is a collected `.md` file.
- Reject before opening the database a template include whose target exists in
  the source tree but is excluded from storage, such as `snippet.txt`.
- Prove import validation and runtime startup use the same materializer and
  resulting file set.
- Recreate required empty skeleton directories.
- Reject missing `app.toml` or `workspace.toml`.
- Reject absolute, empty, dot, dot-dot, backslash, and unsupported stored names.
- Prove direct SQL inserts are rejected by the table check for a leading `/`,
  a backslash, or any `..` component, while application validation continues
  to reject the full invalid-name set.
- Reject matching symbolic links during import.
- Reject a non-empty export destination and never overwrite files.
- Preserve the old configuration after invalid consumed TOML/template, dotenv,
  or workspace validation.
- Restore the materialized tree after a simulated runtime commit failure.
- On POSIX, verify a newly created database, WAL/SHM sidecars, exported `.env`,
  and the single process-private root have owner-only modes from creation.
- Verify normal runtime places both `workspace/` and `welcome/` beneath that
  one root and removes the root through one cleanup owner.
- Tighten an existing permissive version-1 database and sidecars before
  committing `.env`; fail import if any cannot be secured.
- Exercise equivalent private-permission behavior in platform-specific Windows
  tests.

### Lease and process tests

- Runtime prevents a second runtime process from opening the same database.
- Runtime prevents import and export.
- Import or export prevents runtime while it holds the lease.
- A stale lock file without a held kernel lock does not prevent startup.
- Import with legacy per-session databases and no target database fails with
  the archived-migration instruction and creates no database.
- Import with legacy per-session databases and a target database fails with
  the incomplete-cleanup instruction and changes no database content.
- Import into a missing database followed by deletion of the source tree still
  permits normal runtime.
- Runtime refuses a missing or version-1 database with an import instruction.

### Runtime mutation tests

- Character provider/style changes survive restart and appear in export.
- Default character and persona changes survive restart and appear in export.
- Concurrent mutations are serialized and do not publish an older candidate
  after a newer one.
- A validation or SQLite failure leaves the published workspace, database,
  and rematerialized files on the old configuration.
- Existing affected-session reload behavior remains intact.

### Application configuration tests

- Stored `app.toml` supplies host and port.
- CLI host and port override stored values.
- `--data` is required and names the SQLite database.
- The removed `--config` and `--workspace` options are rejected.
- Import/export reject runtime-only arguments.
- `.env` is stored, validated on import, and applied on startup without
  replacing inherited variables.
- Relative logging paths resolve beneath the database parent rather than the
  temporary workspace.

## Operator cutover

The intended one-time cutover is:

1. Stop CHA.
2. Back up the existing workspace and `workspace.sqlite3` by the operator's
   normal method.
3. Check for legacy databases under `forums/*/sessions/` and
   `forums/*/sessions/deleted/`. If any exist, use the archived
   migration-capable CHA build to migrate them into `workspace.sqlite3`, verify
   the result, and remove the legacy files. The new import command refuses to
   continue while they remain.
4. Place the new `app.toml` in the directory that will be imported. Remove its
   obsolete `workspace` and `backup_dir` settings.
5. Run:

   ```text
   chaweb --data /absolute/path/workspace.sqlite3 \
          --import /absolute/path/workspace
   ```

6. Optionally verify the stored representation:

   ```text
   chaweb --data /absolute/path/workspace.sqlite3 \
          --export /absolute/path/exported-workspace
   ```

7. Start normal runtime:

   ```text
   chaweb --data /absolute/path/workspace.sqlite3
   ```

The old workspace directory may be retained as an operator backup, but runtime
never reads it. Subsequent manual edits follow export, edit, stop, and import.

## Design rationale

The central choice is materialization rather than a direct SQLite-aware
`Workspace` parser. The current parser relies on directory iteration, physical
paths, relative template includes, containment checks, and existing file
writers. Teaching all of those operations about a second storage interface
would change a large amount of stable code and create two behaviors to keep in
sync.

A private materialized directory turns the new database representation back
into exactly the input the parser already understands. The new code is
concentrated at the storage boundary, while unavoidable changes are limited to
startup ownership, explicit database-path injection, relative log resolution,
and the three existing persistence call sites.

Replacing the whole small `config` table follows the same principle. SQLite
already provides the atomic mechanism, so generations, revisions, incremental
diffs, and control rows would add concepts without solving an unsolved problem.
