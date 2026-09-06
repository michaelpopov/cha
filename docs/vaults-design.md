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
browser, tab, or user. Switching vaults replaces the application's active
workspace and all of its vault-specific runtime state.

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
├── personal.toml
├── projects.toml
└── unrelated.toml
```

`app.toml` is the only reserved filename. Vault filenames have no identity or
meaning of their own. `personal.toml` could define a vault named `Archive`, and
renaming the file would not rename the vault.

CHA scans only the direct children of the configuration directory. It does not
search subdirectories. Only files whose names end in `.toml` participate in
discovery.

The discovered vault set is fixed for the lifetime of the process. Adding,
removing, or editing a vault file takes effect after CHA is restarted. The one
exception is that CHA updates the `vault` selection in `app.toml` after a
successful runtime switch.

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

The `vault` field must be present and must select one valid discovered vault.
Startup fails if it is missing, empty, or has no match. Startup also fails if
the directory contains no valid vaults.

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

Provider definitions and provider runtime collections remain vault-scoped
because they come from each vault's workspace. Each vault's provider factory
receives a reference to the stable application-wide `OpenAiOAuth` owner.
Switching cancels the old vault's provider work and retires those clients; any
transport tail that does not stop promptly is cleaned up away from the switch
request. The new vault's clients use the same OAuth owner and credential
bundle. Vault switching must not load, replace, delete, or copy an OAuth
credential file.

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

`data`, `mirror`, and `modify` must be absolute paths when present. Relative
paths make the vault definition invalid. The database and optional directories
do not have to exist during discovery. In particular, `--import` and the macOS
Import operation must be able to initialize a vault whose database does not yet
exist.

Unknown fields in a vault file are ignored. This allows a vault file to carry
unrelated metadata without making it unavailable. CHA still verifies that each
known field is present when required, has the correct TOML type, and has a valid
value.

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

Vault paths are normalized before the registry is accepted, using an absolute,
weakly canonical form so `..` components and existing symlinked parents cannot
hide aliases. Registry-wide validation then applies to every individually valid
vault definition, including vaults that are not selected at startup.

Existing data files are additionally compared with filesystem equivalence so
two hard-link paths to the same file are duplicates. For paths that do not yet
exist, normalized path equality is used.

The complete registry must satisfy these invariants:

- Every vault has a distinct `data` path. Two names cannot refer to the same
  SQLite database.
- Every `mirror` root is distinct from and non-overlapping with every other
  `mirror` root. Two vaults cannot write into the same or nested mirror trees.
- Every `modify` root is distinct from and non-overlapping with every other
  `modify` root. Exporting one vault cannot replace another vault's edit tree.
- No `mirror` and `modify` roots overlap, whether they belong to the same vault
  or different vaults.
- No vault's `data` file is equal to or contained by any vault's `mirror` or
  `modify` tree. In particular, Vault A's export must never be able to delete
  Vault B's database with `remove_all(A.modify)`.
- The configuration directory does not overlap any `mirror` or `modify` tree,
  so export or mirror writes cannot replace `app.toml` or vault definitions.

For directory roots, “overlap” means equality or containment in either
direction. Sibling paths are allowed. For example, these paths are independent:

```text
/srv/cha/personal/cha.sqlite3
/srv/cha/personal/mirror/
/srv/cha/personal/modify/
/srv/cha/work/cha.sqlite3
/srv/cha/work/mirror/
/srv/cha/work/modify/
```

A registry-wide collision is fatal to configuration discovery. CHA must not
silently keep whichever definition happened to be scanned first, because that
would make the selected vault and destructive operation targets dependent on
directory iteration order. The diagnostic names both vaults, both fields, and
the conflicting normalized path.

## Discovery and invalid files

After reading `app.toml`, CHA examines the other direct-child `.toml` files.
For each file:

1. Read and parse the TOML document.
2. Look for `vault_name`.
3. If `vault_name` is absent, ignore the file; it is not a vault definition.
4. If it is present, validate `vault_name`, `data`, and any optional `mirror`
   and `modify` fields.
5. If validation succeeds, add the definition to the available vaults.
6. If the file cannot be read, cannot be parsed, or has invalid known fields,
   skip it and write a diagnostic log message naming the file and problem.

After individual files have been filtered, CHA checks case-insensitive vault-
name uniqueness and the registry-wide path invariants above. Unlike one invalid
file, an ambiguity between two otherwise valid definitions fails discovery.

An invalid vault definition does not prevent startup unless `app.toml` selects
it and no valid definition with that name exists. A malformed non-`app.toml`
file is likewise skipped and logged because CHA cannot reliably determine its
contents.

Discovery validates the vault file, not the referenced database. A vault may
therefore appear in the available list even when its database is absent,
corrupt, incompatible, or temporarily inaccessible. The database is opened and
the workspace is validated when the vault is selected for normal use. A missing
database remains a valid target for an import operation that creates it.

After validation and duplicate detection, available vaults are presented
alphabetically by `vault_name`, using case-insensitive comparison.

## Runtime ownership

Vault switching cannot be implemented by replacing the fields currently held
directly by `ApplicationRuntime::Impl`. The HTTP routes are installed once when
the server starts, and today their handlers capture the selected vault's
`SessionRepository`, `LiveSessionManager`, `WorkspaceConfigStore`,
and `SessionMirror` objects. Destroying those objects during a switch would
leave the installed handlers with dangling pointers.

The runtime therefore introduces one vault-scoped aggregate and one
application-scoped holder:

```text
ApplicationRuntime
├── HTTP server, listener, assets, WebSettings, vault registry
├── OpenAiOAuth                                 # stable, application-scoped
├── old-runtime reaper                          # application-scoped cleanup
└── VaultRuntimeHolder                         # stable for process lifetime
    └── shared VaultRuntime                    # atomically replaceable
        ├── VaultDefinition and activation epoch
        ├── WorkspaceConfigStore / Workspace
        ├── SessionRepository
        ├── LiveSessionManager
        ├── SessionMirror (optional)
        └── Providers                          # uses shared OpenAiOAuth
