# Unified CHA database implementation plan

Status: proposed implementation sequence for [`docs/design.md`](design.md).

This plan moves every durable CHA datum into one SQLite database selected by
`--data DATABASE`. The database continues to hold sessions and gains the current
workspace configuration as `(name, content)` rows. The directory workspace becomes
an import/export format only; normal runtime materializes its configuration into a
private temporary directory.

The work is divided into eight sequential blocks. Each block is intended to fit in
one Codex implementation session, including its focused tests and review. Blocks are
as large as practical while retaining one coherent outcome and a clear stopping
point. Implement them in order because later blocks assume the interfaces and
invariants established earlier.

Every block is an implementation staging point, not a separate production release.
Each must compile and pass its focused tests, but the application should not be
deployed from an intermediate block. Block 6 establishes the first complete new
runtime path; Blocks 7 and 8 are still required before release because they finish
hardening, distribution changes, documentation, and cutover verification.

## Final invariants

Keep these rules visible throughout implementation. If a block exposes a conflict
with one of them, update the design before proceeding rather than silently changing
the intended behavior.

- `--data DATABASE` is mandatory for import, export, and normal execution.
- A normal runtime never reads configuration from a durable workspace directory.
- `--workspace` and the old `--config APP_TOML` option are removed without a
  compatibility mode.
- The database schema version is 2. Version 1 contains sessions only; version 2 adds
  the `config` table. Only import may upgrade version 1 to version 2.
- `config` has exactly two application-visible columns: `name` and `content`. There
  is no generation, type, control, or revision column.
- Import validates a byte-identical materialization of the rows that it will commit.
  It must not validate extra source files that will not exist at runtime.
- Runtime, import, and export all hold the existing database lock-file lease for
  their full database-using lifetime. They are mutually exclusive.
- The database, rollback journal, WAL, SHM file, private runtime tree, and exported
  `.env` do not receive group or other access.
- A normal process has one private temporary root with `workspace/` and `welcome/`
  children and one owner responsible for cleanup.
- Runtime configuration edits update the materialized tree, validate it, replace
  all configuration rows in one SQLite transaction, and only then publish the new
  in-memory workspace.
- A published `Workspace` eagerly owns all parsed values. After `Workspace::load`
  returns, no normal read may reopen files below `Workspace::root()`; filesystem
  access there is limited to candidate loading, the three narrow writers, and store
  restoration while the configuration mutex is held. `SessionRepository` may
  compare the stable root path as an identity value but must not read configuration
  through it.
- The broad workspace-reload operation and workspace backup machinery are removed.
  Character/session reload for a specific settings change remains.
- The legacy per-session database detector remains in import preflight so a fresh
  unified database cannot silently hide old sessions.
- Simplicity wins: use direct functions and small value types; do not add a virtual
  filesystem, repository hierarchy, migration framework, watcher, or generic
  transaction abstraction.

## Working rules for every block

At the beginning of each block:

1. Read the relevant sections of `docs/design.md` and inspect the current worktree.
   Preserve unrelated user changes.
2. Run the smallest useful baseline test set for the files being changed. If the
   baseline is already failing, record the failure before editing.
3. Locate current definitions and call sites before modifying an interface. Use
   `local-investigate` for broad questions and `rg` for a single obvious lookup.

At the end of each block:

1. Follow the surrounding C++ style; the repository does not define a general C++
   formatter. For webapp changes, run `npm run check`.
2. Run the block's focused tests and at least compile every target affected by an
   interface change.
3. Run `git diff --check` and inspect the complete diff for accidental generated,
   fixture, or user-file changes.
4. Leave no commented-out implementation, unused compatibility overload, or TODO
   that merely defers work assigned to the current block.
5. Record any design discrepancy before starting the next block.

## Block overview

| Block | Outcome | Main risk retired |
|---|---|---|
| 1 | Private-filesystem, dotenv, and workspace-loading seams | Secrets and validation cannot be handled safely by later code |
| 2 | Version-2 schema and configuration-row database primitives | Session loss or partial schema upgrades |
| 3 | Complete offline import/export implementation | Source/runtime mismatch and unsafe path handling |
| 4 | Runtime materialization and atomic configuration-edit engine | Durable and in-memory configuration divergence |
| 5 | Obsolete reload/backup feature removed end to end | Two competing configuration update models |
| 6 | CLI, composition root, and runtime fully cut over to the database | Split storage remains reachable in production |
| 7 | Integration hardening, fixtures, samples, and packaging updated | Edge cases or distribution paths retain old assumptions |
| 8 | User/developer docs and final release verification complete | Operators cannot perform a safe cutover |

### Primary implementation locations

