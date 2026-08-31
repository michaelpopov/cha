# Block 7 job — Integration hardening, samples, and packaging

## Mission

Implement Block 7 from `docs/plan.md`: audit and harden the completed unified
database implementation across process boundaries, fill test-plan gaps, and update
samples, staging, launch, and packaging assets so nothing outside documentation still
assumes directory-backed runtime configuration.

This block is not an architectural redesign. Fix defects against `docs/design.md`
using the smallest direct change.

## Source of truth and prerequisites

- Blocks 1 through 6 must already be implemented and passing.
- Read all of **Test plan**, **Failure and crash consistency**, **Secret-bearing file
  permissions**, and **Build and test support** in `docs/design.md`.
- Read Block 7 and the global invariants in `docs/plan.md`.
- Inspect existing CMake presets, process fixtures, package scripts, service/launch
  templates, and sample workspace before editing.
- Preserve unrelated worktree changes and never use a real user database/API key as
  a fixture.

## Required result

At the end of this job:

- the complete design test matrix is automated where practical;
- process locking, crash/failure boundaries, v1 preservation, byte round trips, and
  private permissions are covered;
- sample configuration is a valid import seed with root `app.toml`;
- launch/service assets use `--data DATABASE`;
- staging/packages do not ship a developer database or treat a workspace directory
  as live runtime authority; and
- the native, webapp, process, stress, and applicable platform test suites pass.

## Expected implementation areas

- session/workspace/store/application-config unit tests
- native web process/integration/stress fixtures
- webapp tests and generated API check
- sample workspace and test workspace helpers
- root `CMakeLists.txt`, presets, and staging CMake scripts
- package/install/service/launch scripts and manifests
- source/test registration cleanup

User and architecture prose documentation belongs to Block 8 unless a package asset
must contain an executable command to function.

## Work 1 — Audit the complete test matrix

Create a checklist from `docs/design.md` and tests added in Blocks 1–6. Do not assume
a test exists because a unit below it passed. Add the smallest missing tests.

### Database/configuration integrity

Verify:

- import creates valid v2 from a missing database;
- import upgrades a populated v1 and preserves every session, label, turn, entry,
  message, and timestamp;
- v2 replacement changes only configuration rows;
- v1 upgrade and v2 replacement roll back under injected failures;
- normal runtime/export reject missing, v1, foreign, malformed, or incomplete
  databases without modifying them;
- exact import/export content round trip includes arbitrary bytes and embedded NULs;
- stored-name application checks and SQL `CHECK` defense both execute; and
- restart reconstructs runtime solely from database rows.

### Import validation and legacy protection

Verify import fails before database open/creation/change for:

- missing required files;
- malformed consumed application/workspace TOML;
- bad dotenv syntax;
- missing provider key after overlay policy;
- excluded include dependency such as `.txt`;
- unsafe/matching symlink paths; and
- legacy session databases.

Exercise both legacy messages:

- target missing: archived migration never ran;
- target present: cleanup incomplete.

Prove `.env`-only provider validation, inherited variable precedence including an
empty inherited value, and complete environment restoration.

### Lease and process behavior

At process level, verify:

- runtime rejects a second runtime;
- runtime rejects import/export;
- import/export reject runtime while holding the lease;
- a stale lock file without a held kernel lock does not block startup;
- source deletion after import does not affect runtime;
- a process killed during an SQLite transaction recovers to one complete committed
  configuration; and
- shutdown/checkpoint/reopen retains all sessions and configuration.

Avoid timing-only assertions. Use existing process synchronization/test hooks where
possible.

### Runtime mutation behavior

Verify:

- character provider/style changes survive restart and export;
- default character and persona changes survive restart and export;
- affected-session reload behavior remains intact;
- concurrent mutations serialize without publishing an older candidate last;
- validation, row-collection, write, or commit failures restore materialized content
  in place and leave database/published workspace old;
- modeled post-commit publication failure is recovered by restart; and
- no published reader accesses torn materialized files.

Keep failure injection local and explicit. Do not add a generic fault-injection
framework.

### Permissions and paths

On POSIX, verify owner-only modes for:

- newly created database;
- WAL/SHM and rollback sidecars when applicable;
- an existing permissive v1 database tightened before imported secrets are written;
- the one process-private root;
- import validation root; and
- exported `.env` from first creation.

Also verify symlink/non-regular sidecars fail safely and relative `logging.file`
resolves under the database parent even when process CWD differs.

On Windows, compile/run equivalent private-DACL tests where CI supports them. A
platform test that cannot run locally should remain assigned to the appropriate CI
platform; do not weaken the assertion.

### Export failure behavior

Verify:

- missing and empty destinations work;
- non-empty destinations fail before writes;
- no existing file is overwritten;
- a mid-export I/O failure leaves the database/unrelated paths unchanged and reports
  a possibly incomplete destination; and
- export contains configuration only, never sessions/sidecars.

## Work 2 — Update sample/import fixtures

- Put root `app.toml` in the sample import tree with valid required `host` and `port`.
- Remove `workspace` and `backup_dir` from sample application settings.
- Ensure sample includes target only stored `.toml`, `.md`, or root `.env` files.
- Keep distributed sample `.env` secret-free; use clear placeholders if needed.
- Add an automated fixture whose provider key comes only from `.env`, never a real
  credential.
- Make test helpers able to create an import tree and unified database independently.
- Ensure fixture cleanup targets only explicit disposable paths.

The sample directory is an import seed, not runtime authority.

## Work 3 — Update build and source registration

- Ensure new private-filesystem/store sources and tests are registered once.
- Remove reload/backup sources and tests from all build targets.
- Ensure bundled SQLite receives `SQLITE_DEFAULT_FILE_PERMISSIONS=0600` in every
  relevant build/package configuration.
- Remove obsolete fixture arguments/helpers for `--config APP_TOML`, `--workspace`,
  backup directories, and directory-derived databases.
- Keep direct `Workspace::load(directory)` parser tests registered.

## Work 4 — Update staging, packages, and launch assets

- Change executable launch/service templates to use `--data DATABASE`.
- Do not ship or stage a developer's live SQLite database, WAL/SHM, lock, `.env`
  secrets, or sessions.
- If distributing a sample workspace, place/label it as an import seed only.
- Do not silently import the seed during ordinary startup.
- Provide an explicit development/package invocation that imports the seed to a
  caller-selected database, using the same public command as production:

  ```text
  chaweb --data DATABASE --import SEED_DIRECTORY
  ```

- Ensure local staging/package tests execute import before starting the server.
- Update cleanup logic to distinguish disposable build/test databases from user
  databases. Never use a broad unresolved path or recursive deletion of user data.
- Remove package assumptions that a copied durable workspace is live configuration.

## Required verification

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

Also run configured sanitizer, stress, and process-lock targets relevant to the
changed code. Use the repository's actual named targets/presets rather than inventing
new generic ones.

Run package/staging smoke tests on disposable paths. Inspect packaged file lists to
prove no real database, sidecar, lock, or secret-bearing `.env` was included.

Run `git diff --check` and confirm generated TypeScript API artifacts still match
`resources/cha.yaml`.

## Out of scope

- New storage abstractions or schema changes
- Automatic startup import
- Database backup feature
- Broad reload behavior
- General README/tutorial/architecture rewrite (Block 8)
- Destructive migration of real operator data

## Completion gate

This job is complete only when the design test matrix has an explicit pass or a
documented platform assignment, all samples/packages/launchers use the database-only
model, full automated verification passes, and no package can accidentally publish a
developer's data or secrets.

In the final session report, provide the audited test checklist, commands/results,
package contents checked, and any platform-only test not run locally.
