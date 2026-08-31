# Block 6 job — CLI and runtime cutover to the unified database

## Mission

Implement Block 6 from `docs/plan.md`: make `--data DATABASE` the only durable-data
entry point, wire offline import/export into `web_main`, construct normal runtime
from database materialization, simplify `SessionRepository` ownership, and route the
three supported runtime edits through the atomic store operation.

This block establishes the first functionally complete database-only runtime. Do not
add backward compatibility for the old CLI or directory runtime.

## Source of truth and prerequisites

- Blocks 1 through 5 must already be implemented and passing.
- Read `docs/design.md`, especially **Command line**, **Runtime startup**,
  **Application configuration**, **Database lease and WAL**, **Runtime configuration
  mutations**, and the file-specific required changes.
- Read Block 6 and the global invariants in `docs/plan.md`.
- Preserve unrelated worktree changes.

## Required result

At the end of this job:

- every mode requires `--data DATABASE`;
- `--config` and `--workspace` are rejected and have no compatibility aliases;
- import/export run without initializing server/provider/session state;
- normal runtime reads configuration only from v2 SQLite rows;
- one top-level owner holds the lease and private `workspace/`/`welcome/` root;
- `SessionRepository` receives an explicit database path and owns neither lease nor
  runtime root;
- logging initializes after workspace load using the database parent path base;
- all three narrow runtime configuration edits commit through SQLite and survive
  restart/export; and
- missing/v1/foreign/malformed databases fail without runtime creation or upgrade.

## Expected implementation areas

- `src/web/application_config.*`
- `src/web_main.cpp`
- `src/workspace/workspace_config_store.*`
- `src/session/session_repository.*`
- `src/session/session_storage_layout.*`
- `src/workspace/session_open.*`
- `src/web/lobby_routes.*`
- native process/unit/integration fixtures, especially `WebServerProcess`
- directly affected examples/help text embedded in code

## Work 1 — Separate CLI arguments from stored settings

Support exactly these public modes:

```text
chaweb --data DATABASE [--root PATH] [--host HOST] [--port PORT]
chaweb --data DATABASE --import SOURCE_DIRECTORY
chaweb --data DATABASE --export DESTINATION_DIRECTORY
```

Rules:

- `--data` is required exactly once in every mode and names the SQLite file.
- Resolve database and import/export directory paths to absolute normalized paths.
- `--import` and `--export` are mutually exclusive.
- `--root`, `--host`, `--port`, and hidden `--test-idle-grace-ms` are runtime-only
  and rejected in offline modes.
- `--root` remains the installed web-assets path and defaults as it does today. It is
  not workspace data.
- Remove `--config` and `--workspace`; reject them as unknown/removed options.
- Do not add an alias or infer the old invocation form.
- Stored root `app.toml` requires host and port; CLI host/port override them only in
  normal runtime.
- Help/errors must show mode-specific `--data` examples and actionable missing/v1
  import guidance.

Keep command-line values and stored application settings as separate value types or
clearly separate parsing stages. Do not fabricate an old argument vector.

## Work 2 — Dispatch offline modes immediately

In `web_main`, after parsing CLI paths:

- Import calls the tested Block 3 import operation, reports the imported file count,
  and exits.
- Export calls the tested Block 3 export operation, reports the exported file count,
  and exits.
- Neither mode binds a port nor initializes logging, providers, HTTP threads,
  sessions, forum synchronization, or the Welcome database.
- Lock, schema, permission, validation, and filesystem errors produce nonzero exit
  status with a useful path/state message.

Do not duplicate validation or transaction logic in `web_main`.

## Work 3 — Construct normal runtime in the required order

Replace directory-backed startup with this order:

1. parse CLI and require `--data`;
2. construct the runtime store and acquire the process-lifetime lease;
3. secure the existing database/sidecars;
4. require and completely validate schema v2, initialize WAL, and verify sidecars;
5. create one owner-only root with stable `workspace/` and `welcome/` children;
6. materialize committed configuration and known skeleton into `workspace/`;
7. apply materialized `.env` without overwriting inherited variables;
8. load stored `app.toml` and apply CLI host/port overrides;
9. call `Workspace::load(workspace_child, database_parent)`;
10. initialize logging from the loaded workspace—relative `logging.file` must resolve
    under the database parent, not the temporary root;