```

`VaultRuntimeHolder` follows the existing `getws()`/`loadws()` publication
pattern, but publishes the complete vault runtime instead of only a
`Workspace`. It owns the current `shared_ptr<VaultRuntime>`, the switch-in-
progress state, and the count of active vault-route leases.

`LobbyRoutes` and `SessionRoutes` are changed to capture the stable holder, not
a vault runtime or any member of one. This includes the vault-derived `/health`
response. Each handler acquires one holder lease at the start of a request and
uses only the `VaultRuntime` obtained by that lease. It must not fetch the
repository from one holder read and the live-session manager from another. The
SSE content and cleanup callbacks retain the same lease until the stream
closes.

`OpenAiAuthRoutes` instead captures the stable application-wide `OpenAiOAuth`
owner. Its status, login, polling, and disconnect operations do not acquire a
vault lease and remain valid while a vault switch is committing.

The application-level vault-switch route also captures the holder, but it does
not acquire an ordinary vault-route lease. Native macOS maintenance entry
points acquire the current vault through the same holder instead of caching
startup paths or capabilities.

The old-runtime reaper prevents destruction from blocking the HTTP switch
request. `Providers::~Providers()` currently cancels active requests and then
waits without a deadline for every transport and diagnostic tail to leave. The
request thread must therefore never perform the final release of a retired
`VaultRuntime`. After publication, ownership of the complete old runtime is
handed to the application-scoped background reaper, which destroys it and may
wait there for provider shutdown. The new runtime is already independent and
continues serving even if an old provider transport ignores cancellation. The
reaper retains all old-runtime and shared OAuth lifetime needed by those
workers; a detached worker must not refer to an object that has already been
destroyed.

The holder has a short switching gate. Once the commit phase begins, new
vault-scoped requests are rejected with a retryable `503 vault_switching`
response. Routes that already hold a lease keep the old `VaultRuntime` alive
until they finish, so no request can dereference destroyed state. The switch
waits for those leases to drain before publication. Closing old live sessions
causes their long-lived SSE leases to finish. The gate also serializes switch
requests, so a second switch cannot prepare or publish over one already being
committed.

Side loading must not call the current `WorkspaceConfigStore::open()` path if
that path publishes through `loadws()`. Preparation needs an unpublished form
that returns a fully loaded store and `Workspace` without changing `getws()`.
The same rule applies to constructors that currently consult `getws()` while
they initialize. In particular, candidate `SessionRepository` synchronization
must receive the prepared `Workspace` explicitly instead of comparing itself
to the still-current old workspace.

During commit, while the holder gate excludes vault requests, the already
prepared workspace and complete `VaultRuntime` are published together. New
leases are admitted only after both publications refer to the new vault. This
prevents a route from observing a new repository with the old global workspace,
or the reverse.

## Startup behavior

Normal application startup proceeds as follows:

1. Resolve and validate the directory passed with `--config`.
2. Read the required `app.toml` and its application-wide settings.
3. Scan the directory and build the fixed list of valid vault definitions.
4. Reject case-insensitive duplicate vault names, duplicate database paths, and
   unsafe cross-vault path overlap.
5. Find the vault named by `app.toml`'s `vault` field.
6. Create the application-wide `OpenAiOAuth` owner from
   `openai-auth.json` in the configuration directory.
7. Open the selected vault's SQLite database and load and validate its
   workspace.
8. Create a complete `VaultRuntime` for its workspace, sessions, providers,
   and optional mirror; its providers reference the shared OAuth owner.
9. Publish that runtime through `VaultRuntimeHolder` and start serving the
   application with that vault marked active.

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
exactly one valid vault before CHA opens, creates, replaces, or removes any
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

An R2 backup belongs to a vault, not to the basename of its local database.
Deriving the object key from `data.filename()` would make
`/personal/cha.sqlite3` and `/work/cha.sqlite3` upload to and download from the
same remote object.

Upload and download therefore receive both the selected vault identity and its
database path. The object name is the percent-encoded, case-normalized vault
name followed by `.sqlite3`:

```text
vault_name = "Personal"  ->  /<bucket>/personal.sqlite3
vault_name = "Work Notes" -> /<bucket>/work%20notes.sqlite3
```

The normalization is the same one used for case-insensitive vault matching and
duplicate detection. Because discovery rejects duplicate normalized names,
every available vault has one distinct deterministic R2 object even when local
database filenames are equal. The database path continues to identify only the
local upload source or download destination.

There is no fallback to the old filename-only object key: choosing such a
fallback during download could restore another vault's data, which is worse
than reporting that the new vault-specific object does not exist.

## Browser API

Vault information is part of the existing bootstrap response so the browser
does not need a separate discovery request. `GET /api/v1/bootstrap` adds the
canonical active name and the alphabetically ordered list:

```json
{
  "vault_name": "Personal",
  "vaults": ["Personal", "Projects"],
  "vault_epoch": 1,
  "initial_forum_id": "entrance",
  "initial_session_id": "welcome",
  "personas": [],
  "characters": [],
  "forums": [],
  "recent_sessions": []
}
```

The remaining bootstrap fields describe only the active vault's workspace and
sessions. The canonical `vault_name` and `vault_epoch` together identify one
vault activation. `vault_epoch` is a process-local, monotonically increasing
number. It starts with the initially published vault and advances exactly once
for every successful vault switch, including a later switch back to a
previously used vault. Configuration edits and database replacement within the
same vault do not advance it.

Every `SessionSnapshot`, including the initial event on an SSE stream, carries
the complete activation identity of the `VaultRuntime` that created its live
session:

```json
{
  "vault_name": "Personal",
  "vault_epoch": 1,
  "session_id": "welcome",
  "...": "existing session snapshot fields"
}
```

The browser stores both values from bootstrap and checks both on every snapshot
before placing it in application state. A different name or epoch means the
page belongs to a previous vault activation. The browser discards that snapshot
and immediately navigates to `/` for a full reload. This check is shared by
normal session open, recovery probes, and SSE snapshot handling; none of those
paths may publish a mismatched snapshot.

The name comparison is required because the counter restarts with the process.
If CHA restarts on another vault, a stale tab's epoch can equal the new
process's initial epoch, but its vault name cannot. The epoch still distinguishes
switching away from and then back to the same named vault within one process.

Each `LiveSession` captures its creating `VaultRuntime`'s canonical name and
epoch and stamps both values into every snapshot it produces. It must not read
the holder's current identity while serializing an event: an old actor finishing
during a switch still belongs to the old activation. Append events need no
separate identity because their stream begins with a checked snapshot and is
forcibly closed during vault replacement.

The browser selects a vault with:

```http
POST /api/v1/vault/switch
Content-Type: application/json

