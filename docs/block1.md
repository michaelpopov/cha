# Block 1 job — Security and workspace-loading foundations

## Mission

Implement Block 1 from `docs/plan.md`: add the private-filesystem, dotenv, and
workspace-loading seams required by the unified database design. This block must not
change the public command line or switch runtime away from its current directory
workspace.

The result should be a buildable, tested, mostly additive foundation for later
blocks. Prefer small concrete functions over new frameworks.

## Source of truth and prerequisites

- Read `docs/design.md`, especially **Secret-bearing file permissions**,
  **Application configuration**, **Workspace loading and relative paths**, and the
  corresponding entries under **Required code changes**.
- Read Block 1 and the global invariants in `docs/plan.md`.
- No earlier implementation block is required.
- Inspect the current worktree before editing and preserve unrelated changes.
- Use `local-investigate` for broad repository questions and verify important
  findings in the referenced source.

## Required result

At the end of this job:

- security-sensitive directories are private from their first successful creation;
- bundled SQLite defaults new database files to owner-only permissions;
- dotenv parsing can run without changing the process environment;
- import code can later apply dotenv entries through a scoped, non-overwriting
  environment overlay;
- `Workspace::load` can use separate physical and durable-relative-path roots; and
- `Workspace::load(directory)` remains available for parser unit tests and delegates
  to the new implementation.

## Expected implementation areas

- `src/util/environment.*`
- a small new private-filesystem utility under `src/util/`, named according to local
  convention
- `src/workspace/workspace.*`
- the current private Welcome-directory creation in `src/session/session_repository.*`
- SQLite target compile definitions in `CMakeLists.txt` or its included build files
- focused utility, workspace, and session tests

Do not move unrelated code merely to match this list.

## Work 1 — Establish the baseline

1. Find every call site of:
   - `Workspace::load`;
   - dotenv loading/application;
   - the current private temporary-directory helper;
   - `SessionRepository` construction.
2. Identify which configured paths are resolved relative to the workspace today,
   especially `logging.file`.
3. Run the smallest existing workspace, environment, session, and application-config
   test set that covers those paths. Record any pre-existing failure before editing.

## Work 2 — Add a narrow private-filesystem utility

Implement only the operations required by this design:

- atomically create a private directory;
- atomically create or replace an ordinary private file when CHA creates the file;
- tighten and verify an existing database or SQLite sidecar as owner-only; and
- reject symbolic links and unexpected file types when a caller requires a regular
  file or directory.

POSIX requirements:

- Create a private directory with `mkdir(..., 0700)`. Do not create with default
  permissions and apply `chmod` afterward.
- Create secret files with `0600` from the first successful open.
- Permission tightening must verify the final mode and fail if it cannot establish
  owner-only access.
- Do not change the process-wide umask.

Windows requirements:

- Establish an equivalent current-user-private security descriptor/DACL.
- Do not pretend POSIX mode bits provide Windows protection.
- Failure to establish private access is fatal, not a warning.

Use this helper immediately for the current Welcome database directory so the
existing create-then-chmod window is removed in this block. Its ownership moves to
the top-level runtime store in Block 6.

Compile bundled SQLite with:

```text
SQLITE_DEFAULT_FILE_PERMISSIONS=0600
```

Apply the definition to the SQLite library itself, not only to CHA code that includes
SQLite headers.

## Work 3 — Separate dotenv parsing from application

Refactor the current environment utility without changing existing runtime behavior:

1. Parse `.env` into an ordered representation without modifying the process
   environment.
2. Apply parsed entries using the current startup policy.
3. Add a scoped overlay used later by import validation.

The scoped overlay must:

- install only names absent from the inherited environment;
- preserve the current behavior for inherited variables whose value is empty;
- preserve entry order and repeated-name behavior;
- expose inserted values through `std::getenv` while the scope is active;
- remove every value it inserted on normal return or exception; and
- leave pre-existing and unrelated variables untouched.

Do not add a provider lookup interface or general environment dependency-injection
system. The overlay exists so the unchanged provider validator can see imported
dotenv values during `Workspace::load`.

## Work 4 — Separate workspace root from durable path base

Add:

```text
Workspace::load(materialized_root, durable_relative_path_base)
```

Required behavior:

- configuration discovery, Markdown/TOML reads, and template include containment use
  `materialized_root`;
- durable relative output paths such as `logging.file` use
  `durable_relative_path_base`;
- absolute configured paths retain their current behavior;
- `Workspace::root()` continues to identify the physical workspace root; and
- `Workspace::load(directory)` is retained for parser-focused unit tests and
  delegates using `directory` as both arguments.

Keep one loading implementation. Do not add a virtual filesystem or parallel parser.

## Required tests

Add or update focused tests for:

- POSIX private modes existing at creation time;
- symlink and wrong-file-type rejection;
- the SQLite target's private default file mode;
- dotenv parsing with no environment side effects;
- scoped overlay precedence, repeated entries, inherited empty values, and cleanup;
- cleanup when provider/workspace validation throws;
- a provider API key supplied only by `.env` being visible inside the overlay;
- template includes resolving under the physical root; and
- a relative logging path resolving under a distinct durable base.

Compile every target affected by the interface change. Follow surrounding C++ style;
the repository has no general C++ formatter. Run `git diff --check`.

## Out of scope

- The `config` SQLite table or schema version 2
- Import/export directory traversal
- The new `--data` command-line option
- Runtime materialization or storage cutover
- Workspace reload/backup removal
- General documentation updates

## Completion gate

This job is complete only when existing directory-based startup behavior is
unchanged, all focused tests pass, the one-argument loader remains tested, and no
security-sensitive directory introduced or touched here has a create-then-tighten
window.

In the final session report, list changed files, tests run, and any platform-specific
permission behavior that could not be executed locally.
