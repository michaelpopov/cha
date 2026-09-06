# Block 4: Native application, packaging, migration, and acceptance

## Task and prerequisites

Finish integration and documentation for CHA vaults. This is block 4, the last
of four sequential implementation blocks. This brief is self-contained; no
previous chat or block file is required. Verify that the repository already
implements these capabilities:

- A validated configuration directory and fixed vault registry, selected-vault
  startup/console commands, and application-wide `.env`/OAuth storage.
- `ApplicationRuntime::current_vault()` and tested in-process
  `switch_vault(name)`, using stable runtime objects and the same listener.
  Runtime/native maintenance reads current paths inside the lifecycle lock.
- The C++ macOS bridge accepts a configuration directory, seeds the selected
  missing database through directory discovery, and derives `can_modify` from
  the current vault instead of caching a startup flag.
- Bootstrap carries canonical `vault_name` and the `vaults` list. Session
  snapshots carry the name captured at opening. The switch route returns
  204/400/500, and the browser selector reloads `/` on success or mismatched
  vault identity. Protocol, browser, and runtime tests pass.
- Process/browser test harnesses already generate directory configurations.
  Their packaged-file expectations, actual package examples, and native Swift
  first-run setup still need the migration owned by this block.

Identify missing prerequisites instead of silently implementing earlier blocks.
Inspect equivalent code already completed and avoid redoing it. At completion,
the native application, development launcher, Linux packages, and documentation
all use the new layout, and the full feature's acceptance checks are accounted
for.

## Working rules

Read applicable `AGENTS.md`/`CLAUDE.md`, inspect `git status`, preserve unrelated
changes, and keep changes small. This block explicitly includes the `docs/`
edits listed below as part of the requested implementation. Do not expand that
into an unrelated documentation rewrite. Smaller commits are optional.

Never commit or package actual credentials, private `.env` files, local
databases, or runtime state. Existing intentionally tracked seed/example files
remain part of the package, including the seed's placeholder `.env`. Use
isolated temporary configurations/databases for verification. Migration of a
real user's private files is manual; document it rather than modifying those
files as an implementation side effect. Do not add compatibility code or an
automatic migration subsystem. Paths below are repository-relative.

## Final behavior to integrate and document

`--config` names a directory. There is no old single-file compatibility. The
directory holds required `app.toml` with application settings and selection,
plus one vault definition per other direct-child `.toml` file:

```text
cha-config/
├── app.toml
├── personal.toml
├── openai-auth.json
└── .env
```

Example `app.toml`:

```toml
vault = "Personal"

[web]
host = "127.0.0.1"
port = 8080

[logging]
file = "cha.log"
level = "info"
```

Example `personal.toml`:

```toml
vault_name = "Personal"
data = "personal.sqlite3"
# mirror = "personal-mirror"
# modify = "personal-edit"
```

All relative paths resolve against the configuration directory. The authored
vault name, not its filename, identifies it. Names match using the implemented
ASCII case folding, with non-ASCII bytes unchanged, and retain canonical
spelling for display. Discovery is nonrecursive and fixed until restart.
Invalid definitions, duplicate names/database paths/database filenames, and
unsafe modify overlaps fail discovery. Optional database/directory paths need
not exist at discovery; normal startup requires a usable selected database.

Exactly one vault is active globally. The selector switches in process,
closes old live sessions without deleting stored sessions, keeps the same
listener/port, and reloads the initiating browser to the target's Welcome
session. New operations use the selected database, mirror, and modify paths.
Other tabs check snapshot vault identity before applying state; missing old
sessions can end in the existing bounded retry state. No per-tab vaults,
watchers, or UI for editing vault definitions is introduced.

The application loads `.env` once from the configuration directory without
overriding inherited variables. One OAuth owner uses private
`openai-auth.json` there; login and pending device authorization survive
switching. Neither file is in workspace exports, mirrors, or R2 transfers.
Console commands continue using the process environment for credentials.

```text
chaweb --config=CONFIG_DIR [--root PATH]
chaweb --config=CONFIG_DIR --vault=NAME --import SOURCE_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --export DESTINATION_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --upload
chaweb --config=CONFIG_DIR --vault=NAME --download
```

Console maintenance requires explicit `--vault`; normal server mode rejects
it and uses `app.toml`. Console selection never changes the saved default.
Console import/export retain explicit source/destination arguments. Native
Import/Export use the current vault's optional `modify` and are unavailable
without it. Native pre-runtime initial seeding is the private exception to
console `--vault`: it resolves `app.toml` and seeds that vault if missing.
R2 continues using the percent-encoded local database filename as its object
key; duplicate filenames are rejected to prevent backup collisions.

Successful switching saves the canonical selection using an atomic TOML
rewrite. A failed save logs a warning and leaves the running vault changed
but the previously saved default intact. Failed mirror rebuilding likewise
logs and disables mirroring. Invalid target pre-checks leave the old vault
untouched; failed reopening after retarget requires restart, with the old
saved selection still present. Do not describe every failure as recoverable.

## Implementation

### macOS application and smoke setup