{"vault_name":"Projects"}
```

The requested name is matched case-insensitively against the discovered list.
Selecting the already active vault is an idempotent no-op. A successful request
returns `204 No Content` only after the switch is complete. A request for an
unknown vault, a target that cannot be loaded, or a switch that cannot be
persisted returns an error through the existing JSON error-response mechanism.
If the old runtime cannot be drained within the switch deadline, the endpoint
returns `503 vault_switch_timeout`. The current vault and epoch do not change.
`vault_switching` and `vault_switch_timeout` are added to the protocol's closed
error-code set, OpenAPI schema, and browser runtime allowlist.

Vault switching is an application-level route installed against the stable
`VaultRuntimeHolder`. All other vault routes resolve their dependencies from a
holder lease per request as described in [Runtime ownership](#runtime-ownership).

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
recovery ladder. If a session that the tab had already displayed is absent from
the new vault, `openSession` returns not-found; recovery treats that response as
a stale activation and immediately reloads `/` instead of retrying forever. If
the same session identifiers exist in the new vault, the next snapshot's vault
name and epoch differ from the tab's bootstrap identity and force the same
reload. In particular, a tab cannot silently reattach to the new vault's
`entrance/welcome`, whose identifiers intentionally exist in every vault.

## Runtime switching lifecycle

A switch must not discard the working vault before CHA knows the target vault
can be used. It also must not publish the new vault while an old route can still
start work. The switch has a fallible preparation phase followed by a short,
exclusive commit phase.

### Preparation

1. Resolve the requested name in the startup-time vault registry.
2. If it is the active vault, succeed without changing anything.
3. Open the target database and load its `Workspace` without publishing it.
4. Construct the complete candidate `VaultRuntime`, including its store,
   repository, providers, mirror, and an empty live-session manager. Its
   provider factory references the existing application-wide OAuth owner.
5. Prepare a rewritten `app.toml` in a temporary sibling file, but do not
   replace the durable file yet.

The old vault continues serving normally throughout preparation. Any failure
discards the candidate and temporary file and returns an error without touching
the old runtime or epoch.

### Reusing global maintenance for retirement

Process shutdown is not suitable for the commit. `begin_shutdown()` is
terminal: it permanently stops its `LiveSessionManager`, stops HTTP acceptance,
and allows the process coordinator to call `_Exit(1)` when its grace expires.
The existing `reserve_global_maintenance(deadline)` already performs the
reversible work needed by a switch: it closes live-session admission, snapshots
starting and running actors, wakes waiters, requests
`ShutdownReason::reloading`, and waits for all owners using one absolute
deadline.

Do not add a second near-identical manager operation. Instead, add
`commit_retirement()` to the existing `LiveSessionGlobalMaintenance`
reservation. Normally destroying the reservation releases
`global_maintenance_` and resumes admission, exactly as it does today. Calling
`commit_retirement()` disarms that release and leaves admission permanently
closed for the old manager's remaining lifetime. No second drain algorithm or
second manager-wide maintenance flag is needed.

Unlike process shutdown, expiration is an ordinary recoverable result. It
never stops the HTTP listener and never calls `_Exit`. On expiration the
global-maintenance reservation is released, the old manager remains current,
and the switch request fails. Sessions that already finished remain closed and
can be opened again from their stored records. An unfinished actor remains in
its normal stopping state until it finishes and is swept; it is not destroyed
or joined without a bound.

### Commit

After preparation succeeds:

1. Enter the holder's switching gate. New vault-scoped requests receive
   `503 vault_switching`; application-level assets and the in-flight switch
   request remain available.
2. Acquire a successful global-maintenance reservation from the old
   live-session manager using `WebSettings::shutdown_grace` as the single
   absolute drain deadline.
3. Wait, using the remainder of that same deadline, for existing old-vault
   route leases to finish. The session drain closes SSE streams, allowing their
   leases to leave. A request that acquired an old-runtime lease before the gate
   remains memory-safe, but it is not guaranteed to succeed: while global
   maintenance is reserved, a session lookup returns no actor and the existing
   route reports `409 session_not_live`.
4. Atomically replace `app.toml` with the prepared file. If replacement fails,
   abandon the reservation, reopen old-vault admission, and leave the old
   runtime current.
5. Assign the next `vault_epoch` and publish the prepared `Workspace` and
   `VaultRuntime` while the gate is still closed. All allocations and other
   fallible setup happened during preparation; publication is only a no-fail
   shared-pointer swap.
6. Call `commit_retirement()` on the old manager's global-maintenance
   reservation, reopen the gate on the new runtime, and hand the old runtime to
   the background reaper. The switch request does not run the old runtime's
   potentially unbounded provider destructor.
7. Return `204 No Content` so the initiating browser reloads. This does not wait
   for background destruction of the retired runtime.

If session or route-lease draining reaches the deadline, CHA logs the unfinished
session identities and/or remaining lease count, reopens the holder gate on the
old runtime, leaves `app.toml` and `vault_epoch` unchanged, discards the
candidate, and returns `503 vault_switch_timeout`. It does not publish or
destroy the new or old workspace. A later switch can be attempted after the
stopping actor or request finishes.

Closing and removing old sessions means removing their live runtime objects.
It does not delete stored sessions from the old vault's SQLite database. Those
sessions are available again when that vault is selected later.

All vault-specific resources change together. After the commit, every new API
operation uses the new database, workspace, session repository, `mirror`, and
`modify` settings. This includes configuration import/export and database
upload/download. No operation may continue using paths cached from the startup
vault.

Vault selection is global even if more than one browser happens to be
connected. CHA is a personal application and does not require elaborate
multi-client coordination or consensus. The browser that initiated the switch
reloads immediately. Other browsers detect the activation change through the
snapshot vault name and epoch, or through not-found recovery, and reload before
accepting state from the new vault.

## Why switching stays in process

Updating `app.toml` and restarting CHA is not an implementation alternative for
this feature. The packaged Linux `bin/start-cha.sh` performs a one-shot
background `nohup`; it is not a supervisor and does not restart a process that
exits. Development and macOS launches also commonly request `port = 0`, so a
new runtime receives a different ephemeral port while the browser or WKWebView
still points at the old origin. A process that exits also cannot complete the
switch request with `204`.

The HTTP server and listener must therefore survive the switch. Only the
published `VaultRuntime` and its vault-scoped resources are replaced.

## macOS application

The macOS application uses the same configuration-directory layout and vault
rules as the console application:

- its configuration argument names the directory containing `app.toml` and
  vault definitions;
- it discovers and selects the startup vault in the same way;
- the browser UI switches the single in-process runtime globally;
- its OpenAI login remains application-wide and unchanged across vault
  switches;
- native Import, Export, Upload, and Download operations always act on the
  current vault; and
- Import can create the current vault's database when it does not exist.

The bundled first-run database seed is a special pre-runtime path: it runs
before a `VaultRuntimeHolder` exists, so there is no current runtime to query.
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
| No valid vault definitions are found | Startup fails. |
| Two valid definitions have the same case-insensitive name | Startup fails because selection would be ambiguous. |
| Two valid vaults resolve to the same `data` path | Discovery fails before either database is opened. |
| Vault `mirror` or `modify` trees overlap each other, a database, or the configuration directory | Discovery fails before any destructive operation is available. |
| A non-selected vault definition is invalid | It is logged and omitted from the available list. |
| The startup vault has no valid matching definition | Startup fails. |
| The startup database cannot be loaded during normal startup | Startup fails. |
| A console maintenance command omits `--vault` | The command fails before touching any vault data. |
| A console maintenance command names an unknown or invalid vault | The command fails before touching any vault data. |
| An imported vault database does not yet exist | Import may create it. |
| A runtime switch target cannot be loaded | The old vault remains active; the browser shows the failure. |
| `app.toml` cannot be updated during a switch | The old vault remains active; the browser shows the failure. |
| Old-vault sessions or route leases do not drain before the switch deadline | The switch returns `503 vault_switch_timeout`; the process keeps serving the old vault, the epoch and `app.toml` do not change, and unfinished actors are retained safely. |
| A request arrives while the holder is committing a switch | It receives retryable `503 vault_switching`; it cannot acquire either a partial old runtime or a partial new one. |
| A stale browser receives a snapshot with a different vault name or epoch | It discards the snapshot and reloads `/` before displaying or mutating the new vault. |
| Recovery cannot find a session that the tab previously displayed | The tab treats not-found as a stale vault activation and reloads `/` instead of retrying forever. |
| An old provider transport ignores cancellation after a successful switch | The switch still returns `204`; the background reaper retains the old runtime and waits without blocking the new vault. |
| Two vault databases have the same filename | They remain safe: R2 addresses them by normalized vault name, not database filename. |

## Deliberate non-goals

This design does not introduce:

- more than one simultaneously active vault;
- per-browser or per-user vault selection;
- per-vault OpenAI login or credential files;
- recursive vault discovery;
- live rescanning or filesystem watching;
- UI for creating, deleting, renaming, or editing vault definitions;
- fallback compatibility with the old single configuration file;
- database opening as part of vault-file discovery; or
- stopping and externally restarting the process to perform a vault switch.

These constraints keep vaults as a small composition feature: a fixed registry
of external database configurations, one active runtime, and one explicit
switch operation.