| Block | Expected files or areas |
|---|---|
| 1 | `src/util/environment.*`, new `src/util/private_filesystem.*` (name may follow local convention), `src/workspace/workspace.*`, `src/session/session_repository.*`, SQLite target definitions, focused utility/workspace tests |
| 2 | `src/session/workspace_session_database.*`, its unit tests, and small row/database helpers consumed by the store |
| 3 | new `src/workspace/workspace_config_store.*`, `src/web/application_config.*` parser seam, store tests, and `CMakeLists.txt` source/test registration |
| 4 | `src/workspace/workspace_config_store.*`, workspace publication/loading call sites used by its tests, and runtime-store tests |
| 5 | `src/web/lobby_routes.*`, `src/web/workspace_backup.*`, live-session manager/protocol sources, `resources/cha.yaml`, generated API clients, webapp UI/tests, and CMake removal entries |
| 6 | `src/web/application_config.*`, `src/web_main.cpp`, `src/session/session_repository.*`, `src/workspace/session_open.*`, `src/web/lobby_routes.*`, and native process/test fixtures |
| 7 | remaining native/web integration tests, sample workspace, CMake staging, package/service/launch assets, and generated artifacts |
| 8 | root/source READMEs and the files under `docs/` named in the approved design |

Exact test filenames should follow current repository organization. Avoid moving
unrelated code merely to make it match this table.

## Block 1 — Establish security and loading foundations

### Goal

Add the small reusable seams needed by all later work without changing how users
start CHA. This block should be almost entirely additive. Existing directory-based
startup remains functional at its end.

### 1.1 Baseline and ownership inventory

- Identify the current owners and cleanup paths for:
  - the workspace session database and its lease;
  - the `cha-session-*` Welcome database directory;
  - dotenv parsing and process-environment mutation;
  - `Workspace::load` and its log-file path base.
- List every call site of `Workspace::load`, `SessionRepository` construction,
  `load_dotenv`, and private temporary-directory creation. This list is the checklist
  for the later interface changes.
- Run the existing session, workspace, application-config, and web process tests that
  cover these areas.

### 1.2 Add a private-filesystem helper

- Add one small utility for security-sensitive directories and files. Keep the API
  concrete; it only needs the operations used by this design:
  - create a directory atomically with owner-only access;
  - tighten and verify an existing regular file's permissions;
  - create or replace a regular file with owner-only access;
  - reject symlinks and unexpected file types where the caller requires a regular
    file or directory.
- On POSIX, create directories with `mkdir(..., 0700)` instead of creating with
  default permissions and applying `chmod` afterwards. Create secret files with
  mode `0600` from the first successful open.
- Provide the corresponding owner-only Windows ACL behavior behind the same narrow
  utility. Do not emulate POSIX modes on Windows.
- Do not change the process-wide umask.
- Replace the current `create_private_directory` implementation used for the Welcome
  database with this helper immediately, eliminating its create-then-chmod window.
  The ownership of that directory will move in Block 6.
- Add SQLite's `SQLITE_DEFAULT_FILE_PERMISSIONS=0600` compile definition to the
  bundled SQLite target so new databases and sidecars are private by default.

### 1.3 Separate dotenv parsing from application

- Refactor the environment utility into two explicit operations:
  1. parse a `.env` file into an ordered collection or map without mutating the
     process environment;
  2. apply parsed values using the current startup policy.
- Preserve the current runtime semantics for duplicate keys, quoting, invalid lines,
  inherited variables, and empty values.
- Add a small scoped environment overlay for import validation:
  - only variables absent from the importing process are installed;
  - values already present, including the policy for an empty present value, follow
    the approved design exactly;
  - every inserted variable is removed when the scope ends, including on exceptions;
  - unrelated environment variables are never touched.
- Avoid a general dependency-injection system for environment lookup. The scoped
  overlay exists only to let the unchanged provider validator see imported dotenv
  values during `Workspace::load`.

### 1.4 Add the durable-relative-path base to workspace loading

- Add `Workspace::load(materialized_root, durable_relative_path_base)`.
- Retain `Workspace::load(directory)` for parser-focused unit tests. Make it delegate
  to the new overload using the directory as both the physical root and durable path
  base, so there remains only one loading implementation.
- Resolve template includes against `materialized_root` so containment remains
  rooted in the private workspace tree.
- Resolve durable relative paths such as logging paths against
  `durable_relative_path_base`. Runtime will pass the database parent directory.
- Do not add a virtual path abstraction. Both arguments are ordinary filesystem
  paths with clearly different responsibilities.

### 1.5 Tests and completion gate

Add focused unit tests for:

- POSIX modes at creation time and rejection of symlink targets;
- dotenv parsing without side effects;
- scoped overlay precedence and restoration on success and exception;
- an API key supplied only through `.env` satisfying provider validation inside the
  overlay;
- independent resolution of include paths and durable log paths.

The block is complete when existing startup behavior is unchanged, the relevant
native test targets pass, and the SQLite target is demonstrably compiled with the
private default file mode.

## Block 2 — Add schema version 2 and database primitives

### Goal

Teach the session database layer to recognize, create, validate, and atomically
upgrade the unified schema. Expose only the minimal row operations needed by the
configuration store; do not implement directory traversal in the database layer.