In `packaging/macos/main.swift`, use the Application Support directory itself
as the configuration argument instead of `configFile` pointing at `cha.toml`.
Keep `.env` at `supportDirectory/.env`, where the runtime now reads it.

On first run, when `app.toml` is missing, create complete `app.toml` selecting
`Personal` with the existing web/logging settings and the comment explaining
the native application's web-setting behavior. Create `personal.toml` with:

```toml
vault_name = "Personal"
data = "cha.sqlite3"
modify = "modify"
```

Do not overwrite existing user vault definitions as part of first-run setup.
Pass `supportDirectory.path` to both `cha_runtime_import_initial_database`
and `cha_runtime_create`. Update Import alerts that currently mention
`modify in cha.toml` to point to the active vault's configuration.

Make the Database menu refresh capabilities when opened: adopt
`NSMenuDelegate`, assign the delegate to the appropriate menu, and invoke
`updateDatabaseMenuItems()` in `menuNeedsUpdate(_:)`. A switch inside the web
view must immediately affect availability of native Import/Export. Existing
native transfer actions already call the runtime; verify they use current
paths and still handle the bridge's fatal-error return correctly.

Review `packaging/macos/runtime_bridge.h` and `.cpp` to confirm block 1 removed
the cached capability and changed the initial seed path. Update any stale
comments, but do not reintroduce a separate native registry or selection.

In `packaging/macos/runtime-smoke.c`, describe its first argument as a
configuration directory. In `packaging/macos/package.sh`, write complete
`app.toml` and `personal.toml` files under the smoke-test directory and pass
that directory to the smoke binary. Also update the staged example files used
by the packaged browser suite; this script currently copies the Linux example
to satisfy those checks, so changing only the smoke config is insufficient.

The archive must include the intended new repository configuration files in
`cha-config/` instead of `cha.toml`. Copy those files explicitly, not an entire
mutable local configuration directory that could contain `.env` or OAuth
credentials.

### Linux examples, launchers, and package checks

Replace `packaging/linux/cha.toml.example` with:

- `packaging/linux/cha-config.example/app.toml`: `vault = "Personal"` plus
  today's complete web/logging example settings.
- `packaging/linux/cha-config.example/personal.toml`: `vault_name = "Personal"`,
  `data = "cha.sqlite3"`, and commented optional mirror/modify examples.

In `bin/start-cha.sh`, use `CONFIG='../cha-config'`. Require a directory
containing `app.toml`. Update the setup hint to copy the example directory and
seed the explicitly named vault:

```bash
cp -R "$here/cha-config.example" "$config"
"$here/chaweb" --config="$config" --vault="Personal" --import "$import_seed"
```

Retain the launcher's current process/port handling. Do not turn it into a
supervisor or use process restarting to implement switching.

Update these producers and consumers together:

- `scripts/package-linux.sh`: stage the new example directory.
- `scripts/check-linux-package.sh`: expected top-level and nested entries,
  configuration content checks, launcher default, and setup/import hint.
- `scripts/test-linux-package-upgrade.sh`: use temporary directory configs and
  explicit `--vault` for maintenance. Test the documented manual cutover;
  do not imply that old single-file configurations still work automatically.
- `webapp/e2e/start-cha.mjs`: require
  `cha-config.example/app.toml` and its vault example in packaged mode instead
  of `cha.toml.example`. Preserve its directory generation and two-vault test
  setup from the earlier blocks.
- `packaging/macos/package.sh`: stage that same example layout for its browser
  package tests, and update corresponding comments/assertions.

### Development configuration and Makefile

Update `Makefile` usage and arguments:

- `make import-dev CONFIG=/path/to/cha-config VAULT=Personal` supplies
  `--vault="$(VAULT)"`. Require a non-empty `VAULT` along with `CONFIG` and
  provide an accurate usage error.
- `make run-web-dev CONFIG=/path/to/cha-config` passes the directory and does
  not pass `--vault` in server mode.
- `make run` uses the updated launcher's default configuration directory.

Replace tracked root `cha.toml` with tracked `cha-config/app.toml` selecting
`Dev` (`port = 0`, other web/logging values retained with paths adjusted) and
`cha-config/dev.toml`:

```toml
vault_name = "Dev"
data = "../cha.sqlite3"
modify = "/tmp/modify"
```

The development database remains at its existing root location. Update
`.gitignore` to ignore `/cha-config/*` except those two intended tracked files;
check that `openai-auth.json` and `.env` there are ignored. Preserve other
existing secret/database ignores. Do not move private root `.env` or
`cha.sqlite3.openai-auth.json`; document the manual move for the user.

### Documentation and manual migration

Update `README.md`: setup/copy/seed commands, application and vault TOML
examples, configuration-directory `.env` and OAuth locations, CLI usage,
Linux package contents, and migration steps. Include the explicit `VAULT`
argument for `make import-dev`.

Update `docs/MaintainerGuide.md`: the configuration-sources/location table,
configuration examples, environment and credential paragraphs, maintenance
commands, and the list of files deliberately excluded from workspace export.
Distinguish console explicit source/destination arguments from native `modify`.

