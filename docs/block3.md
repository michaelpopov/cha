# Block 3 job — Offline workspace configuration store

## Mission

Implement Block 3 from `docs/plan.md`: add the configuration-store component that
converts an import directory into SQLite `config` rows, validates the exact stored
representation, upgrades/updates the database atomically, and exports rows back to a
directory.

Implement and test import/export as direct C++ operations. Do not wire the public
`--data`, `--import`, or `--export` command line yet.

## Source of truth and prerequisites

- Blocks 1 and 2 must already be implemented and passing.
- Read `docs/design.md`, especially **Stored configuration tree**, **Application
  configuration**, **Import**, **Export**, **Database creation and upgrade**, and
  **New workspace configuration store**.
- Read Block 3 and the global invariants in `docs/plan.md`.
- Preserve unrelated worktree changes.

The most important correctness rule is that import validates the byte-identical
materialized rows it will commit, not the richer source directory.

## Required result

At the end of this job, a direct C++ caller can:

- import a valid directory into a missing, v1, or v2 CHA database;
- preserve all existing sessions while upgrading/replacing configuration;
- export committed configuration to a missing or empty directory;
- reject unsafe stored paths and matching symlinks;
- validate providers using API keys present only in imported `.env`;
- reject includes that depend on excluded source files; and
- do all database work under the existing non-blocking lease.

## Expected implementation areas

- new `src/workspace/workspace_config_store.h`
- new `src/workspace/workspace_config_store.cpp`
- `src/web/application_config.*` for a side-effect-free stored `app.toml` parser
- `src/util/environment.*` and private-filesystem utilities from Block 1
- database primitives from Block 2
- store unit tests and `CMakeLists.txt` registration

Keep the store concrete. Do not add a virtual filesystem, generic repository
interface, migration registry, or configuration history.

## Work 1 — Define the store boundary

The store is responsible for:

- acquiring/holding the database lease for offline operations;
- securing database files before SQLite opens them;
- scanning a directory into sorted private `(name, content)` rows;
- validating portable stored names;
- materializing rows safely beneath a supplied private directory;
- parsing and validating the exact materialization;
- creating/upgrading/replacing database configuration atomically;
- exporting rows; and
- returning concise counts and path-specific errors.

It is not responsible for session queries, HTTP routes, UI state, provider
implementation, template parsing, or workspace semantics. Reuse `Workspace::load`.

## Work 2 — Collect the import tree

Resolve source and database paths to absolute normalized paths. Require the source
directory and database parent to exist.

Walk the source recursively without following symbolic links. Store only:

- required root `app.toml`;
- required root `workspace.toml`;
- every regular file ending in `.toml`;
- every regular file ending in `.md`; and
- optional root `.env`.

Rules:

- Reject a symbolic link that would otherwise match the stored set.
- Do not traverse a symlinked directory.
- Ignore ordinary files with unsupported names/extensions.
- Ignore databases, SQLite sidecars, `.cha-lock`, logs, backups, and editor files.
- The source is allowed to contain the target SQLite database; it is ignored by the
  accepted-file filter.
- Empty source directories are not stored.
- Convert relative names to `/` separators and sort by name.
- Require `app.toml` and `workspace.toml` before any target database open or write.
- Read contents byte-for-byte. Bind SQLite `TEXT` with explicit byte counts; preserve
  embedded NULs, arbitrary bytes, and newlines. Do not add UTF-8 validation.

Every stored name must be:

- non-empty and relative;
- made of non-empty components other than `.` or `..`;
- separated only by `/` and contain no backslash;
- portable against platform-specific absolute path forms;
- unique; and
- a `.toml`/`.md` name or exactly root `.env`.

Validate names on import and every database read, in addition to the table `CHECK`.

## Work 3 — Implement one shared safe materializer

Before writing anything, validate the complete row set for names, duplicates,
required files, and file/directory collisions.

Materialization must:

1. join only validated names beneath the supplied destination;
2. verify containment;
3. create the required empty skeleton:

   ```text
   system/providers/
   personas/
   characters/
   forums/
   ```

4. create implied parent directories;
5. write ordinary files without following symlinks or converting newlines; and
6. create `.env` atomically with owner-only access.

The private parent protects other runtime materialized files. They must not become
group/other writable.

Use this exact materializer for import validation, runtime startup in Block 4, and
export. Do not implement a separate source-tree validator.

## Work 4 — Extract stored application settings parsing

