# Block 4 job — Runtime ownership and atomic configuration edits

## Mission

Implement Block 4 from `docs/plan.md`: add and directly test the normal-runtime
storage owner, single private temporary tree, database-backed workspace startup, and
serialized atomic configuration-edit operation.

Do not change the public CLI or production route wiring yet. Block 6 will connect
these tested primitives to `web_main`, `SessionRepository`, and HTTP behavior.

## Source of truth and prerequisites

- Blocks 1 through 3 must already be implemented and passing.
- Read `docs/design.md`, especially **Runtime startup**, **Workspace loading and
  relative paths**, **Runtime configuration mutations**, and **Failure and crash
  consistency**.
- Read Block 4 and the global invariants in `docs/plan.md`.
- Inspect all current uses of `Workspace::root()` and the three narrow workspace
  writers before implementing restoration.
- Preserve unrelated worktree changes.

## Required result

At the end of this job, direct C++ tests can construct one runtime owner that:

- holds the database lease for its full lifetime;
- owns one private root with `workspace/` and `welcome/` children;
- materializes and loads configuration exclusively from v2 database rows;
- resolves durable relative paths against the database parent;
- serializes the complete runtime edit/commit/publication sequence; and
- restores failed edits in place without invalidating the stable workspace path.

## Expected implementation areas

- `src/workspace/workspace_config_store.*`
- workspace loading/publication functions and their tests
- the private-filesystem and environment helpers from Block 1
- database primitives/materializer from Blocks 2 and 3
- focused runtime-store tests

Production `web_main`, session repository construction, and route injection are
Block 6 responsibilities.

## Safety invariant that must be proven first

In-place rollback is safe only under this invariant:

- a published `Workspace` eagerly owns every parsed value;
- after `Workspace::load` returns, ordinary readers never reopen configuration below
  `Workspace::root()`;
- `SessionRepository` may compare the stable root path as an identity value but does
  not read configuration through it; and
- after publication, filesystem access below the materialized tree is confined to
  candidate `Workspace::load`, the three narrow writers, and store restoration, all
  while the process configuration mutex is held.

Before implementing rollback:

1. Re-audit every `Workspace::root()` use.
2. Find every filesystem open that could reach the materialized workspace.
3. Verify the four current `SessionRepository` checks are path comparisons only.
4. Verify providers, live sessions, templates, and published workspaces own parsed
   values rather than lazily reopening source files.

If any asynchronous or unlocked read reaches the materialized tree, stop and update
the design before implementing in-place restoration. Do not paper over a torn-state
race with retries.

## Work 1 — Add the normal-runtime owner

Add a concrete runtime-mode owner, preferably within `WorkspaceConfigStore`, whose
declaration/destruction order covers:

1. the database lock-file lease;
2. secured SQLite connection/repository-facing handle;
3. one process-private temporary root;
4. stable `workspace/` and `welcome/` children; and
5. one process-local configuration mutation mutex.

Requirements:

- Acquire the lease before permission checks, schema inspection, row reads,
  materialization, dotenv, or workspace loading.
- Secure/validate an existing v2 database and initialize/verify WAL sidecars.
- Runtime rejects missing, v1, foreign, malformed, or incomplete databases and never
  creates/upgrades one.
- Create the root atomically with owner-only access, then create both children below
  it.
- Materialize all committed rows and the known skeleton into `workspace/` once.
- Expose stable child paths without transferring ownership.
- One owner removes the complete root after all users have shut down.
- Never reuse an orphaned root from a previous process; each startup creates a fresh
  one.

## Work 2 — Load runtime configuration from the materialization

Provide an operation that:

1. applies materialized `.env` with current startup non-overwrite semantics;
2. parses required stored `app.toml` host/port;
3. loads `Workspace::load(workspace_child, database_parent)`; and
4. returns/retains the values required for Block 6 startup.

Template includes remain rooted in the private workspace. Relative `logging.file`
resolves against the database parent. `Workspace::root()` remains the stable
materialized `workspace/` path.

## Work 3 — Implement one serialized runtime edit operation

Place the entire mutation flow behind one store operation. Under the single
configuration mutex:

1. obtain the current published workspace;
2. invoke one existing narrow file writer on the materialized tree;
3. load and validate a complete candidate with
   `Workspace::load(workspace_child, database_parent)`;
4. collect the complete accepted configuration set from that tree;
5. begin one short SQLite write transaction;
6. delete all `config` rows and insert the complete candidate set;
7. commit;
8. publish through the existing `loadws(Workspace)` overload; and
9. return enough information for the caller to request its existing affected-session
   reload behavior.

Supported writers are only:

- non-built-in character provider/style;
- forum default character; and
- forum default persona.

Do not add changed-file tracking. Configuration is small; replace the whole table.
Do not add a preallocated publication overload. The existing post-commit allocation
window is recoverable by restart.

Do not reload `.env` or `app.toml` after a narrow runtime edit. They have no runtime
editor and change only through offline import followed by restart.

## Work 4 — Implement failure recovery

For a failure before commit—including writer, candidate validation, row collection,
SQLite begin/write, or commit failure:

- published workspace remains old;
- committed database remains old;
- before releasing the configuration mutex, rematerialize committed rows in place;
- keep the existing `workspace/` directory itself and its path;
- delete/recreate only its contents and required child skeleton;
- never rename, remove, or replace the `workspace/` directory; and
- propagate the original error, adding restoration failure information if needed.

If restoration fails, require process restart.

For process death or publication allocation failure after SQLite commit:

- the complete new configuration remains durable;
- do not attempt a compensating transaction; and
- report restart required; the next startup publishes the committed state.

Keep filesystem editing/validation outside the SQLite transaction so the write lock
is held only for row replacement and commit.

## Required tests

Cover at least:

- one owner-only root with `workspace/` and `welcome/` children;
- one cleanup owner and one orphan explanation;
- lease lifetime and rejection of a second runtime/import/export holder;
- startup after deleting the original import tree;
- startup ignoring/replacing an orphaned prior temporary tree;
- database-parent relative logging resolution;
- successful edit updating materialized files, database rows, and published
  workspace together;
- serialization of two edits and edit/read interaction;
- candidate validation failure restoring old contents;
- row collection, SQLite write, and commit failures restoring old contents;
- restoration keeping the same `workspace/` directory rather than replacing it;
- published values and session root-identity comparisons staying valid during
  restoration;
- simulated post-commit publication failure returning restart-required state; and
- restart publishing that committed configuration.

Compile all affected targets, run focused runtime-store/workspace/session tests, and
run `git diff --check`.

## Out of scope

- New command-line parsing or `web_main` dispatch
- Production `SessionRepository` constructor changes
- Production route/session-open injection
- Broad reload/backup/API removal
- Packaging or user documentation

## Completion gate

This job is complete only when the eager-ownership invariant has been reverified,
the runtime owner and edit engine work through direct tests, rollback rewrites only
the stable directory's contents, and every pre/post-commit failure has the designed
durable result.

In the final session report, explicitly summarize the root-use audit, changed files,
tests run, and any failure injection not executable locally.