### 2.1 Define the version-2 schema once

- Bump the workspace session database schema version from 1 to 2.
- Add the strict table exactly once in a shared schema definition:

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

- Keep the application-level path validator even though the `CHECK` provides
  defense in depth.
- Do not add `generation`, `type`, `config_control`, timestamps, hashes, or metadata.
- Ensure new databases are created directly at version 2 with both session and
  configuration schema objects.
- Extend schema validation so a database claiming version 2 must contain the exact
  required configuration and session objects. A missing or malformed table is an
  error, not an opportunity for runtime repair.

### 2.2 Separate inspection, creation, runtime opening, and import upgrade

- Add a read-only database identity/schema probe that can distinguish:
  - no database;
  - a valid version-1 CHA session database;
  - a valid version-2 unified database;
  - a foreign, corrupt, unsupported, or structurally incomplete database.
- Add the version-1-specific runtime diagnostic in this block, before the generic
  unsupported-schema branch. It must identify the database as a valid CHA schema-1
  database and instruct the operator to stop CHA and run
  `chaweb --data DATABASE --import WORKSPACE` to upgrade it. Do not make a valid
  version-1 database look foreign or corrupt.
- The final normal runtime and export accept only a valid version-2 database. During
  Blocks 2 through 5, the still-directory-based development runtime may retain its
  existing missing-database creation path so a disposable local/CI database can be
  recreated at version 2; Block 6 removes that transitional creation path.
- Because executable import is not wired until Block 6, do not delete a data-bearing
  version-1 database during the intermediate blocks. Preserve it for the import
  upgrade. Developers and CI may delete and recreate only disposable databases that
  contain no sessions they need to keep.
- Import may:
  - create a missing database directly as version 2;
  - upgrade a valid version-1 database in place;
  - replace the configuration rows of a valid version-2 database.
- The version-1 upgrade must validate the existing session schema before beginning
  its write transaction, create `config`, insert the imported rows, and update
  `user_version` last in the same transaction.
- Do not create a chainable migration framework. One explicit `v1 -> v2` function is
  enough.
- A failed creation must not leave a database that looks valid but has no imported
  configuration. A failed upgrade must roll back to a valid version-1 database with
  all session rows intact.

### 2.3 Add low-level configuration-row operations

- Introduce the internal two-field value type used between SQLite and the store,
  for example `ConfigFile { std::string name; std::string content; }`.
- Add operations to:
  - read all rows in deterministic name order;
  - replace all rows using `DELETE` followed by inserts inside the caller's single
    transaction;
  - read and bind `content` using explicit byte counts so empty values and embedded
    NUL bytes round-trip exactly.
- Bind names and contents as parameters. Do not derive SQL identifiers from paths.
- Do not add UTF-8 or other content-encoding validation at the database boundary.
  Existing semantic parsers remain responsible for files they consume.
- Let higher layers enforce required files and the accepted source-file set. The
  database layer enforces only schema and safe stored-name invariants.

### 2.4 Enforce durable-store permissions before SQLite opens it

- After acquiring the lock-file lease and before opening SQLite, tighten and verify
  the main database and any existing `-wal`, `-shm`, and `-journal` files.
- Reject symlinks and non-regular objects for all of those paths.
- Apply the same check in runtime, import, and export through one shared opening
  path; do not duplicate it in each command handler.
- Rely on the SQLite compile-time default from Block 1 for newly created sidecars,
  then verify their modes in tests.

### 2.5 Tests and completion gate

Cover at least:

- creation of a valid empty version-2 schema;
- opening and validating a populated version-2 schema;
- rejection of a valid version-1 database by normal runtime with the specific import
  instruction, distinct from the generic unsupported-schema diagnostic;
- rejection of missing tables, wrong columns, wrong application id, future schema
  versions, and foreign SQLite files;
- direct SQL rejection of empty, absolute, backslash-containing, and `..` names;
- deterministic row reads and atomic all-row replacement;
- byte-exact content reads and writes, including empty content, non-UTF-8 bytes, and
  embedded NULs;
- successful version-1 upgrade preserving every existing session and label row;
- injected failure during upgrade leaving version 1 unchanged;
- permission tightening for an existing database and sidecars;
- owner-only modes for newly created database, WAL, and SHM files.

Run the session database and repository test targets. The block is complete when no
ordinary runtime path silently upgrades version 1 and all schema-changing operations
are transactionally tested.

## Block 3 — Implement the offline configuration store

### Goal

Implement one `WorkspaceConfigStore` (or equivalently named small component) that
converts between directory trees and rows. Import and export should be fully testable
as library operations before CLI and runtime wiring changes.

### 3.1 Define the component boundary

The store owns these responsibilities:

- collect accepted files from an import directory;
- validate stored relative names;
- materialize rows into a caller-supplied private workspace directory;
- validate an imported materialization;
- create or upgrade the database and atomically replace configuration rows;
- export rows to a new directory;
- return concise summaries and path-specific errors.