11. construct `SessionRepository` with explicit paths;
12. synchronize forums and initialize providers, sessions, and web server; and
13. on shutdown, destroy all users before the runtime owner releases the lease and
    removes the one private root.

Normal runtime never creates or upgrades a database and never reads the original
import/export directory.

## Work 4 — Simplify `SessionRepository` ownership

Change construction to receive:

- the explicit unified database path (or already managed handle as designed);
- the stable materialized `workspace/` path used by existing identity comparisons;
  and
- the already-private `welcome/` directory.

Remove from `SessionRepository`:

- lease acquisition/ownership;
- random `cha-session-*` root creation;
- private-root recursive cleanup;
- derivation of `<workspace>/workspace.sqlite3`; and
- runtime legacy-session-directory detection.

Preserve session CRUD, forum synchronization, Welcome storage, maintenance fencing,
workspace identity comparisons, WAL/checkpointing, and shutdown behavior.

Delete `workspace_session_database_path()` and its focused tests once all callers
receive the path explicitly. If `session_storage_layout.*` then contains only
`has_legacy_session_databases()`, keep it under a clear narrow name or rename it with
the smallest practical diff. No directory-derived session database path helper may
remain. The legacy detector itself remains for import only.

`SessionRepository` must not remove or own the materialized workspace/private root.

## Work 5 — Wire supported runtime edits through the store

Inject the runtime store/edit capability only into:

- character provider/style persistence in `lobby_routes`;
- forum default-character persistence in `session_open`; and
- forum default-persona persistence in `session_open`.

For each path:

- retain existing request validation and narrow TOML writer;
- execute the writer through Block 4's serialized edit operation;
- validate candidate, replace all config rows, commit, then publish with existing
  `loadws(Workspace)`;
- request the same affected live-session reloads as current behavior using the
  retained session-specific `reloading` reason; and
- remove direct durable-directory assumptions.

Errors must distinguish:

- pre-commit failure: database and published workspace unchanged, materialized tree
  restored; and
- exceptional post-commit publication failure: durable change committed, restart
  required.

No runtime route edits `.env` or `app.toml`.

## Work 6 — Update directly affected fixtures

- Update application-config tests for all three modes, required/missing/duplicate
  arguments, mutual exclusion, runtime-only rejection, host/port overrides, hidden
  test option, and rejection of removed `--config`/`--workspace`.
- Change process fixtures to build a source workspace, explicitly import it into a
  fresh database, then launch with `--data DATABASE`.
- Update all `SessionRepository` construction in tests.
- Update the three mutation test paths to assert database rows and restart/export
  persistence rather than a durable source-tree write.
- Assert normal runtime rejects missing/v1 databases with import guidance.

## Required verification

Run affected native unit, server, process, and integration tests plus
`git diff --check`.

Perform this disposable manual smoke test:

1. Import a representative workspace into a new database.
2. Rename/remove the source directory.
3. Start `chaweb --data DATABASE` and open/resume a session.
4. Exercise character provider/style and both forum default edits.
5. Stop and restart.
6. Confirm sessions/settings survive.
7. Export and inspect the resulting configuration.
8. Confirm runtime blocks concurrent import/export.

Search executable code/tests updated here for old `--config`, `--workspace`,
directory-derived database paths, and runtime legacy detection. Intentional old-option
rejection tests may remain.

## Out of scope

- Broad workspace reload/backup, already removed in Block 5
- Automatic import at startup
- Backward-compatible CLI aliases
- Database backup functionality
- Full packaging/sample audit (Block 7)
- General user/source documentation rewrite (Block 8)

## Completion gate

This job is complete only when a server can start and operate after the original
workspace is unavailable, every supported edit survives restart/export, the database
lease/private root have one top-level owner, and no normal path can create, upgrade,
or bypass the unified database.

In the final session report, list changed interfaces, commands/tests run, smoke-test
results, and any remaining old-option matches with justification.
