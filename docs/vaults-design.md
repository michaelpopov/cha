# Vaults design

## Purpose

A vault is a named CHA database with optional mirror and modification
directories. A running application has one active vault. Selecting another
vault closes the current live sessions, closes the database, opens the selected
database, and reloads the initiating browser.

This is the same maintenance cycle used by download, with three small additions:
resolve the selected vault, change the database path, and save the selection.
The HTTP server, port, provider supervisor, and OpenAI login stay in place.

Vault selection is global to the process. CHA does not provide per-tab or
per-user vaults.

## Configuration directory

`--config` names a directory:

```text
chaweb --config=/absolute/path/to/cha-config
```

There is no compatibility mode for the previous single-file argument. A typical
directory is:

```text
cha-config/
├── app.toml
├── personal.toml
├── projects.toml
├── api-keys.json
└── openai-auth.json
```

`app.toml` contains application settings and the selected vault:

```toml
vault = "Personal"

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
# mirror = "/Users/alice/CHA/mirror"
# modify = "/Users/alice/CHA/modify"
```

The filename does not identify the vault. `vault_name` is the displayed and
saved name. A relative `data` path resolves from the configuration directory.
`mirror` and `modify` must be absolute paths.

CHA discovers vault definitions at startup. Direct file edits take effect after
restart. Settings → Vaults updates the file and the running registry together,
without changing the active vault. A successful switch only changes the
`vault` value in `app.toml`.

### Validation

Configuration remains strict because a typo can select or overwrite the wrong
data:

- `app.toml` and every discovered vault file must parse and contain only known
  fields.
- The selected name must match a discovered vault. Matching ignores ASCII case
  while preserving the spelling from `vault_name`.
- Vault names, normalized database paths, and database filenames must be
  unique. Database filenames are unique because they remain R2 object names.
- `mirror` and `modify` paths are absolute. Settings accepts a mirror only when
  it is an existing directory. An existing modify directory must be empty or a
  valid CHA workspace.
- A `modify` directory cannot overlap another `modify` directory, a configured
  database, or the configuration directory. Export replaces this directory.
- Mirror paths do not participate in cross-vault collision validation. The
  mirror is a disposable projection and never deletes a whole tree.

Discovery validates definitions and path relationships. It does not open every
database. A nonselected database may be absent; import can create it. Normal
startup and runtime switching require the selected database to be valid.

## Application-wide files

`api-keys.json`, `openai-auth.json`, and an optional `.env` live in the
configuration directory rather than inside a vault. API keys used for model
access exist only in `api-keys.json`; providers in a vault refer to their opaque
IDs. `openai-auth.json` holds the process-wide ChatGPT OAuth session. The
optional `.env` is loaded once without overriding inherited values and is used
only for R2 storage settings.

The compatibility field `api_key_env = "Name"` does not read the environment.
It resolves `Name` as an exact API-key display name in `api-keys.json`. A
missing or ambiguous name fails when that provider is used, not while the vault
is loaded. Saving the provider through Settings converts a resolvable name to
the normal opaque `api_key` ID.

One `OpenAiOAuth` owner uses `openai-auth.json` for the life of the process.
Login, logout, and pending device authorization therefore apply to every vault
and survive a switch. Neither credential file is included in workspace
import/export, session mirrors, or R2 transfers. Provider and style definitions,
by contrast, are configuration rows inside each vault's SQLite database.

Provider definitions still come from the active workspace. The process-wide
provider supervisor needs no vault-switch behavior: closing live sessions
cancels their requests, and newly opened sessions use definitions from the new
workspace.

## Startup and console commands

At startup CHA discovers the vaults, opens the process-wide key and OAuth
stores, loads optional R2 environment settings, and opens the database selected
by `app.toml`. Failure to open that database fails startup.

Console maintenance must name a vault explicitly:

```text
chaweb --config=CONFIG_DIR --vault=NAME --import SOURCE_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --export DESTINATION_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --upload
chaweb --config=CONFIG_DIR --vault=NAME --download
```

`--vault` is rejected in server mode, which uses `app.toml`. A console command
never changes the saved selection. Import may create the selected database;
configuration import/export also requires that vault's optional `modify` path.