It does not own session queries, HTTP routes, UI state, template parsing, or provider
validation. Reuse `Workspace::load` for semantic workspace validation.

### 3.2 Collect the import tree deterministically

- Resolve the source to an absolute normalized directory path.
- Recursively inspect entries without following symlinks. Reject a symbolic link that
  would otherwise match the stored configuration set; ignore unrelated files and do
  not traverse symlinked directories.
- Store only:
  - root `app.toml`;
  - root `workspace.toml`;
  - every regular `.toml` file below the root;
  - every regular `.md` file below the root;
  - optional root `.env`.
- Do not store empty directories or unrelated files.
- Convert paths to normalized relative names using `/`, independent of the host
  platform. Validate each name before reading content.
- Read file contents byte-for-byte and bind them as SQLite `TEXT` with an explicit
  byte count. Preserve embedded NULs and arbitrary byte sequences; do not add UTF-8
  validation or newline conversion. A parser may still reject invalid syntax in a
  file it consumes, but the storage layer has no separate encoding policy.
- Sort rows by name for deterministic tests and exports.
- Require `app.toml` and `workspace.toml` in the collected set before any database is
  opened for writing.

### 3.3 Implement one safe materializer

- Validate the complete row set before creating any output file:
  - unique names;
  - normalized relative paths;
  - no absolute path, empty segment, `.` or `..` segment, or backslash;
  - required root files are present;
  - every parent/child relationship can be represented as directories and regular
    files without collision.
- Join validated names beneath the supplied destination and verify containment.
- Create the known empty workspace skeleton (`system/providers/`, `personas/`,
  `characters/`, and `forums/`) before writing rows.
- Create directories privately and write regular files without following symlinks.
- Write `.env` with owner-only access. Other materialized files are already protected
  by the owner-only parent but should not be group/other writable.
- Use this exact materializer for import validation, normal runtime, and export. Do
  not write a source-tree validator whose accepted input differs from runtime input.

### 3.4 Parse stored application settings

- Extract a side-effect-free application-settings parser that reads materialized
  `app.toml` and reports file/field errors without parsing command-line arguments.
- The stored application settings contain required `host` and `port`. They do not
  accept or retain `workspace` or `backup_dir`; removal of the old runtime backup
  field and its remaining consumers is completed in Block 5.
- Retain the current parser entry point as a thin transitional wrapper until the CLI
  cutover, but keep all TOML validation in one implementation.

### 3.5 Implement import in the required order

Import must execute these phases in order:

1. Canonicalize the source and database paths, require the source and database parent
   directories to exist, and acquire the target database lease non-blockingly.
2. Run the path-only legacy-session detector against the source unconditionally. If
   it finds a legacy database, fail with the existing actionable distinction: a
   missing target database means the archived per-session-to-workspace migration was
   never run, while a present target database means migration cleanup is incomplete.
3. Collect and validate the exact rows described above.
4. Create a short-lived private validation root with a `workspace/` child.
5. Materialize the collected rows with the shared materializer.
6. Parse materialized `app.toml` and `.env` without changing the environment yet.
7. Apply the parsed dotenv values through Block 1's scoped, non-overwriting overlay.
8. Call `Workspace::load(validation_workspace, database_parent)` while the overlay is
   active. This deliberately exercises provider key validation, template includes,
   and the same filesystem representation runtime will see.
9. Destroy the overlay and validation tree whether validation succeeds or throws.
10. Only after validation succeeds, secure existing files, inspect/create/upgrade the
    database, and replace all configuration rows in one transaction. Retain the
    lease acquired in step 1 until the command finishes.

Important consequences to test explicitly:

- `{{include shared/snippet.txt}}` fails import because `.txt` is not stored and is
  therefore absent from the validation materialization.
- A provider key present only in source `.env` succeeds during import validation but
  is absent from the importing process again after import returns.
- A semantic validation failure never creates or modifies the target database.
- A SQLite failure never exposes a partial new configuration.

### 3.6 Implement export safely

- Acquire the target database lease, secure/open the database, require version 2,
  read and validate all rows, and only then prepare output.
- Require the destination to be absent or an existing empty directory. Reject a
  non-empty destination before writing and never overwrite an existing file.
- Create a missing destination and the known skeleton, then materialize directly
  into it. If an I/O failure occurs after writing begins, report that the destination
  may be incomplete and must be emptied before retrying.
- Never recursively delete an unresolved or pre-existing user path.
- Ensure exported `.env` is `0600` and no exported path can escape the destination.
- Do not export sessions or SQLite implementation files.

### 3.7 Tests and completion gate

Add store tests for:

- exact round-trip of all accepted files, Unicode, arbitrary bytes, embedded NULs,
  empty contents, and nested paths;
- ignoring ordinary unsupported files while rejecting a template that depends on
  one of them;
