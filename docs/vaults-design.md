# Vaults design

## Purpose

A CHA vault is one self-contained collection of CHA data. It consists of:

- a SQLite database containing the workspace configuration and sessions; and
- a vault configuration file that names the vault and points to that database.

A machine may have one or more configured vaults, but a running CHA application
has exactly one active vault. The active vault supplies the current in-memory
`Workspace`, session repository, live sessions, mirror directory, modification
directory, and database used by maintenance operations.

Vault selection is global to the application process. It is not scoped to a
browser, tab, or user. Switching vaults retargets the application's
vault-specific runtime state at the new vault and republishes its workspace.

OpenAI login is application-wide rather than vault-specific. It remains active
when the current vault changes.

## Configuration directory

The `--config` option names a directory rather than a TOML file:

```text
chaweb --config=/absolute/path/to/cha-config
```

There is no compatibility mode for the previous single-file `--config`
argument. Passing a file instead of a configuration directory is an error.

A representative directory is:

```text
cha-config/
├── app.toml
├── openai-auth.json
├── .env
├── personal.toml
└── projects.toml
```

`app.toml` is the only reserved filename. Vault filenames have no identity or
meaning of their own. `personal.toml` could define a vault named `Archive`, and
renaming the file would not rename the vault.

CHA scans only the direct children of the configuration directory. It does not
search subdirectories. Every other file whose name ends in `.toml` is a vault
definition. Files with other extensions, such as `openai-auth.json` and
`.env`, are not vault definitions and are ignored by discovery.

The discovered vault set is fixed for the lifetime of the process. Adding,
removing, or editing a vault file takes effect after CHA is restarted. The one
exception is that CHA updates the `vault` selection in `app.toml` after a
successful runtime switch.

### Migration from the single configuration file

There is no compatibility mode, so upgrading an existing installation is a
one-time manual step:

- `cha.toml` becomes a directory. Its `[web]` and `[logging]` tables move to
  `app.toml` together with the new `vault` field; its `data`, `mirror`, and
  `modify` fields move to one vault file with a `vault_name`. Relative paths
  keep resolving against the directory that holds them.
- The OpenAI credential file moves from `<database>.openai-auth.json` to
  `cha-config/openai-auth.json`. CHA does not move it. An upgraded
  installation starts signed out until the user signs in again through
  Settings, or moves the old file by hand before starting CHA.
- The `.env` file moves from the database directory to the configuration
  directory. It is moved by hand as well.
- R2 backups keep their names, because the object key is still the database
  filename.

The following describe or generate the single-file layout today and change
with it: `packaging/linux/cha.toml.example`, which becomes an example
`app.toml` and vault file; the `CONFIG` default and the seed hint in
`bin/start-cha.sh`, whose hint gains `--vault`; the `import-dev` target in the
`Makefile`, which passes `--vault`; the macOS first-run writer, which creates
`app.toml` and one vault file instead of `cha.toml`; and the configuration
descriptions in `README.md` and `docs/MaintainerGuide.md`, including the
guide's table of configuration locations.

## Application configuration

Application-wide settings live in the reserved `app.toml`. The existing web,
logging, and other application settings retain their roles. Database and
workspace paths no longer belong here. A new required `vault` field selects the
vault to open at startup:

```toml
vault = "Personal"

[web]
host = "127.0.0.1"
port = 8080

[logging]
file = "/absolute/path/to/logs/cha.log"
level = "info"
```

The value of `vault` is matched case-insensitively against the `vault_name`
values discovered in the other TOML files. The configured spelling of the
matching vault's `vault_name` is canonical and is used in the UI. For example,
`vault = "personal"` selects a vault whose definition says
`vault_name = "Personal"`.

The `vault` field must be present and must select one discovered vault.
Startup fails if it is missing, empty, or has no match. Startup also fails if
the directory defines no vaults.

When a user changes the active vault, CHA rewrites `app.toml` with the selected
vault's canonical name. CHA is allowed to rewrite the complete TOML file; it
does not promise to preserve comments or formatting. The selected vault is
therefore also the default on the next application launch.

