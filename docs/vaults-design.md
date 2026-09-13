# Vaults design

## Purpose

A vault is a named CHA database. It owns workspace configuration, saved
sessions, model API keys, and R2 credentials. A running application has one
active vault. Vault selection is global to the process; there are no per-tab or
per-user vaults.

Selecting another vault closes live sessions, releases the old database,
opens the selected database, rebuilds its mirror, and reloads the initiating
browser. The HTTP server, port, provider supervisor, and process-wide OpenAI
OAuth login stay in place.

## Configuration directory

`--config` names a directory:

```text
chaweb --config=/absolute/path/to/cha-config
```

A typical directory is:

```text
cha-config/
├── app.toml
├── personal.toml
├── projects.toml
├── personal.sqlite3
├── projects.sqlite3
├── mirror/
├── modify/
└── openai-auth.json
```

`app.toml` contains application settings, the selected vault, and optional
base directories:

```toml
vault = "Personal"
mirror = "mirror"
modify = "modify"

[web]
host = "127.0.0.1"
port = 8080

[logging]
file = "logs/cha.log"
level = "info"
```

Every other direct-child `.toml` file defines one vault:

```toml
vault_name = "Personal"
data = "personal.sqlite3"
protected = false
```

The filename does not identify the vault. `vault_name` is the displayed and
saved name. A relative `data`, `mirror`, or `modify` path resolves from the
configuration directory. For each vault, CHA appends the display name to the
optional bases. The example therefore uses `mirror/Personal` and
`modify/Personal`.

Vault names must be valid single filesystem components because CHA uses them
for derived directory names and for the default database filename. New vaults
created in Settings use `<display-name>.sqlite3` in the configuration
directory. Their database path does not change when the vault is later renamed.

The only meaningful vault-file fields are `vault_name`, `data`, and optional
`protected`. Extra fields, including obsolete per-vault `mirror` and `modify`
values, are ignored and logged as warnings. This lets an otherwise valid vault
open despite unused old settings.

### Validation

- `app.toml` must parse and contain only known application fields.
- Every vault file must contain a valid display name and data path.
- The selected name must match a discovered vault using the platform's path
  component comparison rules.
- Vault names, normalized database paths, and database filenames must be
  unique.
- An existing derived modify directory must be empty or a valid CHA workspace.
- A modify directory cannot overlap another modify directory, a configured
  database, or the configuration directory.
- Mirror paths do not participate in cross-vault collision validation. A
  mirror is a disposable projection and never deletes a whole tree.

Discovery validates definitions and path relationships but does not open every
database. Normal startup and runtime switching require the selected database
to be valid. Import may create a missing selected database.

## Credentials and protection

Provider and style definitions, model API keys, and R2 credentials are
configuration rows inside each vault database. Keys are stored as plaintext
TOML under `system/keys/` in a workspace export and are never returned to the
browser. A provider stores only its selected key ID.

`openai-auth.json` remains process-wide in the configuration directory. Login,
logout, and pending device authorization therefore apply to every vault and
survive a switch. A source workspace `.env` is ignored. For migration only,
each empty vault can import model keys from a legacy configuration-directory
`api-keys.json` and R2 values from the inherited environment or
configuration-directory `.env`. Those source files are not removed.

A protected vault has `protected = true` in its definition and a
SQLCipher-encrypted database. The password is not stored in the definition,
database, or another credential file. Native launchers and `chaweb` request it
before opening a protected startup vault. Browser switching retries with a
password only after the server returns `vault_password_required`.

Settings can create a protected vault or protect an existing unprotected one.
Protecting checkpoints the database, copies it into an encrypted replacement,
and keeps the active password in memory for database users. The current UI does
not remove protection or change a password. A lost password has no recovery
path in CHA.

Workspace export decrypts through the open database connection and writes
plaintext configuration, including saved model and R2 secrets. R2 upload copies
the complete database file, so a protected vault remains encrypted remotely.

## Startup and console commands

At startup CHA discovers vault definitions, requests the selected vault's
password when needed, opens that database, and then opens the vault-backed key
store plus the process-wide OAuth store. Failure to unlock the selected
database fails startup.

Console maintenance names a vault explicitly:

```text
chaweb --config=CONFIG_DIR --vault=NAME --import SOURCE_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --export DESTINATION_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --upload
chaweb --config=CONFIG_DIR --vault=NAME --download
```

`--vault` is rejected in server mode, which uses `app.toml`. A console command
never changes the saved selection. Protected vaults prompt on standard input;
interactive terminals hide the entered password. Import, export, upload, and
download all use that password.

## Runtime ownership

Routes are installed once and hold references to long-lived runtime objects:

- `WorkspaceConfigStore` owns the database handle, file lease, materialized
  workspace, and database password.
- `SessionRepository` performs stored-session queries against the same database
  and password.
- `SessionMirror` writes the optional Markdown projection.
- `ApiKeyStore` reads and mutates the active vault's key rows.
- `LiveSessionManager` owns live session actors; their journals receive the
  database password when opened.

All database maintenance operations are serialized by the runtime lifecycle
mutex. Store and repository maintenance guards can close their handles while
the process retains the database lease. Retargeting assigns the selected path,
lease, and password before reopening.

## Switching

`ApplicationRuntime::switch_vault(name, password)` uses this sequence:

1. Resolve the target. An unknown name fails; selecting the current vault is a
   no-op.
2. Require a password for a protected target, acquire its database lease, apply
   the SQLCipher key, and validate the database before disturbing live sessions.
3. Reserve global maintenance, stop and drain live sessions, and checkpoint the
   old database.