- rejecting symlinks, path traversal, backslashes, absolute names, duplicate names,
  file/directory collisions, missing required files, and malformed TOML/Markdown;
- `.env` provider validation and complete environment restoration;
- database-parent resolution of durable relative paths;
- import into a missing database, a valid version-1 database, and an existing
  version-2 database;
- preservation of sessions across both upgrade and configuration replacement;
- rollback on validation, schema, insertion, and commit failures;
- refusal to export a foreign/v1 database or a non-empty destination;
- import/export lock contention and private output permissions;
- both legacy-database detector outcomes and their actionable messages.

The block is complete when import and export work through direct C++ calls and every
validation path operates on materialized rows rather than the richer source tree.

## Block 4 — Implement runtime storage and atomic edit primitives

### Goal

Prepare the normal-runtime owner and configuration mutation algorithm behind direct
unit tests. Do not change the public command line yet. Keeping the final wiring out of
this block limits its context to lifetime, locking, materialization, and transactions.

### 4.1 Add the normal-runtime owner

- Add a concrete runtime-mode object, preferably as part of the configuration store,
  that owns in declaration/destruction order:
  1. the database lock-file lease;
  2. the secured SQLite connection or repository-facing database handle;
  3. one process-private temporary root;
  4. `workspace/` and `welcome/` children;
  5. the process-wide configuration mutation mutex.
- Acquire the lease before inspecting permissions, schema, configuration rows, or
  session rows.
- Create the private root atomically with owner-only access. Create both children
  beneath it and expose their paths read-only to consumers.
- Materialize version-2 configuration rows into `workspace/` once at startup.
- Use a single cleanup path owned by this object. Consumers must not remove the root
  or either child.

### 4.2 Define startup loading through the runtime owner

- Add an operation that applies materialized dotenv using the existing startup
  semantics, parses materialized `app.toml`, and calls
  `Workspace::load(materialized_workspace, database_parent)`.
- Keep the lease held across materialization and for the complete lifetime of the
  returned runtime owner.
- Fail clearly for missing/invalid required configuration, a version-1 database,
  malformed stored names, or materialization errors.
- A restart must rebuild the private tree solely from database rows; it must never
  reuse an orphaned previous process tree.

### 4.3 Implement one runtime configuration edit operation

- Place the complete edit sequence behind one store operation so all current narrow
  writers share the same mutex and failure behavior.
- Before implementing in-place restoration, re-audit every use of
  `Workspace::root()` and every filesystem open beneath the materialized tree.
  Confirm that a published `Workspace` eagerly owns its values, that
  `SessionRepository` only compares the stable root path, and that candidate loading
  plus the three narrow writers and store restoration are the only filesystem access
  after publication, all under the configuration mutex. If any asynchronous or
  unlocked read reaches the tree, stop and revise the design; in-place restoration
  would otherwise expose torn files.
- Under the mutex:
  1. apply the caller's existing filesystem edit to the materialized tree;
  2. load and validate a candidate `Workspace` from that tree;
  3. collect the entire accepted configuration set from the materialized tree;
  4. begin one SQLite write transaction;
  5. delete and insert all `config` rows;
  6. commit;
  7. publish with the existing `loadws(Workspace)` overload;
  8. let the caller reload only sessions affected by its specific change.
- Do not add a preallocated-publication overload. Allocation before `loadws` already
  occurs outside `workspace_mutex`, and restart is the recovery for an exceptional
  allocation failure after commit.
- On any failure before commit, restore the materialized tree in place from current
  database rows before releasing the configuration mutex. Keep the existing
  `workspace/` directory itself and its stable path; remove/recreate only its
  contents and required child skeleton. Never rename, remove, or replace the
  `workspace/` directory because published workspace and session-repository identity
  checks retain that path. Report the original failure; if restoration also fails,
  include that fact and require process restart.
- On a failure after commit but before in-memory publication, report that the durable
  update committed and require restart. Do not attempt a compensating transaction.
- Keep SQLite transactions short: filesystem edits, materialization, and
  `Workspace::load` validation happen before the write transaction.
- Do not reload `.env` or `app.toml` after a narrow runtime mutation. Neither is
  editable through the runtime UI; changing them remains an offline import followed
  by restart.

### 4.4 Unit tests and completion gate

Test the runtime owner and edit primitive directly for:

- a single private root containing exactly the expected workspace and Welcome
  subtrees;
- lock lifetime and rejection of a second runtime/import/export process;
- startup from database rows after deleting the original import tree;
- clean recreation after an orphaned temporary tree is left behind;
- successful edits changing materialized, database, and in-memory state together;
- serialization of two edits and of an edit racing a workspace read;
- validation failure restoring the tree and leaving database/in-memory state old;
- SQLite write or commit failure restoring the tree;
- restoration preserving the same `workspace/` directory while replacing its
  contents, with published workspace reads and session identity checks remaining
  valid throughout;
- simulated post-commit publication failure yielding the documented restart state;
- successful restart publishing the already-committed configuration.