Update `docs/web-ui/api-requirements.md` to include `vault_name` and `vaults`
in bootstrap and describe snapshot identity/switching where that document
describes those contracts. In `webapp/src/api/README.md`, note that
`switchVault` is a 204 mutation like deletion.

Document the one-time manual migration accurately:

1. Create the configuration directory. Move web/logging settings into
   `app.toml`, add its required `vault`, and move data/mirror/modify settings
   into a named vault file. There is no automatic compatibility path.
2. Adjust relative paths if the configuration files move to a different
   directory. For example, root `data = "cha.sqlite3"` becomes
   `data = "../cha.sqlite3"` when using the new root `cha-config/` directory.
   Existing database files need not move.
3. Move `.env` from the database directory to the configuration directory by
   hand. Shell variables still take precedence.
4. Move `<database>.openai-auth.json` to `openai-auth.json` in the configuration
   directory by hand, preserving private permissions, or sign in again through
   Settings. Do not copy credentials into example files or package artifacts.
5. Keep the database filename when retaining its R2 backup identity. R2 objects
   are not renamed to vault names.

Use `rg` to find stale config-file examples, usage text, generated configs, and
package assertions. Leave references to the historical `app.toml` inside an
import source alone; it is unrelated to the reserved application config file.

## Verification

Establish baseline results for available C++/browser checks before editing.
After changes run from the repository root:

```bash
make test
(cd webapp && npm run check)
make web-e2e
```

Run `bash -n` separately on each edited shell script, including the launcher,
Linux package/check/upgrade scripts, and macOS packaging script. Run the
existing Linux package validation/upgrade workflow and
`make package-linux VERSION=dev` in a supported packaging environment. On a
Mac with Xcode, run `./packaging/macos/package.sh <version>` with an appropriate
local test version; this must cover the runtime smoke test and packaged browser
checks. If a platform/toolchain is unavailable, identify the exact unrun checks
and run the available compile/fixture checks. Do not claim full platform
verification from `make build` alone.

Inspect actual package contents and native first-run output. Test using two
isolated vaults with distinct databases and optional settings: switch in the
web view, reopen the Database menu, exercise the current vault's native
operations, and restart to check saved selection. Verify a fresh native seed
creates the configured missing database without console `--vault`, and does
not replace an existing database. Do not use live R2 credentials for acceptance;
use existing local transfer mocks/fixtures where transfer verification needs
credentials.

## Final acceptance checklist

Map every item to an existing/new test or a documented manual result. Reuse
earlier block tests; add a focused missing test instead of repeating whole
suites without a reason. This checklist is evidence to collect, not an
assertion that the checks already passed.

- [ ] Configuration: directory-only `--config`, complete strict app/vault
      parsing, empty registry, unknown selection, canonical sorting/matching,
      duplicate names/data paths/filenames, and unsafe modify paths behave as
      specified; missing nonselected databases remain discoverable.
- [ ] Startup and credentials: a bad selected database fails startup; `.env`
      loads from the configuration directory before provider/workspace use,
      inherited values win, and OAuth uses the private application-wide file.
- [ ] Console: missing/unknown `--vault` fails before touching data; import
      creates its explicitly selected missing database; all four operations
      use the selected data and never persist console selection.
- [ ] Runtime switching: A-to-B-to-A keeps the listener/port and stable owners,
      closes old live sessions while preserving stored sessions, publishes the
      right workspace, and uses the new paths for every maintenance operation.
- [ ] Failure paths: no-op, unknown target, database/mirror pre-check failure,
      drain timeout, busy lease, and fatal reopen match their contracts.
      Mirror rebuild/config-save failures log without failing a completed
      switch; saved config remains whole on write failure.
- [ ] Concurrency: maintenance cannot capture old paths before its lock;
      bootstrap and compound repository/mirror operations cannot mix vaults;
      target mirror rebuilding does not re-enter the repository lock. Record
      the block-2 TSan result or run it if evidence is unavailable.
- [ ] Providers/auth: login and pending device authorization survive switching;
      a cancelled slow provider tail does not block switching teardown and
      new requests use the target workspace's definitions.
- [ ] Protocol: bootstrap and all snapshots have correct canonical identity;
      switch route validation, 204 success, and 400/500 errors are covered.
- [ ] Browser: selector layout and pending/error states, full reload to `/`,
      snapshot/refresh mismatch rejection, normal matching snapshot behavior,
      and bounded recovery of a missing old session are covered.
- [ ] Native app: first-run config/seed, directory arguments, dynamic menu
      capabilities, current-vault operations, error handling, and saved
      selection on restart are verified.
- [ ] Packages/development: examples, launchers, Makefile, E2E package checks,
      smoke setup, and archives agree on the directory layout; no private
      runtime files are included in version control or packages.
- [ ] Documentation: setup and maintenance examples are valid; manual path,
      `.env`, and OAuth migration and unchanged R2 object identity are clear.

Finish with a concise report of completed integration, tests/platform results,
and any specifically outstanding manual checks. No later implementation block
is planned; do not call the feature fully verified while hiding a required
unrun platform check.