Provide a side-effect-free parser for materialized root `app.toml`:

```toml
host = "127.0.0.1"
port = 8080
```

`host` and `port` are required and validated. Stored settings do not accept or retain
the old `workspace` or `backup_dir` fields. Keep the current application parser entry
point as a thin staging wrapper until Block 6, but do not duplicate TOML validation.

## Work 5 — Implement import in this exact order

1. Normalize source/database paths and require the source and database parent.
2. Acquire `DATABASE.cha-lock` non-blockingly.
3. Run `has_legacy_session_databases(source)` unconditionally.
4. If legacy session databases exist, fail before any database change:
   - missing target: archived per-session-to-workspace migration was never run;
   - present target: migration cleanup is incomplete.
5. Collect and validate the accepted rows.
6. Create an atomically private, short-lived validation root with `workspace/`.
7. Materialize the collected rows using the shared materializer.
8. Parse materialized `app.toml` and `.env` without environment mutation.
9. Apply `.env` with Block 1's scoped, non-overwriting overlay.
10. Call `Workspace::load(validation_workspace, database_parent)` while the overlay
    is active.
11. Restore the environment and remove the validation root on return or exception.
12. Only after semantic validation succeeds, secure existing database/sidecar files
    and open/inspect SQLite.
13. Create missing v2, upgrade valid v1, or prepare valid v2 replacement.
14. In one immediate transaction, create/delete as appropriate, insert the complete
    row set, set version 2 when creating/upgrading, and commit.
15. Verify newly created sidecar permissions and report the imported file count.

Candidate validation must occur before opening, chmodding, or writing the target
database. The lease is intentionally held during validation.

Required consequences:

- A provider key found only in source `.env` satisfies import validation and is gone
  from the caller's environment afterward.
- An inherited environment value, including an inherited empty value, is not
  overwritten by `.env`.
- `{{include shared/snippet.txt}}` fails even if that file exists in the source,
  because `.txt` is excluded from the materialization.
- An include of collected `.md` content succeeds.
- Semantic validation failure never creates, secures, opens, or changes the target.
- Transaction failure exposes only the prior complete database/configuration.
- Existing sessions are never changed by import.

## Work 6 — Implement export

Export must:

1. normalize paths and acquire the same lease;
2. secure and completely validate an existing v2 database;
3. read rows ordered by name and validate every name;
4. require the destination to be absent or an existing empty directory;
5. reject a non-empty destination before writing;
6. create a missing destination, known skeleton, parents, and files with the shared
   materializer; and
7. report the exported file count.

Never overwrite an existing file. Export is configuration-only; it does not copy
sessions or database sidecars. If an I/O error occurs after output begins, leave the
database unchanged and report that the destination may be incomplete and must be
emptied before retry. Do not recursively delete pre-existing or unresolved paths.

## Required tests

Cover at least:

- import into missing, populated-v1, and populated-v2 databases;
- session/label/turn/entry preservation across upgrade and replacement;
- import/export byte-exact round trip, including Unicode, arbitrary bytes, embedded
  NULs, empty files, nested TOML/Markdown, and root `.env`;
- required empty skeleton recreation;
- missing required files;
- unsafe, duplicate, unsupported, absolute, dot, dot-dot, backslash, and colliding
  stored names;
- matching symlinks and symlinked directories;
- `.env`-only provider keys, inherited precedence, and exception-safe restoration;
- accepted collected `.md` include and rejected excluded `.txt` include;
- malformed consumed TOML, template, dotenv, and workspace configuration leaving the
  target untouched;
- v1/v2 transaction failure rollback;
- unconditional legacy detector with both target-existence messages;
- export to missing and empty destinations;
- rejection of non-empty destination, v1, foreign, and malformed databases;
- incomplete-output reporting on export I/O failure;
- import/export lock contention; and
- private database/sidecar, validation-root, and exported `.env` permissions.

Run focused store/session/workspace tests, compile all affected targets, and run
`git diff --check`.

## Out of scope

- Public CLI dispatch
- Normal-runtime private root lifetime
- Runtime configuration mutations
- SessionRepository ownership changes
- Reload/backup/API removal
- General documentation updates

## Completion gate

This job is complete when direct C++ import/export operations satisfy the full
ordering above, source-only excluded files cannot make validation pass, v1 sessions
are preserved, environment state is restored, and exported content is byte-exact.

In the final session report, list changed files, tests run, and all failure paths
that were explicitly exercised.