This block is complete when the new owner and edit engine are independently usable,
but no production route or CLI depends on them yet.

## Block 5 — Remove workspace reload and backup end to end

### Goal

Delete the broad directory-reload feature before the final cutover so there is only
one configuration update model to wire. This is a removal block: avoid replacement
state or compatibility endpoints.

### 5.1 Remove backend reload and backup machinery

- Remove `POST /api/v1/workspace/reload` and its route registration.
- Remove `backup_dir`, `workspace_backup.*`, their CMake entries, and their tests.
- Remove `WorkspaceReloadReservation`, `WorkspaceReloadResult`, global workspace
  reload reservation/state, and related `LiveSessionManager` branches.
- Remove error handling and messages that exist only for the broad reload.
- Preserve the separate, session-scoped `reloading` reason used after character
  settings changes.
- Simplify `ApplicationConfig` only as far as removing backup configuration in this
  block; final CLI/TOML separation happens in Block 6.

### 5.2 Remove protocol and API surface

- Remove `workspace_reloading` and `workspace_reload_failed` from
  `resources/cha.yaml`.
- Remove reload references embedded in unrelated descriptions so generated
  documentation does not imply the operation still exists.
- Hand-edit the repository's C++ protocol implementation; there is no C++ generator.
  Remove the enum values and string mappings from `src/web/protocol.h` and
  `src/web/protocol.cpp`, and remove the shutdown-priority case from
  `src/web/live_session.cpp`, together with the manager/route uses removed in 5.1.
- From `webapp/`, run `npm run api-types` to regenerate
  `webapp/src/api/schema.d.ts` from `resources/cha.yaml`. That TypeScript schema is
  generated and must not be hand-edited; the C++ files above are hand-maintained.
- Remove client methods, response types, UI controls, state, notifications, and tests
  dedicated to workspace reload.

### 5.3 Tests and completion gate

- Build native server and protocol targets after the hand edits, and verify
  `npm run api-types:check` after TypeScript regeneration.
- Run backend route and live-session tests.
- Run the webapp API type check, unit tests, typecheck, and production build.
- Search the repository for `workspace_reload`, `workspace reloading`, `backup_dir`,
  and the deleted symbols. Remaining occurrences should be historical design text or
  the intentional session-specific `reloading` concept, not executable behavior.

The block is complete when the route is absent, the OpenAPI source and generated
TypeScript schema agree, the hand-maintained C++ protocol has no removed values, the
UI has no reload affordance, and no backup directory is required by application
startup.

## Block 6 — Cut CLI and normal runtime over to the unified database

### Goal

Make the new storage model the only executable behavior. This is the largest wiring
block, but the schema, store, edit engine, and obsolete-feature removal are already
complete, keeping it within one implementation session.

### 6.1 Replace command-line parsing

- Separate command-line options from stored application settings.
- Support exactly these modes:
  - `chaweb --data DATABASE --import SOURCE_DIRECTORY`;
  - `chaweb --data DATABASE --export DESTINATION_DIRECTORY`;
  - `chaweb --data DATABASE [--root PATH] [--host HOST] [--port PORT]`.
- Require one `--data` in every mode.
- Make `--import` and `--export` mutually exclusive and reject normal-run-only
  options in offline modes where they have no meaning.
- Retain hidden `--test-idle-grace-ms` for normal browser process tests and reject it
  in offline modes.
- Remove `--workspace`. Do not accept aliases or detect the old CLI heuristically.
- Treat `--data` only as the SQLite path. Remove the old `--config` option; it no
  longer names `app.toml` and is not an alias for `--data`.
- Load required `host` and `port` from stored `app.toml` for normal execution, then
  apply CLI `--host` and `--port` overrides. Keep `--root` as the existing web-root
  override, not a workspace path.
- Update `--help` and errors with complete example invocations and clear guidance
  that a missing/version-1 database must first be imported.

### 6.2 Dispatch offline commands at the composition root

- In `web_main`, parse the mode before constructing web-server state.
- For import, call the tested store import and exit after a concise summary.
- For export, call the tested store export and exit after a concise summary.
- Do not initialize HTTP, providers, sessions, or the Welcome database in either
  offline mode.
- Map validation, lock, schema, permission, and filesystem failures to nonzero exit
  status with the specific offending path or database state.

### 6.3 Construct normal runtime from the store

- Replace durable-directory startup with Block 4's runtime owner:
  1. acquire the database lease;
  2. secure and validate the version-2 database;
  3. create the single private root and its two children;
  4. materialize configuration;
  5. apply stored dotenv and load stored application settings;
  6. load the workspace using the database parent for durable relative paths;
  7. initialize logging from the loaded workspace, so a relative `logging.file`
     demonstrably resolves below the database parent rather than the materialized
     root;
  8. initialize sessions and web routes while keeping the owner alive.
- Ensure destruction order stops users of the workspace/session database before the
  runtime owner releases the lease or removes the private root.