The rewrite must never truncate `app.toml` in place. It uses the same
temp-file-and-rename transaction as workspace `rewrite_config`: parse the
current table, change `vault`, write and flush a uniquely named sibling file,
close and verify it, and atomically rename it over `app.toml`. A failure removes
the temporary file and leaves the original `app.toml` intact. Thus an
interrupted switch leaves either the complete old application configuration or
the complete new one, never a torn file that prevents the next startup.

The implementation should share or extract the existing rewrite helper rather
than introduce a second in-place or ad hoc application-config writer.

### Application-wide OpenAI credentials

OAuth tokens are stored in a private local JSON file, not in a vault's SQLite
database. The application uses one credential file in the configuration
directory:

```text
cha-config/openai-auth.json
```

One `OpenAiOAuth` owner loads and writes this file for the lifetime of the
application runtime. The file is outside every vault database and is not part
of workspace import/export, session mirroring, or R2 database transfer. Its
existing private-file permissions and validation rules continue to apply.

The login is shared by every vault discovered through that `app.toml`. Signing
in while one vault is active makes the same account available to subscription
providers in every other vault. Disconnecting from any vault disconnects the
application-wide account. A pending device-login attempt also survives a vault
switch.

Provider definitions come from each vault's workspace, but the `Providers`
supervisor is process-wide and stores none of them; each request carries its
own character definition. Its client factory holds a reference to the stable
application-wide `OpenAiOAuth` owner. Switching drains the live sessions,
which cancels their in-flight provider requests, and requests made after the
switch carry definitions from the new vault's workspace through the same
supervisor, OAuth owner, and credential bundle. Vault switching must not load,
replace, delete, or copy an OAuth credential file.

### Application-wide environment file

API keys and R2 credentials may be supplied in one `.env` file in the
configuration directory:

```text
cha-config/.env
```

CHA loads it once when the runtime starts, before the first vault is opened,
with the existing rule that a variable already present in the process
environment is not overridden. The directory of a vault's database is no
longer consulted. A per-vault file could not work with that rule: after a
switch, the first vault's `OPENAI_API_KEY` would silently shadow the second
vault's. Like the credential file, `.env` is outside every vault and is not
part of workspace import/export, session mirroring, or R2 database transfer.
Console maintenance commands keep reading credentials from the process
environment, as they do today.

## Vault configuration

Each vault is defined by a direct-child `.toml` file other than `app.toml`:

```toml
vault_name = "Personal"
data = "/absolute/path/to/personal.sqlite3"
mirror = "/absolute/path/to/personal-mirror"
modify = "/absolute/path/to/personal-edit"
```

The fields are:

| Field | Required | Meaning |
| --- | --- | --- |
| `vault_name` | Yes | Stable vault name and the text displayed by the vault selector. |
| `data` | Yes | SQLite database containing the vault's workspace and sessions. |
| `mirror` | No | Directory receiving the vault's session mirror. |
| `modify` | No | Directory used by the vault's configuration import and export operations. |

Relative `data`, `mirror`, and `modify` values are resolved against the
configuration directory, exactly as the single file resolved them against its
own directory. The database and optional directories do not have to exist
during discovery. In particular, `--import` and the macOS Import operation
must be able to initialize a vault whose database does not yet exist.

Unknown fields are rejected, exactly as in `app.toml`, so a misspelled optional
field such as `mirrror` is reported instead of silently ignored. CHA verifies
that each known field is present when required, has the correct TOML type, and
has a valid value.

### Vault-name rules

`vault_name` follows the same textual validation rules as a forum display name:

- it is a non-empty string;
- it contains valid UTF-8;
- it contains no control characters or line breaks; and
- it has no leading or trailing whitespace.

Names may contain Unicode, spaces, and punctuation. `Entrance` is allowed as a
vault name.

Vault identity and matching are case-insensitive. Two otherwise valid vault
files whose names differ only by case are duplicates; for example, `Personal`
and `personal` cannot both be configured. Duplicate names are a fatal
configuration ambiguity rather than two entries in the selector.

