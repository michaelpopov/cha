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
├── .env
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
# mirror = "personal-mirror"
# modify = "personal-edit"
```

The filename does not identify the vault. `vault_name` is the displayed and
saved name. Relative `data`, `mirror`, and `modify` paths resolve from the
configuration directory.

CHA reads vault definitions once at startup. Adding, removing, or editing a
definition takes effect after restart. A successful switch only changes the
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
- A `modify` directory cannot overlap another `modify` directory, a configured
  database, or the configuration directory. Export replaces this directory.
- Mirror paths do not participate in cross-vault collision validation. The
  mirror is a disposable projection and never deletes a whole tree.

Discovery validates definitions and path relationships. It does not open every
database. A nonselected database may be absent; import can create it. Normal
startup and runtime switching require the selected database to be valid.

## Application-wide files

`.env` and `openai-auth.json` live in the configuration directory rather than
inside a vault. CHA loads `.env` once at startup without overriding variables
already present in the process environment. Console commands continue to use
that process environment.

One `OpenAiOAuth` owner uses `openai-auth.json` for the life of the process.
Login, logout, and pending device authorization therefore apply to every vault
and survive a switch. Neither file is included in workspace import/export,
session mirrors, or R2 transfers.

Provider definitions still come from the active workspace. The process-wide
provider supervisor needs no vault-switch behavior: closing live sessions
cancels their requests, and newly opened sessions use definitions from the new
workspace.

## Startup and console commands

At startup CHA discovers the vaults, loads the application-wide environment and
OAuth owner, and opens the database selected by `app.toml`. Failure to open that
database fails startup.

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

## macOS application

The native application passes its Application Support directory as the
configuration path. On first run it creates `app.toml` selecting `Personal` and
`personal.toml` pointing to `cha.sqlite3` and `modify` beneath that directory.

The pre-runtime seed bridge reads `app.toml`, discovers the selected vault, and
creates that database if it is missing. It does not need a separate `--vault`
argument and does not change the selection.

Native Import, Export, Upload, and Download always consult the current vault.
The Database menu refreshes when opened so Import/Export availability follows
the selected vault's optional `modify` path.

## Failure behavior

| Failure | Result |
| --- | --- |
| Invalid configuration or startup vault | Startup fails. |
| Console command omits or misnames `--vault` | It fails before touching vault data. |
| Switch target is unknown | The old vault remains active. |
| Target lease is busy or target database is invalid | The switch fails before live sessions are paused. |
| Live sessions do not drain in time | The old vault remains active and admission resumes. |
| Reopen fails after the database path changes | The runtime requires restart; the saved selection is unchanged. |
| Mirror rebuild fails | The switch succeeds with mirroring inactive. |
| Saving `app.toml` fails | The switch succeeds; the next launch uses the previously saved vault. |

The `app.toml` rewrite uses a temporary sibling file and rename, so a failed
save does not leave a truncated configuration.

## Migration

Migration from a single configuration file is manual:

1. Create a configuration directory.
2. Move web and logging settings into `app.toml` and add the selected `vault`;
   move `data`, `mirror`, and `modify` into a vault file and add `vault_name`.
3. Adjust relative paths for the new directory. Existing databases do not need
   to move.
4. Move `.env` into the configuration directory.
5. Move `<database>.openai-auth.json` to `openai-auth.json` in the configuration
   directory, or sign in again.

Packages provide `cha-config.example/app.toml` and `personal.toml`. They never
contain private credentials or databases. The import seed's placeholder `.env`
is intentional.

## Verification

Tests should cover the behavior at its actual boundaries:

- configuration discovery, validation, command selection, and credentials;
- switching between two databases, early lease/validation failures, drain
  failure, fatal reopen, mirror failure, and selection persistence;
- bootstrap fields, switch endpoint responses, selector pending/error behavior,
  and the initiating-page reload;
- native first-run files and current-vault menu capabilities; and
- launcher and package contents using the directory layout.

There is no need for snapshot-identity, cross-tab mismatch, generic retarget,
mirror-precheck, or additional concurrency test matrices.

## Non-goals

Vaults do not add:

- more than one active vault;
- per-browser or per-user selection;
- UI for creating or editing vault definitions;
- live rescanning or filesystem watching;
- per-vault `.env` or OAuth credentials;
- automatic migration or compatibility with single-file `--config`;
- automatic cross-tab reload; or
- process restart as the normal switching mechanism.

The feature remains a fixed list of local database configurations, one current
selection, and one explicit database-switch operation.