R2 continues to derive its object key from the selected database filename. A
vault switch does not rename or migrate existing backups.

## Runtime ownership

Routes are installed once and hold references to long-lived runtime objects.
Those objects stay alive during a switch:

- `WorkspaceConfigStore` owns the database handle, file lease, and materialized
  workspace.
- `SessionRepository` performs stored-session queries against the same database.
- `SessionMirror` writes the optional Markdown projection.
- `LiveSessionManager` owns live session actors.

The first two objects each contain a database path. While the database is
closed and their existing maintenance locks are held, the switch assigns the
new path to both. This is an implementation detail of changing databases, not
a separate subsystem. There is no prepared retarget state, generic transaction
framework, second runtime, activation epoch, or per-request vault lease.

`SessionMirror` is one stable object because session callbacks already refer to
it. After the new database opens, it clears its old mappings and rebuilds from
the new repository and optional mirror path. With no configured mirror it is
inactive, and `add`/`update` do nothing.

All database maintenance operations are serialized by the existing runtime
lifecycle mutex and read the current vault only after taking that mutex. This
prevents an operation from pausing one vault and using another vault's path.

## Switching

`ApplicationRuntime::switch_vault(name)` uses an explicit sequence:

1. Resolve the target. An unknown name fails; selecting the current vault is a
   no-op.
2. Acquire the target database lease and validate the target database. Both
   happen before live sessions are disturbed.
3. Reserve ordinary global maintenance, which stops and drains live sessions
   using the same bounded mechanism as download.
4. Under the existing store and repository maintenance locks, checkpoint and
   close the old database, assign the target path and lease, and reopen. Reopen
   validates and publishes the target workspace and synchronizes its forums.
5. Record the target as current.
6. Rebuild the mirror from the open target database. A rebuild failure is logged
   and leaves mirroring inactive; it does not undo the database switch.
7. Atomically update the `vault` value in `app.toml`. A save failure is logged;
   the running process remains on the new vault and the next launch uses the
   previously saved vault.
8. Release maintenance so new sessions may open, then return success.

Acquiring the target lease first is important: a busy target fails before the
old sessions are closed. The target mirror is not prechecked. It is derived
output and is handled after the primary database is open.

Closing live sessions does not delete their stored rows. Switching back to a
vault makes its stored sessions available again. The listener and port never
change.

The application does not promise a general rollback after the old database has
been closed. If the new database cannot be reopened after paths change, the
runtime reports that restart is required. `app.toml` still names the previous
vault because selection is saved only after a successful reopen.

Existing locks protect database replacement and session draining. They do not
form a new application-wide transaction around every HTTP request. A request
already in progress may finish using state from around the switch. That small
window is accepted for this personal, single-user application.

## Browser API and UI

Vault discovery uses the existing bootstrap response:

```json
{
  "vault_name": "Personal",
  "vaults": ["Personal", "Projects"],
  "initial_forum_id": "entrance",
  "initial_session_id": "welcome",
  "personas": [],
  "characters": [],
  "forums": [],
  "recent_sessions": []
}
```

`vault_name` is the canonical active name. `vaults` is the sorted list of
canonical names. Session snapshots are unchanged and carry no vault identity.

The browser switches with:

```http
POST /api/v1/vault/switch
Content-Type: application/json

{"vault_name":"Projects"}
```

Success returns `204 No Content`. An unknown name returns the existing
`bad_request` response; maintenance failures use `internal_error`.

The sidebar uses a native `<select>` next to Settings. The control is disabled
while its request is pending. Pending and error state are local to the sidebar;
the application reducer needs no vault-switch state.

After success, the initiating page reloads `/` and bootstraps the new vault's
Welcome session. It does not preserve its previous screen, URL, conversation,
or draft. If the request fails before database replacement, the page remains on
the current vault and displays the error.

Other tabs receive no broadcast and perform no vault-identity checks. Their old
live sessions close and their existing recovery behavior applies. They may need
a manual reload. This is an accepted consequence of keeping a process-global
feature simple for a personal application.

Settings shows Vaults above Providers, Styles, and API Keys. The collection API
supports listing, creating, updating, and deleting definitions through
`/api/v1/vaults`.

