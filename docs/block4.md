# Block 4: Launchers, packages, and migration

## Goal

Finish the vault feature by updating the remaining launchers, packages,
development files, and documentation to use a configuration directory. This is
the last of four sequential blocks and this brief is self-contained.

Blocks 1–3 already implement directory configuration, explicit `--vault` for
console maintenance, in-process switching, the browser selector, and shared
`.env`/OAuth storage. Bootstrap contains `vault_name` and `vaults`; session
snapshots intentionally have no vault field. Do not redesign those parts, add
cross-tab coordination, or change runtime switching. This block only fixes
consumers that still expect one `cha.toml` file.

Read `AGENTS.md` and `CLAUDE.md`, inspect the worktree, and preserve unrelated
changes. This block explicitly includes the documentation edits listed below.
Never package actual credentials or runtime data. The existing import seed's
placeholder `.env` remains intentional.

## Required layout

Launchers and examples use a directory containing `app.toml` and one TOML file
per vault:

```text
cha-config/
├── app.toml
└── personal.toml
```

Linux example `app.toml`:

```toml
vault = "Personal"

[web]
host = "0.0.0.0"
port = 8086

[logging]
file = "logs/cha.log"
level = "info"
```

Example `personal.toml`:

```toml
vault_name = "Personal"
data = "cha.sqlite3"
# mirror = "mirror"
# modify = "modify"
```

Relative paths resolve from the configuration directory. Private installations
may also contain `.env` and `openai-auth.json`; example directories must not.

## macOS application and package

Update `packaging/macos/main.swift`:

- Remove the single-file `configFile` property and pass
  `supportDirectory.path` to `cha_runtime_import_initial_database` and
  `cha_runtime_create`.
- On first run, create missing `app.toml` and `personal.toml` files. Select
  `Personal`, retain the native app's current web/logging values, and put
  `data = "cha.sqlite3"` and `modify = "modify"` in the vault file. Never
  overwrite an existing file.
- Change the Import explanation that mentions `modify in cha.toml` to refer to
  the active vault's configuration.
- Refresh Database menu availability whenever that menu opens. Use
  `NSMenuDelegate` and call the existing `updateDatabaseMenuItems()` from
  `menuNeedsUpdate(_:)`.

Update `packaging/macos/package.sh` and its runtime smoke fixture to pass a
directory containing both TOML files. Stage the Linux
`cha-config.example/` directory for the packaged browser checks. Replace the
archive's copy of root `cha.toml` with explicit copies of the tracked
`cha-config/app.toml` and `cha-config/dev.toml`; do not copy the whole mutable
directory. The runtime bridge already accepts a directory and reads the current
vault's capabilities, so it should need no functional changes.

## Linux package and launchers

Replace `packaging/linux/cha.toml.example` with:

```text
packaging/linux/cha-config.example/
├── app.toml
└── personal.toml
```

Use the examples above and update these consumers:

- `bin/start-cha.sh`: default to `../cha-config`, require `app.toml`, show how
  to copy the example directory, and initialize with
  `--vault="Personal" --import "$import_seed"`.
- `bin/start-cha.bat`: make the equivalent path, setup-message, and import
  changes. Do not add Windows packaging work.
- `scripts/package-linux.sh`: copy `cha-config.example/` into the package.
- `scripts/check-linux-package.sh`: expect the two example files and verify
  their essential values and launcher command. Keep the existing package and
  private-file checks; do not build another TOML validator in shell.
- `scripts/test-linux-package-upgrade.sh`: generate temporary `app.toml` and
  vault files and pass an explicit vault to import. Keep the existing proof
  that replacing the application directory preserves the external database.
- `webapp/e2e/start-cha.mjs`: require both example files in packaged mode
  instead of `cha.toml.example`.

## Development files

Replace root `cha.toml` with tracked `cha-config/app.toml` and
`cha-config/dev.toml`. The app file selects `Dev` and retains the current
development web/logging values. The vault file is:

```toml
vault_name = "Dev"
data = "../cha.sqlite3"
modify = "/tmp/modify"
```

Update `.gitignore` so only those two files under `/cha-config/` are tracked.
Local vault files, `.env`, `openai-auth.json`, databases, and logs remain
ignored.

Update `Makefile`:

- `make import-dev CONFIG=/path/to/cha-config VAULT=Personal` requires both
  variables and passes `--vault="$(VAULT)"`.
- `make run-web-dev CONFIG=/path/to/cha-config` passes the directory without
  `--vault`.
- `make run` continues through the updated launcher.

## Documentation and migration

Update `README.md` and `docs/MaintainerGuide.md` where they describe setup,
configuration, maintenance commands, package contents, `.env`, and OAuth
credentials. Keep the explanation concise. Document this manual migration:

1. Create a configuration directory. Put selection and web/logging settings in
   `app.toml`; put data, mirror, and modify paths in a vault TOML file.
2. Adjust relative paths for the new base directory. For a root database this
   commonly changes `data = "cha.sqlite3"` to `data = "../cha.sqlite3"`; the
   database itself does not move.
3. Move `.env` to the configuration directory if it was beside the database.
4. Move `<database>.openai-auth.json` to
   `<config-directory>/openai-auth.json`, preserving private permissions, or
   sign in again.

There is no automatic migration or old single-file compatibility. R2 object
keys still come from database filenames.

Update `docs/web-ui/api-requirements.md` for the actual browser contract:
bootstrap has `vault_name` and `vaults`, and
`POST /api/v1/vault/switch` is followed by a full reload on success. Do not add
vault identity to session snapshots. In `webapp/src/api/README.md`, describe
`switchVault` as an empty 204 mutation.

Use `rg` to find remaining user-facing references to the old external
`cha.toml`. Leave unrelated `app.toml` files inside import workspaces alone.

## Verification

Run:

```bash
make test
(cd webapp && npm run check)
make web-e2e
```

Run `bash -n` on every edited shell script and run the existing Linux package
integrity and upgrade checks. On macOS, run the package script or its available
Swift/runtime smoke checks. Report any unavailable platform check explicitly.

Inspect the package tree: both example TOML files must be present, and no
private `.env`, OAuth file, database, log, or lease may be included. The import
seed's placeholder `.env` is expected. Finish with a concise change and test
report.
