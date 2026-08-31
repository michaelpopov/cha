# Block 8 job — Documentation and release cutover verification

## Mission

Implement Block 8 from `docs/plan.md`: update all user/developer documentation to
describe the unified database and `--data` interface, establish the relationship to
the older session design, and perform final release/cutover verification on
disposable copies.

This block completes the feature. It is not optional follow-up documentation.

## Source of truth and prerequisites

- Blocks 1 through 7 must already be implemented and passing.
- Read all of `docs/design.md`, then Block 8 and the global invariants in
  `docs/plan.md`.
- Inspect actual `--help`, current sample/package behavior, and passing tests before
  writing instructions. Documentation must describe implemented behavior, not an
  earlier draft.
- Preserve unrelated worktree changes.
- Never run migration/import/cutover tests against the only copy of real operator
  data.

## Required result

At the end of this job:

- every user-facing command uses `--data DATABASE`;
- removed `--config`, `--workspace`, broad reload, and `backup_dir` behavior is not
  presented as available;
- import/export, file filters, permissions, lease behavior, and v1 upgrade are
  accurately documented;
- `docs/session-design.md` clearly states which older decisions are superseded;
- source/module documentation describes top-level lease/private-root ownership;
- the documented operator cutover has been exercised on disposable realistic data;
  and
- the complete definition of done is verified.

## Expected documentation areas

- root `README.md`
- `docs/tutorial.md`
- `docs/users.md`
- `docs/session-design.md`
- `docs/web-ui/api-requirements.md`
- `src/README.md`
- `src/web/README.md`
- `src/workspace/README.md`
- other source/package/sample command documentation found by repository search
- generated documentation only through its authoritative source/generator

Do not change uses of “workspace” that correctly mean the C++ module or logical
configuration tree. Remove only claims that normal runtime reads a durable workspace
directory.

## Work 1 — Document the public command interface

Use exactly:

```text
chaweb --data DATABASE [--root PATH] [--host HOST] [--port PORT]
chaweb --data DATABASE --import DIRECTORY
chaweb --data DATABASE --export DIRECTORY
```

Explain:

- `--data` is mandatory and names the SQLite file containing sessions and current
  configuration;
- old `--config APP_TOML` and `--workspace` options are removed, not aliases;
- `--root` still names installed web assets;
- stored `app.toml` supplies required host/port and CLI values override them;
- import/export are offline and fail immediately while runtime holds the lease; and
- normal startup requires a valid v2 database created/upgraded by import.

Update service, tutorial, package, and troubleshooting examples consistently.

## Work 2 — Document configuration authority and editing

Explain that the `config` table stores `(name, content)` only, with no generation,
type, control, or revision fields. SQLite transaction commit is the atomic
configuration transition.

Document the accepted import set:

- required root `app.toml` and `workspace.toml`;
- all `.toml` and `.md` regular files;
- optional root `.env`;
- no symlink following; and
- no other file type.

State that an include target must itself be in the stored set. For example,
`snippet.txt` is excluded and makes import validation fail; rename it to a stored
`.md` name if appropriate.

Document the manual edit lifecycle:

1. stop CHA;
2. export to a missing or empty directory;
3. edit the directory;
4. import it back; and
5. restart normally.

Explain that runtime materializes a private temporary tree, the original/exported
directory is never consulted by normal execution, and only the three current narrow
settings edits persist online.

Remove instructions for broad workspace reload and workspace `.tar.gz` backup.
State that a safe unified-database backup feature is not part of this change; naive
copying of a live WAL database is unsafe.

## Work 3 — Document secrets, paths, and failure behavior

Explain:

- `.env` is durable content inside the database;
- inherited environment values, including empty values, take precedence;
- import temporarily overlays only absent values for provider validation and restores
  them afterward;
- database, SQLite sidecars, lock, private roots, and exported `.env` require
  owner-only access;
- users must apply secret-grade backup/access discipline to the database;
- relative `logging.file` resolves against the database parent, not the temporary
  materialization; and
- template includes resolve beneath the materialized workspace.

Document failure boundaries:

- invalid import leaves the existing database unchanged;
- v1 upgrade failure leaves valid v1;
- v2 replacement failure leaves old complete configuration;
- runtime edit failure before commit restores the temporary tree and leaves durable
  and published state old;