4. Close store and repository handles, retarget them to the selected database,
   and reopen. Publish the selected workspace and synchronize its forums.
5. Replace the active in-memory password and current-vault value.
6. Import legacy keys if the target is empty, then rebuild its mirror. Mirror
   failure is logged and disables mirroring without undoing the switch.
7. Atomically update `vault` in `app.toml`. Save failure is logged; the running
   process stays on the new vault while the next launch uses the previous saved
   selection.
8. Release maintenance and return success. The initiating browser reloads at
   Welcome.

A bad password, busy target, or invalid database fails before old sessions are
closed. After retargeting begins, a reopen failure leaves the runtime unable to
serve safely, so it stops and requires restart. Other browser tabs receive no
switch broadcast and may need manual reload.

## Browser API and UI

Bootstrap exposes the active canonical name and sorted vault names. The
collection route returns richer `VaultDetail` values, including `protected`,
derived paths, active state, and deletion availability.

```http
POST /api/v1/vault/switch
Content-Type: application/json

{"vault_name":"Projects","password":null}
```

Success returns `204 No Content`. A protected target without a usable password
returns `401` and `vault_password_required`; the sidebar opens a password
dialog and retries. The password is sent only in that mutation body and is not
saved by the browser.

Settings → Vaults supports four operations:

- **New vault** derives the database and working paths from its display name.
  New empty vault copies the active workspace configuration without sessions;
  copying an existing vault copies its complete database. The destination may
  be protected or unprotected independently. A protected source can be copied
  only while active.
- **Download vault** lists root-level `.sqlite3` objects in the R2 bucket
  configured by the active vault. It excludes local database filenames,
  downloads the selected object into the configuration directory, validates it,
  creates a local definition, and leaves it inactive. A legacy database-only
  object derives its name from the filename. Protected remote vaults are
  rejected because this flow does not collect their password.
- **Edit vault** renames through the top-bar title and can enable protection.
  Rename moves existing derived mirror and modify directories. The create and
  protect forms include an explicit password show/hide control.
- **Delete vault** removes only an inactive vault definition and keeps its
  database and directories. The last vault cannot be deleted.

The Vaults list shows names and marks the active entry without exposing local
database paths.

## R2 behavior

The active vault's R2 storage record supplies the bucket URL, access-key ID,
and secret. Upload and Download are unavailable when that record is absent.

Upload validates the current schema-v2 database and vault definition, then
uploads two root-level objects: `<database-filename>.toml` followed by
`<database-filename>`. R2 cannot replace this pair atomically, so a failed
upload should be retried before download.

Database Download fetches and validates both objects before replacing either
local file. It saves previous local versions with `.bac` suffixes. A legacy
database-only upload cannot replace the active vault until it has been uploaded
again by a current CHA version. Transfers use connection timeouts and abort a
response that remains below one byte per second for 30 seconds.

The separate Download vault screen tolerates a missing companion definition so
legacy remote databases can be added under a name derived from the `.sqlite3`
object. It does not overwrite an existing local database or definition.

## macOS and Windows applications

On first launch an empty configuration directory receives `app.toml`,
`default.toml`, `default.sqlite3`, and private `mirror/` and `modify/`
directories. The Default vault is unprotected and contains the built-in
Assistant configured for ChatGPT OAuth.

The macOS and Windows launchers inspect the selected vault definition before
creating the runtime. If it is protected, they show a native password prompt,
retry after a wrong password, and quit cleanly when the prompt is cancelled.
Native Import, Export, Upload, and Download always act on the active vault and
reuse its in-memory password.

## Failure behavior

| Failure | Result |
| --- | --- |
| Invalid configuration or startup vault | Startup fails. |
| Missing, wrong, or cancelled startup password | The protected vault does not open. |
| Startup mirror cannot be rebuilt | Startup continues with mirroring inactive. |
| Console command omits or misnames `--vault` | It fails before touching vault data. |
| Create/update paths fail validation | The request is rejected and the registry is unchanged. |
| Protected copy source is inactive | Creation is rejected; switch to that source first. |
| Delete targets the active or last vault | The request is rejected. |
| Switch target is unknown, busy, invalid, or has a bad password | The old vault remains active. |
| Live sessions do not drain in time | The old vault remains active and admission resumes. |
| Reopen fails after retargeting | The runtime stops and requires restart; the saved selection is unchanged. |
| Mirror rebuild fails | The switch succeeds with mirroring inactive. |
| Saving `app.toml` fails | The switch succeeds; the next launch uses the previously saved vault. |
| Export target is an unrelated nonempty directory | Export refuses to replace it. |
| Active R2 download is missing its companion definition | Neither local file is replaced. |
| Download vault selects a protected remote object | Nothing is installed. |

## Migration

Migration from a single configuration file is manual:

1. Create a configuration directory.
2. Move vault selection, optional mirror/modify bases, web, and logging settings
   into `app.toml`; put `vault_name`, `data`, and optional `protected` in a vault
   file.
3. Adjust paths for the new directory. Existing databases do not need to move.
4. Move `<database>.openai-auth.json` to `openai-auth.json`, or sign in again.
5. Place legacy `api-keys.json` and R2 `.env` in the configuration directory
   only long enough to import them into each intended empty vault. Secure or
   remove those legacy sources afterward.

## Non-goals

Vaults do not add:

- more than one active vault;
- per-browser or per-user selection;
- live rescanning or filesystem watching;
- per-vault OpenAI OAuth credentials;
- password storage, recovery, change, or removal;
- automatic migration from the old single-file configuration; or
- automatic cross-tab reload.

The feature remains one process-wide list of local databases, one current
selection, and one explicit switch operation.