The spelling from `vault_name` is preserved for display and when CHA persists
the selection to `app.toml`.

### Cross-vault path safety

Vault paths are normalized before the registry is accepted, using the same
absolute, weakly canonical form as command-line paths, so `..` components and
symlinked parents cannot hide aliases. Registry-wide validation then applies
to every vault definition, including vaults that are not selected at startup,
and compares normalized paths.

The complete registry must satisfy these invariants:

- Every vault has a distinct `data` path. Two names cannot refer to the same
  SQLite database.
- Every vault has a distinct `data` filename, because R2 names a backup after
  the local filename. See [R2 object identity](#r2-object-identity).
- No `modify` root equals, contains, or is contained by another vault's
  `modify` root. Export removes the whole `modify` tree before writing, so a
  shared or nested root would let one vault's export delete another's edit
  tree.
- No `modify` root equals or contains any vault's `data` file or the
  configuration directory. This generalizes the existing single-file rule
  that `modify` must not contain the config file or the database, so
  `remove_all(A.modify)` can never delete Vault B's database, `app.toml`, or
  a vault definition.

`mirror` roots are not validated against each other or against the other
paths. The mirror is a best-effort projection that only creates directories
and replaces individual Markdown files, so a misconfigured overlap degrades
that projection without touching primary data. Hard-link aliases of an
existing database are likewise not detected; normalized path equality is
enough for a personal configuration.

Containment is checked in either direction for `modify` roots. Sibling paths
are allowed. For example, these paths are independent:

```text
/srv/cha/personal/personal.sqlite3
/srv/cha/personal/mirror/
/srv/cha/personal/modify/
/srv/cha/work/work.sqlite3
/srv/cha/work/mirror/
/srv/cha/work/modify/
```

A registry-wide collision is fatal to configuration discovery. CHA must not
silently keep whichever definition happened to be scanned first, because that
would make the selected vault and destructive operation targets dependent on
directory iteration order. The diagnostic names both vaults, both fields, and
the conflicting normalized path.

## Discovery

After reading `app.toml`, CHA reads every other direct-child `.toml` file as a
vault definition. For each file:

1. Read and parse the TOML document.
2. Validate `vault_name`, `data`, and any optional `mirror` and `modify`
   fields, and reject unknown fields exactly as `app.toml` does.
3. Add the definition to the available vaults.

A file that cannot be read or parsed, or that fails validation, fails startup
with a diagnostic naming the file and the problem. CHA does not skip a bad
vault file and continue: skipping would silently remove a vault from the
selector, or hide a typo behind a log line, and the same strictness already
applies to `app.toml`.

After every file has been accepted, CHA checks case-insensitive vault-name
uniqueness and the registry-wide path invariants above.

Discovery validates the vault file, not the referenced database. A vault may
therefore appear in the available list even when its database is absent,
corrupt, incompatible, or temporarily inaccessible. The database is opened and
the workspace is validated when the vault is selected for normal use. A missing
database remains a valid target for an import operation that creates it.

After validation and duplicate detection, available vaults are presented
alphabetically by `vault_name`, using case-insensitive comparison.

## Runtime ownership

The HTTP routes are installed once when the server starts, and their handlers
capture the `SessionRepository`, `LiveSessionManager`, `WorkspaceConfigStore`,
and `SessionMirror` objects held by `ApplicationRuntime::Impl`. Destroying and
recreating those objects during a switch would leave the installed handlers
with dangling pointers. A switch therefore keeps every one of them for the
lifetime of the process and retargets them in place.

This is what `maintain_database` already does for an R2 download, which
replaces the database file underneath the running process. It drains every
live session under one deadline, holds the store and repository maintenance
guards, closes the SQLite handle, runs the operation, then reopens the
database, revalidates it, republishes the workspace through `loadws()`, and
resynchronizes forums. A vault switch is that operation with a different
database path.

```text
ApplicationRuntime::Impl
├── HTTP server, listener, assets, WebSettings
├── vault registry and current VaultDefinition   # which vault is active
├── OpenAiOAuth                                  # application-wide, config directory
├── Providers                                    # process-wide request supervisor
├── WorkspaceConfigStore                         # retargeted: lease, database, workspace
├── SessionRepository                            # retargeted: database path
├── SessionMirror                                # retargeted: root, or inactive
└── LiveSessionManager                           # unchanged; drained, then resumed
```

`Impl` holds the startup-time vault registry and the definition of the
current vault. Maintenance operations read the current vault's `data` and
`modify` paths from that definition instead of from the parsed command line,
and the macOS bridge asks for the current vault's capabilities instead of
caching them when the runtime is created.

`WorkspaceConfigStore` keeps its process lease, SQLite handle, private
materialized tree, and configuration mutex. Its `MaintenanceGuard` already
offers `close()` and `reopen()`; a switch adds a retarget step between them
that acquires the lease for the new database, releases the old lease, and
points the store at the new path. `reopen()` then validates the database at
that path, rematerializes the workspace into the same private tree, and
publishes it through `loadws()`, exactly as it does after a download. Because
the private tree is reused, `workspace_path()` and `welcome_path()` do not
change across a switch.

`SessionRepository` keeps its workspace root and its process-local Welcome
database. It gains a retarget operation, run under the exclusive maintenance
lock it already has, that points the repository at the new database; the
existing post-reopen step synchronizes forums from the republished workspace,
and archived rows in that database are purged at its next startup, as today.
Routes hold the same `shared_ptr` before and after.

`SessionMirror` keeps its identity too. It gains an inactive state for a vault
without `mirror`, and a retarget operation that clears its forum and session
maps and rebuilds them for the new vault's root, or leaves it inactive. The
opener and the lobby routes keep the pointer they captured; an inactive mirror
turns `add()` and `update()` into no-ops.

`Providers` is not vault-scoped. It is a process-wide supervisor for
independent request workers that stores no provider configuration, credential,
client, or scheduling state; each request carries the character definition
from the workspace that was current when its session opened. Draining the
live sessions cancels their in-flight requests, and a slow transport tail
finishes on its own, as after any cancelled request. A switch never creates or
destroys a `Providers`, so there is no unbounded destructor to wait for and no
background cleanup.

`LiveSessionManager` is unchanged. `reserve_global_maintenance()` closes
admission, stops every actor with `ShutdownReason::reloading`, and waits under
one deadline; destroying the reservation resumes admission on the same
manager. Sessions opened afterwards go through the same opener lambda, which
reads the retargeted store, repository, and mirror and passes the current
vault's canonical name to the new `LiveSession`.

`OpenAiAuthRoutes` capture the application-wide `OpenAiOAuth` owner directly.
Its status, login, polling, and disconnect operations do not touch vault state
and remain valid throughout a switch.

Because no object is replaced, there is no second runtime aggregate, no
per-request lease, no switching gate, and no activation epoch. A request that
arrives during a switch behaves as it does during an R2 download: a session
open is refused with the existing `session_stopping` response while global
maintenance is reserved, a session route reports `session_not_live` because
the old actor is gone, and a lobby request waits on the store or repository
maintenance guard and then runs against whichever vault is current. That last
case, like a stale tab acting on the lobby after the switch, can carry the old
tab's intent into the new vault. CHA accepts that exposure: it is a personal
application, the window is bounded by `shutdown_grace`, and the browser's
vault-name check reloads the tab as soon as it sees a snapshot.

## Startup behavior

Normal application startup proceeds as follows:

1. Resolve and validate the directory passed with `--config`.
2. Read the required `app.toml` and its application-wide settings.
3. Read every other `.toml` file as a vault definition; any invalid file fails
   startup.
4. Reject case-insensitive duplicate vault names, duplicate database paths or
   filenames, and unsafe `modify` overlap.
5. Find the vault named by `app.toml`'s `vault` field.
6. Load `cha-config/.env` into the process environment.
7. Create the application-wide `OpenAiOAuth` owner from
   `openai-auth.json` in the configuration directory.
8. Open the selected vault's SQLite database through `WorkspaceConfigStore`,
   which validates it and publishes its workspace, and record that vault as
   current.
9. Create the session repository, the optional mirror, the process-wide
   providers, and the live-session manager as today.
10. Start serving with that vault marked active.

The application does not have a usable partial state if its initially selected
vault cannot be opened. Normal startup fails with the existing diagnostic error
handling.

### Console maintenance commands

Console maintenance commands must name their target vault explicitly with
`--vault`:

```text
chaweb --config=/path/to/config --vault="Personal" --import /path/to/source
chaweb --config=/path/to/config --vault="Personal" --export /path/to/destination
chaweb --config=/path/to/config --vault="Personal" --upload
chaweb --config=/path/to/config --vault="Personal" --download
```

`--config` still identifies the configuration directory, and the command reads
`app.toml` for application-wide settings and performs the normal vault-file
discovery and cross-vault safety validation. `--vault` is matched
case-insensitively against the discovered vault names. The name must resolve to
exactly one discovered vault before CHA opens, creates, replaces, or removes any
vault data.

`--vault` is required whenever `--import`, `--export`, `--upload`, or
`--download` is used. It is not accepted during normal server startup; the
server continues to select its initial vault from `app.toml`. A command-line
vault selection applies only to that invocation and never rewrites the `vault`
field in `app.toml`.

Import may create the explicitly selected vault's missing database. Export,
upload, and download use that vault's `data` path. Configuration import and
export use that vault's `modify` path and remain unavailable when that optional
field is absent. In particular, `--download` cannot infer its destructive
destination from the application default: both the vault name and resulting
database path are established before the download begins.

### R2 object identity

An R2 backup keeps its existing name: the local database filename,
percent-encoded, under the configured bucket.

```text
/srv/cha/personal/personal.sqlite3  ->  /<bucket>/personal.sqlite3
/srv/cha/work/work.sqlite3          ->  /<bucket>/work.sqlite3
```

Nothing changes in the transfer code. Two vaults whose databases shared a
filename would upload to and download from the same object, so discovery
rejects duplicate `data` filenames instead; see
[Cross-vault path safety](#cross-vault-path-safety). Keying objects by vault
name was considered and rejected: it would orphan every existing backup, so an
upgraded installation's first `--download` would report that its object does
not exist.

Upload and download use the selected vault's `data` path as the local source
or destination and derive the object name from it, as today.

## Browser API

Vault information is part of the existing bootstrap response so the browser
does not need a separate discovery request. `GET /api/v1/bootstrap` adds the
canonical active name and the alphabetically ordered list:

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

The remaining bootstrap fields describe only the active vault's workspace and
sessions.

Every `SessionSnapshot`, including the initial event on an SSE stream, carries
the canonical name of the vault that was current when its live session opened:

```json
{
  "vault_name": "Personal",
  "session_id": "welcome",
  "...": "existing session snapshot fields"
}
```

The browser stores the name from bootstrap and checks it on every snapshot
before placing it in application state. A different name means the page
belongs to a vault that is no longer active. The browser discards that
snapshot and immediately navigates to `/` for a full reload. This check is
shared by normal session open, recovery probes, and SSE snapshot handling;
none of those paths may publish a mismatched snapshot.

The name alone identifies the activation. Switching away from a vault and
back to it needs no separate counter: a stale tab that reattaches to that
vault's session receives the session's complete current transcript in its
first snapshot, exactly as after any reload, and a process restart on the same
vault is indistinguishable from today's behavior.

The opener passes the current vault's canonical name into each `LiveSession`
when the session opens, and the session stamps that name into every snapshot
it produces. It does not consult the runtime's current vault while
serializing an event. Append events need no separate identity because their
stream begins with a checked snapshot and is closed when the vault's live
sessions are drained.

The browser selects a vault with:

```http
POST /api/v1/vault/switch
Content-Type: application/json

{"vault_name":"Projects"}
```

The requested name is matched case-insensitively against the discovered list.
Selecting the already active vault is an idempotent no-op. A successful request
returns `204 No Content` only after the switch is complete. A request for an
unknown vault returns `bad_request`. A target that fails its pre-checks, a
switch whose live sessions do not drain in time, or a failure after the old
database was closed returns `internal_error` carrying the maintenance message.
Both go through the existing JSON error-response mechanism, and nothing is
added to the protocol's closed error-code set, the OpenAPI schema, or the
browser runtime allowlist. The current vault does not change.

The switch route is installed by `ApplicationRuntime::start()` itself, because
it calls the runtime's maintenance operation. Every other route is unchanged
and keeps the objects it captured at installation, as described in
[Runtime ownership](#runtime-ownership).

## Sidebar UI

The bottom of the sidebar contains one horizontal row:

```text
┌──────────────────────────────┐
│ [ Personal              ▾ ] ⚙ │
└──────────────────────────────┘
```

The vault selector occupies the available space on the left. The existing
Settings gear remains on the right at the same visual level. The selector is a
small, polished control consistent with the rest of the sidebar; it does not
need a complex custom menu. Each option displays only its canonical
`vault_name`, ordered alphabetically.

The current vault is selected when bootstrap completes. Choosing that same
entry does nothing. While a change request is in progress, the selector is
disabled. No additional switching progress indicator is required.

If switching fails, the current page and old workspace remain usable, the
selector returns to the active vault, and the selector becomes enabled again.
The browser reports the failure using its normal application error treatment.

After a successful switch, the initiating browser navigates to `/` and performs
a full page reload. It then bootstraps from the new vault and opens that vault's
initial Welcome session. It does not attempt to preserve the previous screen,
forum, session URL, conversation, or draft across vaults.

No separate cross-tab broadcast channel is required. Closing the old vault's
live sessions closes their SSE streams. Another tab follows its existing
recovery ladder, which is bounded and ends in the retry state it has today. If
a session that the tab had already displayed is absent from the new vault,
`openSession` returns not-found and the ladder ends there, exactly as for a
deleted session; no new recovery rule is added. If the same session
identifiers exist in the new vault, the next snapshot's vault name differs
from the tab's bootstrap name and forces the reload. In particular, a tab
cannot silently reattach to the new vault's `entrance/welcome`, whose
identifiers intentionally exist in every vault.

## Runtime switching lifecycle

A switch must not touch the working vault before the cheap checks that catch
the common failures have passed, and it must not let a live session or a
route observe half of a switch. It runs as one `maintain_database` operation,
so it is serialized by the runtime's lifecycle mutex with every other
maintenance operation, and it reuses that operation's drain, close, reopen,
and failure handling.

### Pre-checks

Before any live session is touched:

1. Resolve the requested name in the startup-time vault registry.
2. If it is the active vault, succeed without changing anything.
3. Require `inspect_workspace_session_database` to report a valid current
   database at the target's `data` path. A missing or corrupt file, another
   application's database, or an unsupported schema version fails here.
4. Require the target's `mirror` directory to exist when one is configured.

A pre-check failure returns an error without draining sessions or touching
the old vault.

### Switch

The remaining steps are the existing `maintain_database` sequence with the
retarget as its operation and two additions after the reopen:

1. Reserve global maintenance on the live-session manager with
   `WebSettings::shutdown_grace` as the drain deadline. This closes admission,
   stops every actor with `ShutdownReason::reloading`, ends their SSE streams,
   and waits for them to finish. Expiration is the ordinary
   `MaintenanceFailure` result: the reservation is released, admission
   resumes on the old vault, and the request fails with the existing "could
   not pause active sessions" error. Unfinished actors stay in their normal
   stopping state until they finish and are swept; nothing is destroyed or
   joined without a bound.
2. Take the store and repository maintenance guards, checkpoint, and close the
   SQLite handle, as for a download.
3. Retarget. Acquire the process lease for the target database, then release
   the old lease, point the store and the repository at the new path, and
   record the target as the current vault. The lease acquisition is the only
   part of this step that can fail, and it runs before anything is
   re-pointed. A busy lease, held for example by a console `--import` on that
   vault, fails here; the existing failure path then reopens the old database
   at its unchanged path, and the old vault stays current with its sessions
   closed but stored.
4. Reopen. The store validates the target database, rematerializes its
   workspace into the same private tree, and publishes it through
   `loadws()`; the repository synchronizes forums from it. A failure here is
   the same outcome as a failed download: the store stays closed, the runtime
   reports `WorkspaceRestartRequiredError`, and the process must restart.
   `app.toml` has not been rewritten, so the next launch opens the previous
   vault.
5. Rebuild the mirror for the target's `mirror` root, or leave it inactive
   when the target has none. Mirroring is best-effort, so an I/O failure here
   is logged and leaves the mirror inactive instead of failing the switch.
6. Rewrite `app.toml` with the target's canonical name using the shared
   temp-file-and-rename helper. The process has already switched, so a
   failure here is logged and does not fail the request; the next launch
   opens the previously saved vault.
7. Release the maintenance guards and the global reservation, which resumes
   live-session admission on the new vault, and return `204 No Content`.

Steps 5 and 6 run before the reservation is released so that no session can
open against the new vault while the mirror still describes the old one.

Closing the old vault's live sessions removes their runtime objects only. It
does not delete stored sessions from the old vault's database. Those sessions
are available again when that vault is selected later.

All vault-specific resources change together and before admission resumes.
After the switch, every new API operation uses the new database, workspace,
session repository, `mirror`, and `modify` settings. This includes
configuration import and export and database upload and download. No
operation may continue using paths cached from the startup vault.

Vault selection is global even if more than one browser happens to be
connected. CHA is a personal application and does not require elaborate
multi-client coordination or consensus. The browser that initiated the switch
reloads immediately. Other browsers detect the change through the vault name
on the next snapshot they receive and reload before accepting state from the
new vault; a tab whose session no longer exists ends in the existing retry
state.

## Why switching stays in process

Updating `app.toml` and restarting CHA is not an implementation alternative for
this feature. The packaged Linux `bin/start-cha.sh` performs a one-shot
background `nohup`; it is not a supervisor and does not restart a process that
exits. Development and macOS launches also commonly request `port = 0`, so a
new runtime receives a different ephemeral port while the browser or WKWebView
still points at the old origin. A process that exits also cannot complete the
switch request with `204`.

The HTTP server and listener must therefore survive the switch. Only the
vault-scoped resources are retargeted, underneath the routes that already hold
them.

## macOS application

The macOS application uses the same configuration-directory layout and vault
rules as the console application:

- its configuration argument names the directory containing `app.toml` and
  vault definitions, which is its Application Support directory;
- on first run it writes `app.toml` selecting `Personal` and a `personal.toml`
  whose `data` and `modify` are relative paths under that directory, in place
  of the single `cha.toml` it writes today, and continues to write `.env`
  beside them;
- it discovers and selects the startup vault in the same way;
- the browser UI switches the single in-process runtime globally;
- its OpenAI login remains application-wide and unchanged across vault
  switches;
- native Import, Export, Upload, and Download operations always act on the
  current vault; and
- Import can create the current vault's database when it does not exist.

The bundled first-run database seed is a special pre-runtime path: it runs
before the runtime exists, so there is no current vault to query.
`cha_runtime_import_initial_database` receives the configuration directory,
reads `app.toml`, performs normal vault discovery, resolves the vault named by
`app.toml`, and seeds that vault's `data` path if it is missing. It does not
require a separate `--vault` argument and does not change the selection. The
mandatory console `--vault` rule applies to user-invoked console maintenance
commands, not this private native startup bridge.

Capabilities derived from optional vault settings are dynamic. For example,
whether native Import and Export are available follows the active vault's
`modify` field rather than a value cached when the application first launched.

## Failure behavior

The important failure cases are:

| Failure | Behavior |
| --- | --- |
| `app.toml` is missing or invalid | Startup fails. |
| No vault definitions are found | Startup fails. |
| Any vault file cannot be read, parsed, or validated | Startup fails, naming the file and the problem. |
| Two definitions have the same case-insensitive name | Startup fails because selection would be ambiguous. |
| Two vaults resolve to the same `data` path | Discovery fails before either database is opened. |
| Two vault databases have the same filename | Discovery fails, because R2 names a backup after the filename. |
| A `modify` tree overlaps another `modify` tree, a database, or the configuration directory | Discovery fails before any destructive operation is available. |
| `app.toml` names a vault that is not defined | Startup fails. |
| A variable is set both in the shell and in `cha-config/.env` | The shell value wins, as today; the file never overrides an existing variable. |
| The startup database cannot be loaded during normal startup | Startup fails. |
| A console maintenance command omits `--vault` | The command fails before touching any vault data. |
| A console maintenance command names an unknown vault | The command fails before touching any vault data. |
| An imported vault database does not yet exist | Import may create it. |
| A runtime switch target fails its pre-checks | The old vault remains active and untouched; the browser shows the failure. |
| Live sessions do not finish within `shutdown_grace` | The switch fails with the existing maintenance error; the old vault remains active, `app.toml` is unchanged, and unfinished actors are retained safely. |
| The target database's lease is held by another process | The retarget fails before the store is re-pointed; the old database is reopened and the old vault remains active with its sessions closed but stored. |
| The target database fails validation after the old handle was closed | Restart is required, as after a failed download; `app.toml` still names the old vault, so the next launch opens it. |
| The target's mirror cannot be rebuilt | The switch succeeds with the mirror inactive; the failure is logged. |
| `app.toml` cannot be rewritten after the switch | The process is serving the new vault; the failure is logged; the next launch opens the previously saved vault. |
| A request arrives during a switch | A session open gets `session_stopping`, a session route gets `session_not_live`, and a lobby request waits for the maintenance guards and then runs against the current vault, as during a download. |
| A stale browser receives a snapshot with a different vault name | It discards the snapshot and reloads `/` before displaying or mutating the new vault. |
| Recovery cannot find a session that the tab previously displayed | The existing bounded ladder ends in its retry state, as for a deleted session; the first snapshot the tab accepts afterwards must carry the current vault name. |
| An old provider transport ignores cancellation after a successful switch | `Providers` is process-wide and untouched by the switch; the tail finishes on its own without blocking anything. |

## Tests

The change is verified where the code it touches is already tested:

- `tests/web/unit_application_config.cpp`: the directory form of `--config`,
  `app.toml` validation including the required `vault` field, vault-file
  validation and unknown-field rejection, case-insensitive duplicate names,
  duplicate `data` paths and filenames, `modify` overlap, and the `--vault`
  requirement and matching for console commands.
- `tests/web/unit_application_runtime.cpp`, which already drives upload and
  download through `maintain_database`: a switch between two vault databases
  closes the live sessions, republishes the workspace, retargets the
  repository and mirror, and rewrites `app.toml`; selecting the active vault
  is a no-op; a pre-check failure leaves the old vault serving; a busy target
  lease reopens the old vault; a target whose rows fail validation reports
  `WorkspaceRestartRequiredError` with `app.toml` unchanged.
- `tests/web/unit_lobby_routes.cpp` and `tests/web/unit_protocol.cpp`:
  `vault_name` and `vaults` in bootstrap, `vault_name` in every snapshot, and
  the switch route's `204`, `bad_request`, and `internal_error` responses.
- `tests/web/unit_session_mirror.cpp`: retargeting to a new root and to the
  inactive state.
- `webapp/src`: bootstrap validation of the new fields, the snapshot
  vault-name check forcing a reload, and the sidebar selector's disabled and
  failure states.

## Deliberate non-goals

This design does not introduce:

- more than one simultaneously active vault;
- per-browser or per-user vault selection;
- per-vault OpenAI login, credential, or `.env` files;
- recursive vault discovery;
- live rescanning or filesystem watching;
- UI for creating, deleting, renaming, or editing vault definitions;
- fallback compatibility with the old single configuration file;
- database opening as part of vault-file discovery; or
- stopping and externally restarting the process to perform a vault switch.

These constraints keep vaults as a small composition feature: a fixed registry
of external database configurations, one active runtime, and one explicit
switch operation.