- Delete code that discovers the session database from a durable workspace path.
- Remove runtime invocation of `has_legacy_session_databases`; it belongs only to
  import preflight.

### 6.4 Simplify `SessionRepository` ownership

- Change construction to receive the explicit unified database path, the stable
  materialized `workspace/` path used by existing workspace identity checks, and the
  private `welcome/` path from the top-level runtime owner.
- Use the owner's `welcome/` child for the temporary Welcome database.
- Remove its lock-file acquisition, random `cha-session-*` root creation, private
  root cleanup, database-path derivation, and legacy-directory detection.
- Delete `workspace_session_database_path()` and its focused tests once every caller
  receives the database path explicitly. If `session_storage_layout.*` then contains
  only `has_legacy_session_databases()`, retain it under a clear narrow name or rename
  it with the smallest practical diff; no helper that derives
  `<workspace>/workspace.sqlite3` may remain.
- Keep session CRUD and Welcome-session behavior otherwise unchanged.
- Preserve forum synchronization, maintenance fencing, checkpointing, and shutdown
  behavior apart from the new ownership boundaries.
- Do not give `SessionRepository` ownership of the materialized workspace tree.

### 6.5 Wire narrow runtime edits through the store

- Inject the store/edit capability only into the existing code that persists:
  - character provider/style settings;
  - forum default character;
  - forum default persona.
- Keep each existing filesystem writer as the function that edits the materialized
  TOML/Markdown file, then execute it through Block 4's transaction wrapper.
- Publish the validated candidate via existing `loadws(Workspace)` after commit.
- Reload only the sessions already affected by the corresponding setting, using the
  retained session-specific `reloading` reason.
- Remove direct durable-directory assumptions from `lobby_routes`, `session_open`,
  and any helpers they call.
- Ensure HTTP errors distinguish a pre-commit rejection (nothing changed) from the
  exceptional post-commit condition (restart required).

### 6.6 Update directly broken fixtures and tests

- Change the native process fixture to create a source workspace, run explicit
  import into a fresh database, and then launch the server with `--data DATABASE`.
- Update application-config unit tests for the three modes, required arguments,
  mutual exclusion, overrides, and rejection of the removed `--config` and
  `--workspace` options.
- Update session repository construction in all unit/integration fixtures.
- Update tests for the three runtime edit paths to assert database persistence, not
  just changes to a durable source directory.

### 6.7 Completion gate

Run the affected native unit, web, process, and integration targets. Also complete a
manual smoke test in a disposable directory:

1. Import a representative workspace into a new database.
2. Rename or remove the source workspace.
3. Start the server from the database and open a session.
4. Change each supported runtime setting.
5. Stop and restart the server.
6. Confirm the settings and sessions survive.
7. Export and inspect the resulting workspace.

The block is complete only when no normal execution path accepts or consults a
durable workspace tree and no supported runtime edit can bypass the database.

## Block 7 — Harden integration, fixtures, samples, and packaging

### Goal

Exercise the completed system across process boundaries and update every non-doc
asset that assumes directory-backed runtime configuration. This block closes gaps
that focused implementation tests may miss.

### 7.1 Complete the test matrix

Audit `docs/design.md`'s test plan against tests added in Blocks 1 through 6. Add any
missing coverage, especially:

- process-level mutual exclusion among runtime, import, and export;
- killing import before commit and verifying the prior schema/configuration;
- killing runtime with WAL active, then reopening and checking sidecar permissions;
- replacing configuration in a populated database without changing session counts,
  labels, timestamps, or message content;
- import rejection before database creation for malformed source, missing provider
  key, unsupported include dependency, or legacy session databases;
- export failure clearly reporting a possibly incomplete destination while leaving
  the database and unrelated paths unchanged;
- startup refusal for version 1 with the correct import guidance;
- exact relative logging behavior when the process current directory differs from
  the database parent;
- concurrent runtime edits and restart recovery after every modeled failure point;
- POSIX and Windows-specific private permission behavior where CI supports it.

Keep failure injection local and explicit. Do not build a generic fault-injection
framework for this feature.

### 7.2 Update sample configuration

- Move the sample runtime application settings into the sample import tree as root
  `app.toml`.
- Remove `workspace` and `backup_dir` from sample TOML and require `host` and `port`.
- Ensure sample templates include only file types preserved by import.
- Keep sample `.env` secret-free and document values as placeholders if it is
  distributed.
- Add a fixture with `.env`-only provider credentials for automated import testing;
  never commit a real key.

### 7.3 Update build, staging, and package behavior

- Update CMake sources for new helpers/store files and remove backup sources/tests.
- Update local deployment staging and package scripts so they do not treat a copied
  workspace directory as runtime state.
- Never package a developer's real database or API keys.
- If a sample workspace is distributed, label and place it as an import seed only.
  Do not make normal server startup silently import it.
- Provide an explicit development/package command that imports the seed to a chosen
  database; keep the same command users run in production.