Creating a vault has two database choices:

- **New empty vault** copies the active vault's workspace configuration but no
  sessions into a new database.
- **Copy an existing vault** copies that vault's complete SQLite database,
  including configuration and sessions.

Both choices add the vault to the selector without making it active. Creation
also records the display name, a new database path, and optional absolute
mirror and modify paths. Later edits can rename the display name or change the
mirror and modify paths; the database path is read-only.

Deletion is registry-only: it removes the vault TOML and keeps the database and
directories. The active vault cannot be deleted, and the last vault cannot be
deleted.

## macOS application

The native application passes its Application Support directory as the
configuration path. When normal server startup finds that directory empty, the
C++ configuration layer creates `app.toml`, `default.toml`, and
`default.sqlite3`. The new `Default` vault has an absolute `modify` path, no
saved sessions, and a minimal workspace containing the built-in Assistant and
ChatGPT OAuth provider. Bootstrap does not run for a nonempty directory or an
offline command.

The browser title is `CHA: <Vault name>`. The native launcher observes that
title and applies it to the main window after startup and after a vault switch;
a Database operation temporarily shows its progress title.

Native Import, Export, Upload, and Download always consult the current vault.
The Database menu refreshes when opened so Import/Export availability follows
the selected vault's optional `modify` path.

## Failure behavior

| Failure | Result |
| --- | --- |
| Invalid configuration or startup vault | Startup fails. |
| Startup mirror cannot be rebuilt | Startup continues with mirroring inactive. |
| Console command omits or misnames `--vault` | It fails before touching vault data. |
| Create/update paths fail validation | The request is rejected and the registry is unchanged. |
| Delete targets the active or last vault | The request is rejected. |
| Switch target is unknown | The old vault remains active. |
| Target lease is busy or target database is invalid | The switch fails before live sessions are paused. |
| Live sessions do not drain in time | The old vault remains active and admission resumes. |
| Reopen fails after the database path changes | The runtime requires restart; the saved selection is unchanged. |
| Mirror rebuild fails | The switch succeeds with mirroring inactive. |
| Saving `app.toml` fails | The switch succeeds; the next launch uses the previously saved vault. |
| Export target is a nonempty directory that is not a CHA workspace | Export refuses to replace it. |

The `app.toml` rewrite uses a temporary sibling file and rename, so a failed
save does not leave a truncated configuration.

## Migration

Migration from a single configuration file is manual:

1. Create a configuration directory.
2. Move web and logging settings into `app.toml` and add the selected `vault`;
   move `data`, `mirror`, and `modify` into a vault file and add `vault_name`.
3. Adjust the data path for the new directory and make `mirror` and `modify`
   absolute. Existing databases do not need to move.
4. Put any R2-only `.env` in the configuration directory.
5. Move `<database>.openai-auth.json` to `openai-auth.json` in the configuration
   directory, or sign in again.

Packages provide `cha-config.example/app.toml` and `personal.toml`. They never
contain private credentials or databases. Model API keys must be created in
Settings → API Keys, which writes the process-wide `api-keys.json`.

## Verification

Tests should cover the behavior at its actual boundaries:

- configuration bootstrap, discovery, validation, command selection, and
  credentials;
- switching between two databases, early lease/validation failures, drain
  failure, fatal reopen, mirror failure, and selection persistence;
- runtime vault creation with and without a copy source, updates, active/last
  deletion protection, and preservation of deleted-vault data;
- bootstrap fields, switch endpoint responses, selector pending/error behavior,
  and the initiating-page reload;
- native first-run files, current-vault menu capabilities, and window title;
- launcher and package contents using the directory layout.

There is no need for snapshot-identity, cross-tab mismatch, generic retarget,
mirror-precheck, or additional concurrency test matrices.

## Non-goals

Vaults do not add:

- more than one active vault;
- per-browser or per-user selection;
- live rescanning or filesystem watching;
- per-vault API keys, `.env`, or OAuth credentials;
- automatic migration or compatibility with single-file `--config`;
- automatic cross-tab reload; or
- process restart as the normal switching mechanism.

The feature remains a small process-wide list of local database configurations,
one current selection, and one explicit database-switch operation.
