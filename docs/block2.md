# Block 2 job — Unified schema and database primitives

## Mission

Implement Block 2 from `docs/plan.md`: add schema version 2, the strict two-column
`config` table, explicit version-1 inspection/upgrade primitives, byte-exact row
operations, and private database-opening behavior.

Do not implement directory import/export or the final CLI in this block.

## Source of truth and prerequisites

- Block 1 must already be implemented and passing.
- Read `docs/design.md`, especially **Database schema and version**, **Database
  creation and upgrade**, **Database lease and WAL**, and **Secret-bearing file
  permissions**.
- Read Block 2 and the global invariants in `docs/plan.md`.
- Inspect the current database creation, identity validation, WAL initialization,
  integrity checks, and session schema tests before editing.
- Preserve the existing database application ID and all session semantics.

## Required result

At the end of this job, the database layer can:

- distinguish missing, valid v1, valid v2, foreign, corrupt, and unsupported
  databases;
- create a complete schema-v2 database;
- validate all required v2 session and configuration objects;
- upgrade v1 to v2 transactionally when called by import;
- read and replace sorted `(name, content)` rows without changing content bytes; and
- secure an existing database and SQLite sidecars before opening them.

Normal runtime must never silently upgrade v1.

## Expected implementation areas

- `src/session/workspace_session_database.*`
- the SQLite storage/binding helpers used by that layer
- the private-filesystem helper from Block 1
- session database/repository unit tests
- relevant build definitions

Keep row representation private and small, for example:

```cpp
struct ConfigFile {
    std::string name;
    std::string content;
};
```

Do not create a public storage-interface hierarchy.

## Work 1 — Define schema version 2 once

Bump the workspace session database schema version from 1 to 2 and add this exact
table definition to the authoritative schema source:

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

Requirements:

- Reuse the same table definition for new-v2 creation and v1 upgrade.
- A database claiming v2 must contain the exact required session and configuration
  schema objects.
- Do not add `generation`, `type`, `config_control`, revision, timestamp, or hash
  fields.
- `PRAGMA user_version` is a schema version only and does not change with config
  edits.
- Higher layers require `app.toml` and `workspace.toml`; the table itself need not
  encode that rule.

## Work 2 — Separate database state inspection from mutation

Add a read-only identity/schema probe that distinguishes:

- missing database;
- valid CHA v1 session database;
- valid CHA v2 unified database;
- wrong application ID;
- unsupported past/future version;
- corrupt or structurally incomplete database.

Land the v1-specific normal-runtime diagnostic now, before the generic unsupported
schema branch. It must identify the file as a valid schema-1 CHA database and tell
the operator to stop CHA and run:

```text
chaweb --data DATABASE --import WORKSPACE
```

Do not describe valid v1 as foreign or corrupt.

The import entry point added later may create missing v2, upgrade valid v1, or
replace valid v2. Runtime/export ultimately accept only valid v2.

Intermediate-block policy:

- The directory-based development runtime may temporarily retain its existing
  missing-database creation path through Block 5 so a disposable local/CI database
  can be recreated at v2.
- Never delete a data-bearing v1 database. Preserve it for the Block 6 import
  upgrade.
- Only disposable databases with no sessions worth keeping may be deleted and
  recreated during this staging period.

## Work 3 — Implement the explicit v1-to-v2 upgrade

Use one direct upgrade function, not a migration framework.

The upgrade must:

1. validate the existing application ID and complete v1 session schema;
2. begin one SQLite transaction;
3. create `config` using the authoritative definition;
4. insert the caller's complete imported row set;
5. set `PRAGMA user_version = 2` last; and
6. commit.

Any failure must roll back to a valid v1 database with every session, label, turn,
entry, and timestamp unchanged.

For a missing database, the eventual import path creates all session tables plus
`config` and imported rows as one complete v2 result. If creation fails, the caller
must be able to remove only the newly created database and its SQLite sidecars and
retry. Do not expose a valid-looking empty configuration as a successful import.

## Work 4 — Add byte-exact row operations

Provide minimal operations to:

- read all `config` rows ordered by `name`;
- replace all rows with `DELETE` plus bound inserts inside the caller's transaction;
- validate stored names at the application boundary; and
- construct/bind content using explicit byte counts.

Content requirements:

- preserve empty strings, embedded NULs, non-UTF-8 bytes, and newline bytes exactly;
- do not perform UTF-8 or encoding validation;
- continue binding paths and contents as SQL parameters; and
- leave semantic TOML/Markdown validation to existing parsers.

The SQL `CHECK` is defense in depth. Later application validation also rejects empty,
dot, empty-component, unsupported-extension, platform-absolute, and otherwise
non-portable names.

## Work 5 — Secure durable SQLite files

Through one shared opening path used later by runtime/import/export:

1. acquire the CHA lock-file lease;
2. before opening SQLite, inspect any existing main database, `-wal`, `-shm`, and
   `-journal` paths;
3. reject symlinks and non-regular objects;
4. tighten them to owner-only access and verify the result;
5. open/initialize SQLite; and
6. after WAL initialization, verify sidecar privacy again.

Use Block 1's SQLite compile-time default for newly created files. Do not rely on the
process umask.

## Required tests

Cover at least:

- creation and complete validation of v2;
- missing/wrong v2 objects and columns;
- wrong application ID, corruption, and future version rejection;
- valid v1 producing the specific import instruction at runtime;
- generic unsupported schema remaining distinct from the v1 diagnostic;
- direct SQL rejection of empty, leading-`/`, backslash, and `..` names covered by
  the table check;
- deterministic sorted reads and atomic whole-table replacement;
- byte-exact empty, embedded-NUL, and non-UTF-8 content round trips;
- successful v1 upgrade preserving all session data;
- injected upgrade failure leaving a valid unchanged v1 database;
- existing database/sidecar permission tightening; and
- owner-only modes for a newly created database, WAL, and SHM.

Run session database and repository tests, compile all affected targets, and run
`git diff --check`.

## Out of scope

- Directory scanning, materialization, or semantic workspace validation
- Import/export CLI dispatch
- Runtime private workspace ownership
- Runtime config mutations
- Broad reload removal

## Completion gate

This job is complete only when v1 and v2 are intentionally distinguishable, no
ordinary runtime call upgrades v1, the upgrade transaction is failure-tested, row
contents are byte-exact, and durable SQLite files follow the private-access policy.

In the final session report, list schema/API changes, tests run, and how disposable
versus data-bearing v1 databases were handled.