- rare failure after commit requires restart and loads the new committed state; and
- failed export may leave an incomplete destination that must be emptied before
  retry, but never changes the database.

## Work 4 — Document migration and legacy protection

Document:

- schema v1 is the current unified session database without configuration;
- only `--import` upgrades v1 to v2;
- session data is preserved;
- runtime/export reject v1 with an actionable import instruction; and
- import unconditionally scans the source for legacy per-session databases before
  database modification.

Explain both guard messages:

- target missing means the archived per-session migration build was never run;
- target present means migration cleanup is incomplete.

Do not imply the new build can migrate old per-session databases. It only preserves
already unified v1 sessions while adding configuration.

## Work 5 — Update architecture/source documentation

Describe the final ownership model:

- top-level runtime/configuration store owns the lease, secured database, one private
  root, materialized `workspace/`, `welcome/`, config mutex, and cleanup;
- `SessionRepository` receives explicit database/materialized/welcome paths and owns
  none of those outer resources;
- `Workspace::load` remains the parser and separates physical root from durable path
  base;
- normal reads use eagerly owned published values; and
- runtime mutations replace the whole small config table and publish after commit.

Add a clear supersession notice to `docs/session-design.md`:

Superseded by the unified design:

- storage layout;
- per-session/workspace database lease ownership;
- physical-deletion database lifecycle; and
- database path ownership/derivation.

Still applicable unless separately changed:

- session rename/delete UI behavior;
- label rules; and
- live-session coordination.

Keep `docs/design.md` authoritative for design and `docs/plan.md` plus these block
files as implementation history/job definitions. Resolve contradictions instead of
copying divergent text.

## Work 6 — Exercise the operator cutover

Use recoverable copies of realistic data:

1. Stop CHA.
2. Back up the workspace and v1 `workspace.sqlite3` using a safe offline method.
3. Check `forums/*/sessions/` and `forums/*/sessions/deleted/` for legacy databases.
4. If present, prove new import refuses them. Use the archived migration build on a
   disposable copy, verify its result, and remove legacy files there.
5. Put valid root `app.toml` in the import tree and remove `workspace`/`backup_dir`.
6. Run:

   ```text
   chaweb --data /absolute/path/workspace.sqlite3 \
          --import /absolute/path/workspace
   ```

7. Verify schema v2, session counts/content, config rows, and private database/sidecar
   permissions.
8. Export to a new directory and compare every accepted file byte-for-byte:

   ```text
   chaweb --data /absolute/path/workspace.sqlite3 \
          --export /absolute/path/exported-workspace
   ```

9. Move the original workspace aside and start:

   ```text
   chaweb --data /absolute/path/workspace.sqlite3
   ```

10. Exercise session open/resume, all three narrow settings, restart, and export.
11. Confirm concurrent import/export fails while runtime holds the lease.
12. Stop runtime, import a second valid configuration, and prove sessions remain.

Do not delete the old backup. Retention/removal is an operator decision after the new
release is accepted.

## Required final verification

Run the final native/webapp/process/integration/sanitizer/platform matrix established
in Block 7. At minimum:

```sh
cmake --preset ninja
cmake --build build/ninja
ctest --test-dir build/ninja --output-on-failure
cd webapp
npm run check
npm run build
npm run e2e
```

Also:

- run `git diff --check`;
- verify generated artifacts match their authoritative sources;
- compare implemented `--help` with all examples;
- search executable code for removed `--config`, `--workspace`, reload, `backup_dir`,
  directory-derived database paths, and runtime legacy detection;
- allow intentional old-option rejection tests and historical/supersession text;
- verify a new database is created only through successful import;
- verify supported runtime edits survive restart/export; and
- verify private-permission requirements on supported platforms.

## Out of scope

- A database backup command
- A configuration editor/UI
- Online import/export
- Configuration generations/history
- Deleting real operator backups or legacy data
- Redesigning code that already meets `docs/design.md`

## Completion gate

This job and the overall change are complete only when all tests pass, the disposable
cutover succeeds, every document describes one database-only storage model and
`--data` interface, supersession is unambiguous, and repository searches find no
undocumented executable path back to the old model.

In the final session report, list every document changed, verification commands and
results, cutover evidence, remaining historical keyword matches, and anything not
run locally.