- Update launch scripts and service templates to pass `--data` with a SQLite path.
- Ensure cleanup rules distinguish disposable build databases from user databases.

### 7.4 Run full automated verification

Use the repository's Ninja preset and build directory:

```sh
cmake --preset ninja
cmake --build build/ninja
ctest --test-dir build/ninja --output-on-failure
cd webapp
npm run check
npm run build
npm run e2e
```

Also run configured sanitizer and stress/process-lock targets. If a platform-specific
permission test cannot run locally, ensure it is compiled and assigned to the
appropriate CI platform rather than weakening the assertion.

The block is complete when all code, tests, generated assets, sample trees, launch
scripts, and packaging agree on database-only runtime configuration.

## Block 8 — Update documentation and perform release cutover verification

### Goal

Make the new operational contract unambiguous, remove stale directory/reload
instructions, and verify the exact operator transition on disposable copies. This
block is documentation-heavy but should not introduce new architecture.

### 8.1 Update user-facing documentation

Update at least:

- the root `README.md`;
- `docs/users.md`;
- `docs/tutorial.md`;
- web UI/API documentation generated or maintained outside `resources/cha.yaml`;
- examples showing startup, service files, environment setup, or configuration
  edits.

Document:

- `--data DATABASE` for every command and removal of the old `--config` option;
- initial import, later edit/export/import workflow, and destination rules;
- the requirement to stop the service before import/export and the enforced lease;
- stored `app.toml`, required host/port, and CLI overrides;
- the accepted import file set and the consequence for template includes;
- the fact that `.env` is now durable database content and the required database
  backup/permission discipline;
- version-1 import upgrade and the archived legacy per-session migration prerequisite;
- removal of `--workspace`, broad reload, and workspace backups;
- a recovery procedure for pre-commit failures and the rare post-commit/restart case.

Remove or correct stale claims about `sessions.sqlite3`, directory-backed runtime
reload, workspace paths, and backup directories.

### 8.2 Update developer and design relationships

- Update source-area READMEs that describe session database ownership, application
  configuration, or workspace reload.
- Add the supersession note to `docs/session-design.md`:
  - this unified design supersedes its storage layout, per-session database lease,
    physical-deletion database lifecycle, and database path ownership;
  - its session rename/delete UI behavior, label rules, and live-session coordination
    remain applicable unless separately changed.
- Keep `docs/design.md` as the authoritative detailed design and this file as the
  implementation sequence. Resolve inconsistencies rather than duplicating divergent
  explanations.
- Update filenames and command examples in contributor/testing documentation.

### 8.3 Verify the operator cutover exactly

Use copies of realistic data, never the only production files:

1. Stop the old service and preserve a recoverable backup of its workspace and
   version-1 unified session database.
2. If per-session legacy databases exist, run the archived migration build first and
   verify that the guard blocks import until migration/cleanup is complete.
3. Run `chaweb --data DATABASE --import WORKSPACE`.
4. Inspect the command summary and verify schema version 2, session counts, private
   database/sidecar permissions, and stored required rows.
5. Run `chaweb --data DATABASE --export EXPORTED_WORKSPACE` and compare accepted
   files byte-for-byte with the source.
6. Move the original workspace aside and start normal runtime using only the
   database.
7. Exercise session open/resume, character settings, forum defaults, restart, and
   export after edits.
8. Confirm a concurrent import/export is rejected while runtime holds the lease.
9. Confirm a second import replaces configuration while preserving all sessions.

Do not delete the old backup as part of implementation. Its retention is an operator
decision after the new build has been accepted.

### 8.4 Final definition of done

The change is complete when all of the following are true:

- All native, webapp, process, integration, sanitizer, and applicable platform tests
  pass.
- `git diff --check` is clean and generated artifacts match their sources.
- Repository searches find no executable use of the removed `--config` or
  `--workspace` options, workspace reload, `backup_dir`, directory-derived session
  database paths, generation/type/control configuration columns, or runtime legacy
  detection.
- A new database can be created only through successful import.
- A version-1 database upgrades only through successful import and preserves
  sessions exactly.
- Normal runtime and export reject missing, foreign, malformed, or version-1
  databases without modifying them.
- Runtime succeeds after the original workspace is unavailable.
- All supported runtime edits survive restart and export.
- Database, sidecars, private roots, and exported `.env` satisfy the private-access
  requirements on supported platforms.
- The operator documentation describes one storage model and one safe cutover path.

## Suggested commit boundaries

One reviewed commit per block is the simplest history. If a block needs more than one
commit while being implemented, squash or organize by the block's internal sections
without mixing later-block behavior. In particular:

- do not combine generated protocol removal with unrelated schema code;
- do not land the CLI cutover without its runtime mutation wiring;
- do not treat documentation or packaging as optional follow-up after Block 8;
- do not delete legacy operator data or run migration against non-disposable data as
  part of automated implementation.
