# Assistant vault download and load

Date: November 12, 2026.
Status: Technical design. Application implementation is not part of this change.

## 1. Required behavior and decisions

The Assistant in Entrance / Welcome must download vaults from R2 and change
the running application's active vault. This must work in ChaWeb without
desktop Settings, a daemon restart, or access to the browser's filesystem.
The desktop Assistant uses the same application implementation.

The user's final command rules supersede the earlier download-only proposal:

| User request | Required action |
| --- | --- |
| `Download vault` | Ask the user to confirm the replacement. After a yes, download a new version of the active vault from R2, replace its local database, and reopen it. |
| `Download an updated version of the current vault and load it` | Same operation as `Download vault`. |
| `Download vault Gladius` | Download Gladius from R2 and make the downloaded vault active. If Gladius is already registered, ask the user to confirm the replacement of its local copy first. |
| `Download and load vault Gladius` | Same operation as `Download vault Gladius`. |
| `Load vault Gladius` | Open the registered local Gladius vault. Do not contact R2. |
| `Load the current vault` | Report that it is already active. Do not download or reopen it. |

There is no download-only command in this design. There is no pending download
to retain between chat turns. Temporary files exist only while an operation
runs. Downloading a different vault does not delete the previously active
vault or copy the downloaded database over that other vault's files.

The first version supports unprotected target vaults. Do not request passwords
in chat, add password arguments to tools, or implement password entry UI. A
protected active vault may be the source of a switch to an unprotected vault;
refreshing a protected target through these tools is unsupported, even if the
application already holds its password. Existing non-Assistant password
workflows are unchanged.

Before the Assistant replaces the database of a registered local vault, it
asks the user in chat to confirm. The question names the vault and states
that the local database, including local notes and folders, is replaced, and
that the previous database replaces the existing `.bac` backup. The Assistant
calls the download tool only after the user's yes in a later turn. A download
of a vault with no local registration, and `Load`, need no confirmation. The
confirmation is an Assistant instruction. Native code does not verify the
reply and keeps no confirmation state between turns. Ask a chat
question if the intended action or target is ambiguous, for example `Update
Gladius` without enough context. Do not infer a download from `Load Gladius`.
If a user explicitly requests download without activation, explain the
supported behavior and ask before doing something different.

## 2. Current implementation

These are observations from the repository, not proposed new components.

| Location | Existing behavior |
| --- | --- |
| `src/app/assistant_service.*` | Validates native Welcome identity, cancellation, and context epoch; exposes configuration, inspection, logs, and events. Tool execution starts under `lifecycle_mutex`. |
| `src/providers/tool_calls.cpp` | Declares maintenance tools for Responses, Chat Completions, Anthropic, and Google through the common schema builder. |
| `src/providers/provider_client.cpp` | Runs tool calls on the provider request worker, then normally makes another model request. Calls in a model response are processed in sequence. |
| `src/app/vault_operations.cpp` | `VaultMaintenance` owns vault registration, R2 download, switch, resource draining, reopen, context publication, and recovery. |
| `Application::download_r2_vault(name, epoch)` | Downloads a new local vault and registers it. Refuses an existing destination. Does not activate it. |
| `Application::download_database(epoch)` | Downloads and replaces the active database and its definition, then reopens the runtime and rebuilds the mirror. |
| `Application::switch_vault(name, password, epoch)` | Opens a registered local vault, retargets runtime storage, updates active identity, and normally persists the startup selection. |
| `src/app/r2_database_transfer.*` | Implements signed R2 requests, object listing, temporary files, database validation, ETags, replacement, and `.bac` backups. |
| `CurrentVault` and `command.vaults` | Already hold active identity and the local registry. `CurrentVault::set` alone does not switch runtime resources. |
| `BackgroundJobs` | Owns application background workers, cancellation, and shutdown joining. |
| `LiveSessionManager::post_session_notice` | Adds a native notice to an existing or temporarily opened session. Unlike `post_maintenance_result`, it does not refresh unrelated sessions as a configuration edit. |
| Native bridge | Already publishes `app.contextChanged` and carries context epochs. |
| `webapp/src/components/App.tsx` | Its context-change handler currently requires `causing_request_id`; an Assistant background action has no bridge request ID. The handler needs an explicit change. |
| ChaWeb adapter | Uses the daemon's current epoch when each HTTP request arrives. Bootstrap returns presentation data and discards `ApplicationBootstrap::context_epoch`. |
| `webapp/src/chaweb/useChaweb.ts` | Polls snapshots and recovers bootstrap after some failures. It does not implement a complete vault-context reset. Same-name refresh cannot be detected by vault name alone. |

R2 contains root-level `<object-stem>.sqlite3` objects and, for current uploads,
`<object-stem>.sqlite3.toml` companions. A local vault's display name need not
equal its database filename. A remote companion can also supply a display
name different from the object stem. Preserve these existing distinctions.

The current R2 picker listing removes databases already stored locally. That
filtered list cannot be the Assistant's complete list of downloadable vaults.

## 3. Architecture and scope

Use two new maintenance tools and one new inspection topic. Put application
work in `VaultMaintenance`; keep language interpretation in the Assistant's
instructions. Do not add a command parser for the example sentences.

```mermaid
sequenceDiagram
    participant U as User / Welcome
    participant P as Provider worker
    participant A as AssistantService
    participant J as Application background job
    participant V as VaultMaintenance
    U->>P: Download vault Gladius
    P->>A: assistant_vault_download(name="Gladius")
    A->>A: Validate identity, arguments, target, epoch
    A->>J: Schedule bounded operation
    A-->>P: Native accepted result
    P-->>U: Native starting notice; end this turn
    J->>V: Download, validate, install, activate
    V->>V: Drain workers before replacing active storage
    V-->>U: Context change / ChaWeb context recovery
    J-->>U: Native completion or failure notice
```

The background job must not wait for a final model answer. The model turn
ends when native code accepts a vault operation. The job can then drain that
worker through the existing maintenance path. Running a switch directly in
the provider worker would wait for the worker that is executing the switch.
Releasing `lifecycle_mutex` alone does not solve that problem.

No new database schema, download cache, job service, operation history table,
scheduled refresh, retry daemon, or upload capability is required. Reuse the
existing registry, transfer code, native session notices, and logs.

## 4. Tool contracts and interpretation

### 4.1 `assistant_vault_download(name)`

Strict schema: one required field, `name`, of type string or null; no other
properties. Null means the active vault captured from the native request
context. A string must be nonempty and pass the existing public-name/path
component validation. Paths, URLs, credentials, SQL, and passwords are not
accepted.

The action always downloads and activates. Resolve the target as follows:

1. Null selects the captured active `VaultDefinition`.
2. A registered local name selects that definition, using `find_vault` and
   `same_vault_name`. Its database filename determines the R2 object. Do not
   synthesize an object filename from its display name. An active target uses
   6.1; an inactive target uses 6.3.
3. Otherwise the name denotes an R2 object stem, as in the existing download
   picker. Native code uses the name as given (`<name>.sqlite3`) and does not
   list R2 to resolve it. Preserve R2 key spelling and case. If the name does
   not match an object, the Assistant can call
   `assistant_inspect(topic="r2_vaults")` and use an exact returned
   `download_name`. It asks the user when more than one key could match.
   Native code does not use fuzzy matching.
4. For a new download, activate the canonical name returned by registration,
   not the input object stem. Report both names if they differ. A companion
   name collision with another registration is an error, not permission to
   overwrite that other vault.

A registered name takes precedence over a remote stem of the same spelling.
This keeps `Download Gladius` tied to the existing Gladius registration. List
results must distinguish `vault_name` from `download_name` so the Assistant
can explain a nonstandard mapping without inventing it.

### 4.2 `assistant_vault_load(name)`

Strict schema: one required nonempty string `name`; no other properties.
Resolve only against the local registry. Unknown local names return
`not_found`; the Assistant may offer download, but must not download without
the user's instruction. Loading an already active vault returns a synchronous
no-op result and does not end the turn through the background handoff.

Reject a protected target before scheduling. Use `switch_vault` with an empty
password for supported targets. A local load must work without R2 credentials
and without network access.

### 4.3 Discovery

Continue to use `assistant_inspect(topic="application", ...)` for local vaults
and active identity. Add `topic="r2_vaults"`; its other ID arguments must be
null. Use the existing `after`/`next_after` inspection pagination convention
and result-size limits. Read the live list on demand; do not cache it.

Remote items contain the exact `download_name` and any known local registry
association based on database filename. Do not fetch every companion just to
list remote display names. Do not return bucket credentials, signed requests,
or arbitrary object contents. Include registered objects in this inspection;
preserve the existing desktop picker's filtering behavior.

### 4.4 Results

An accepted operation returns a small native result, for example:

```json
{
  "accepted": true,
  "message": "Downloading and loading vault Gladius."
}
```

`accepted` means that the application owns the operation. It does not mean
that files have been installed or that the vault has loaded. No model reads
this result, so it has no other fields. Immediate validation errors return
the existing tool-error shape, with `committed=false`, and allow a normal
model explanation.

The provider loop recognizes `accepted=true` only in a native result from
these two exact tool names. Check the result immediately after the tool
returns, before the result-size checks. Then emit its `message` through the
existing answer-delta path and return from `perform` at once, without another
model request or automatic retry. Later calls from the same model response,
including web calls collected for later execution, therefore never run.

No control field from model text, a remote document, or another tool may
trigger this behavior. A small helper to recognize this native result is
enough; a new provider protocol or generation event type is not required.
An operation may finish quickly enough to cancel the source generation
before its starting text becomes visible. Completion must therefore use a
native notice independent of the provider stream.

## 5. Application ownership and concurrency

Add narrow application callbacks to `AssistantService::Links` for remote
inspection and starting a vault action. The service validates Welcome access
and JSON; the application owns target resolution, resource lifetime, file
changes, and scheduling. Do not expose `Impl` or the entire `Application` to
the service.

Use this lock contract:

1. `AssistantService::execute` performs its normal admission check under the
   lifecycle lock. Capture the trusted `MaintenanceContext` and parsed action.
2. Release that lock before calling an application entry point that acquires
   it itself. The entry point repeats admission with the captured epoch.
3. Under that lock, resolve and copy the target identity and reject
   unsupported protection.
4. Start a worker through `BackgroundJobs`. The worker owns copied values,
   not pointers into `command.vaults`, a provider stack frame, or a cancellation
   flag whose lifetime ends with the model request.
5. On scheduling failure, return an immediate error. On success, return the
   accepted result. Do not wait for the worker.
6. The worker acquires application locks through the normal maintenance entry
   points, repeats epoch/identity admission, runs the operation, and publishes
   the outcome.

Add no in-flight reservation, queue, or `busy` result. The lifecycle lock
serializes the jobs, and each job's captured epoch rejects work after an
earlier job changed the context. A second accepted operation therefore runs
after the first one, or fails with `stale_context`. Retain no
completed-operation cache.

Existing UI maintenance remains protected by the lifecycle lock, database
leases, and epochs. If it changes the context between two steps of an
operation, the Assistant operation fails its old-epoch check instead of
targeting the newly active vault. Do not replace a failed check with
`context_epoch()` and retry. A new vault download can remain saved and
registered in this case. An existing inactive target can remain active
without its refresh.

Do not classify these tools as configuration edits that always call
`store->require_main()`. Downloads require the existing main-process rights;
a local switch retains `switch_vault`'s existing secondary-process behavior.

Cancellation before acceptance prevents scheduling. After acceptance, the
operation belongs to the application; provider cancellation caused by vault
maintenance must not cancel the download itself. Use the job's cancellation
flag and the application's stop flag for transfers. Stop-generation is not
an operation undo. Do not add a new cancel-operation UI or tool in this scope.
Shutdown must stop and join the worker through `BackgroundJobs`; never detach
work from its application dependencies.

The job records which steps finished (download saved, vault loaded), the
public error text, and any persistence warning. It learns this from which
calls returned. After its last step, it reads the current epoch once and
posts the notice with that epoch. Do not add an "installed" flag at the
publication boundary, and do not return the epoch from inside
`maintain_database`. The record is used for the native notice and the log,
then discarded.

This has two accepted costs:

- In a refresh, `publish_downloads` changes file permissions after it installs
  the files. If that step, or the definition read after it, throws, the vault
  has the new content but the notice says that the download failed. The
  desktop download has the same behavior today.
- If a UI vault switch completes just before the job reads the epoch, the
  notice goes into that vault's Welcome.

## 6. Download and activation paths

### 6.1 Active target

Reuse `VaultMaintenance::download_database`. Keep its existing sequence:

1. Admit the captured epoch and check main-process ownership.
2. Drain sessions and providers; pause note/media work and D1 synchronization.
3. Close/checkpoint database owners through their maintenance guards.
4. Download the companion and SQLite database to temporary sibling files.
5. Validate the database and matching vault identity, normalize local paths,
   save the downloaded ETag, and clear the downloaded outgoing sync queue.
6. Install the pair with the existing backups and rollback-on-error behavior.
7. Reopen the store and repository, recover sessions, publish the downloaded
   definition and a new context epoch, resume with `SyncStart::recover`, and
   rebuild the mirror.

Check missing R2 credentials and unsupported target protection before draining
when possible. Retain the existing validation inside the operation as well.
Do not hold a second download while waiting for a separate load command.
`download_database` keeps its return type; the job reads the epoch after it
returns (section 5).

### 6.2 New named target

Use `download_r2_vault` for download, validation, destination selection, and
registration. Then call `switch_vault` with the returned canonical name and
the original admitted epoch. Registration does not itself change the active
context.

Activation immediately after an R2 download must ensure that this machine's
D1 queue exists. Change `switch_vault` to start the D1 worker with
`SyncStart::recover` instead of `SyncStart::open` for every switch. Do not add
a switch option. This also covers the desktop "download, then switch" flow.
A switch with D1 configured makes one more D1 request and creates this
machine's queue table if it is missing. D1 failures only log warnings, as
today.

The existing active vault stays open during the transfer. Download failure
must remove files created for the failed new registration and leave the
registry and active vault unchanged. Preserve unrelated files and reject
existing unregistered destination paths, directories, or symlinks.

If registration succeeds but activation fails, keep the valid downloaded
vault registered. Report `Downloaded Gladius, but it could not be loaded`
with the actual error. The user can later issue `Load vault Gladius`. Do not
redownload or delete a successfully saved vault to hide a partial outcome.

### 6.3 Existing inactive named target

Run two existing paths in sequence. Do not add a separate inactive-refresh
transfer path or a candidate validation callback.

1. Call `switch_vault` for the target with the captured epoch, as for a local
   Load (6.4). Reject a protected target before scheduling.
2. Call `download_database` with the epoch that the switch returned. This is
   the active refresh in 6.1. It uses the target's own R2 key, the target's
   database filename as the R2 object, and `SyncStart::recover`.

Each entry point acquires the lifecycle lock itself. The job holds no lock
between the two calls, and it never calls either entry point while it
already holds that lock. A context change between the two steps makes the
download fail with `stale_context`. Do not retry with a newer epoch.

Use the target's own R2 key. The target uploads to the bucket that its own
key names. The previously active vault's key can name a different bucket,
which can contain an older object with the same name.

Outcomes:

- The switch fails: report it as a Load failure (6.4). The previous active
  vault remains selected for failures before retargeting. A failure after
  runtime retargeting can require restart, as it does today. Do not promise
  that all switch failures automatically restore the old runtime.
- The switch succeeds and the download fails, for example because the target
  has no R2 key, the network fails, validation fails, or the context changed:
  the target stays active with its previous local content. Report `Loaded
  vault Gladius, but the download failed` with the actual error. Do not
  switch back.
- Both steps succeed: report that the vault was downloaded and loaded.

The target opens with its previous content before the refresh. Its D1 worker
starts as for an ordinary Load, and the refresh drain attempts its usual
final send.

### 6.4 Local load

Use the existing `switch_vault` path directly from the background job. It
checks the target database before draining, retargets the store and session
repository, applies target credentials/configuration, starts D1 with
`SyncStart::recover` (6.2), rebuilds the mirror, and publishes the new epoch.
Retain its startup-selection persistence rules. Persisting `app.toml` can
fail after a successful live switch: report that the vault is loaded but
startup selection was not saved, and retain the warning log. Do not report
that the live switch failed.

### 6.5 Transfer rules retained from the application

- Existing-vault refresh uses the local database filename as the remote key
  and requires a matching companion. Missing companions do not silently
  replace an existing vault with a legacy database-only upload.
- Each transfer holds the lifecycle lock until it ends, as today. Desktop and
  ChaWeb requests wait during that time. The ChaWeb daemon serves one
  connection at a time, so all ChaWeb requests wait. A transfer longer than
  nginx's `scgi_read_timeout` (75 s) gives 504 errors, and the client uses its
  existing failure and recovery handling. This is accepted. Do not move the
  transfer outside the lock in this scope.
- New-vault download retains the existing legacy database-only fallback.
- Refresh replaces the full SQLite vault, including local notes, folders,
  sessions, and configuration. It is not a merge or a configuration import.
- Existing `.bac` backups for the database and companion are replaced by the
  immediately preceding local files. Do not add backup history or automatic
  rollback commands.
- Preserve validation, private file permissions, ETags, sidecar cleanup, and
  downloaded sync-queue cleanup. Download does not require R2 uploader
  registration and never uploads data to R2.
- Check cancellation after validation and before publication. Once publication
  starts, finish installation or rollback; do not interrupt between paired
  renames merely to honor cancellation.
- The current paired-file replacement is not a crash-atomic filesystem
  transaction. Preserve its exception recovery and backups; do not claim
  automatic recovery from power loss or a failed rollback.

## 7. Completion, errors, and Assistant instructions

Post a concise native notice to Welcome after the outcome is known, using
`post_session_notice` with the epoch that the job read after its last step
(section 5) and the built-in Welcome identity. Do not copy the source vault's
transcript into the destination. Do not start another model request in the
destination to announce success. The new vault may have no usable Assistant
provider.

The notice goes to the vault that is active when the job ends: the resulting
vault after success, the original vault after an early failure, and the
target after a 6.3 partial outcome. For a partial outcome, state both facts:
a new vault download was saved but the load failed (6.2), or an existing
inactive target was loaded but its download failed (6.3). Write a diagnostic
log with the action, target, phase, and outcome; exclude credentials and raw
signed HTTP material.

`post_session_notice` checks the epoch again when it consumes the notice. If
notice persistence fails, log that failure; it does not undo a successful
vault operation. If the runtime is unavailable, use the existing unavailable
state and diagnostics instead of pretending that a Welcome message can be
saved.

| Condition | Required result |
| --- | --- |
| Invalid arguments, paths, or ambiguous target | Reject before mutation; ask for a precise name if needed. |
| Unknown local name for Load | `not_found`; no R2 request. |
| No R2 key / missing remote object / rejected R2 credentials | Explain the download failure; do not change to another vault. In 6.3, the target is already active and stays active (row below). |
| Protected target or encrypted download | Unsupported in this feature; no password request or installation. |
| Existing unregistered file / registry conflict / invalid database | Refuse publication; preserve unrelated local files. |
| A second operation is accepted while one runs | It runs after the first one; `stale_context` if the first one changed the context. No queue or automatic replay. |
| Existing inactive target: switch succeeds, download fails | The target stays loaded with its previous content; report both facts. |
| Source epoch changes before an operation or between phases | `stale_context`; do not retarget. State any already committed download. |
| Session drain or lease acquisition fails | No replacement; report the failure and preserve the existing maintenance recovery behavior. |
| Network, validation, or pre-publication cancellation failure | Remove temporary files; preserve the installed target. |
| File publication or reopen failure | Report the error; preserve backups and expose restart-required when necessary. A failure after installation can report a failed download for installed files (section 5). |
| Startup-selection persistence fails | Loaded successfully for this run, with a persistence warning. |

Update `resources/assistant-maintenance.md` to describe these exact command
rules, the confirmation rule in section 1, and the tools. Replace the blanket
prohibition on vault management with this narrow exception. Keep arbitrary
filesystem, encryption, deletion, rename, merge, upload, and import/export
operations outside the new tools.
Keep the existing warning that manually edited host registrations may differ
from the live registry until restart; these operations use the live registry.

An explicit vault replacement is allowed to bring in the downloaded vault's
Assistant settings. This is a whole-vault operation, not a new permission for
`vault_config_apply` to edit the Assistant's protected configuration. Its
existing write restrictions remain.

## 8. ChaWeb context handling

This is necessary for correctness, not just for displaying a new vault name.
Welcome has the same built-in IDs in different vaults, and same-name refresh
can replace every session and note while retaining the displayed name.

### 8.1 HTTP context contract

Add an opaque browser context token. Construct it from one daemon-instance
identifier and the application's existing context epoch. The instance
component prevents an old browser from treating epoch 1 after a daemon
restart as epoch 1 from the previous process. Keep one instance value for the
daemon lifetime; this is not an authentication token or session store.

- `GET /api/cha/v1/bootstrap` needs no incoming token. On success, add a
  `context` field with the token for the exact `ApplicationBootstrap`
  snapshot to the Bootstrap JSON. Update the client's bootstrap validation.
- Every context-bound HTTP request carries the last successful bootstrap
  token in one query parameter, `context`. Use no header. Parse and validate
  the parameter in the adapter, then pass its epoch to the application API
  instead of sampling a fresh epoch on receipt.
- A well-formed stale token returns HTTP 409 / `vault_changed` before opening
  a session, reading its contents, or applying a mutation. A missing,
  duplicate, or malformed required token returns HTTP 400 /
  `invalid_argument` with an actionable reload message. Do not silently fall
  back to the current epoch.
- Application maintenance/unavailability still uses existing admission and
  errors. A request that races a switch must fail its application admission
  check, even if the token matched when first parsed.
- Client requests capture the token at creation. Late responses belong to
  that captured context and cannot update state after a newer bootstrap.
  Only a successful bootstrap adopts a new token.

Extend `ScgiRequest` and its parser for `QUERY_STRING`. nginx's
`scgi_params` already forwards it, so the nginx configuration does not change.
Do not put credentials or operation commands in the query string.

Apply the guard to chat/list/create/input/stop/delete, notes, voice, and
audio requests that address vault-owned data. This includes the direct
cached-audio URL that the browser plays. The client adds the parameter with
URL encoding, not arbitrary string concatenation. Keep the existing audio
offset header. Direct audio URLs must not become an unguarded way to read
same-ID content after replacement.

Frontend assets and the daemon must be deployed together.
An old frontend must receive a clear reload/configuration error, not operate
against whichever vault happens to be active. No new HTTP vault mutation
endpoint is needed: the Assistant tools run inside the application.

### 8.2 Browser recovery

Handle `vault_changed` before ordinary missing-session or retry handling.
Use one context-recovery function from the existing read/write hooks:

1. Block sends while recovering. Invalidate the existing request generations,
   cancel pending reads, and stop playback/dictation associated with the old
   context. Do not resend an accepted or uncertain user prompt.
2. Reload bootstrap using the existing read retry policy and visibility
   behavior. Maintenance may temporarily return unavailable. Do not create a
   second polling service or new retry schedule.
3. If its context token differs, clear old session snapshots, session lists,
   note state, media state, Welcome-turn bookkeeping, and pending command
   acknowledgements. Reset even when `vault_name` is unchanged. Keep drafts:
   draft keys already contain the vault name. Do not add context scoping to
   drafts or discard them.
4. Open the new vault's Welcome session, update the route and presentation,
   and read its native outcome notice. Do not restore an old note/session ID
   into the new vault simply because that ID also exists there.
5. Reject all late responses captured under the previous token. Resume normal
   polling and sending only with a valid new-context snapshot.

Existing active and idle polling will detect the switch even if it completes
between polls: the next request still has the old context token. Handle the
same recovery from user actions on list/notes screens and from visibility
restoration. Multiple browser tabs recover independently; the active vault
belongs to the daemon, not to one tab.

On desktop, retain `app.contextChanged` and the existing context-reset path,
but remove its requirement for a numeric `causing_request_id` when processing
a genuine new context epoch. Use epoch/state to deduplicate handling; retain
the request ID only for correlating bridge replies. Do not reset drafts in
this shared path: it also runs for upload, download, import, export, forum
delete, merge, and rename, which keep drafts today. Handle unavailable
outcomes too. After recovery, open
Welcome so the native result is visible. Do not fabricate a bridge request ID.

## 9. Visible flow

Use the existing Welcome transcript and existing unavailable/reconnecting
presentation. No new panel, settings control, progress meter, modal, or
password field is proposed. These sketches show the intended chat behavior:

Refresh of the active vault Atlas:

```text
Atlas Welcome, before:
  You: Download vault
  Assistant: This replaces the local database of vault Atlas, including
      local notes and folders. The previous database replaces the existing
      .bac backup. Download?
  You: Yes
  Assistant: Downloading and loading vault Atlas.
    [existing context recovery]
Atlas Welcome, after (the downloaded database):
  ...Welcome history from the downloaded database...
  Notice: Downloaded and loaded vault Atlas.
```

The four lines before the recovery were in the replaced database. They do
not appear after the refresh; they remain only in the `.bac` backup.

Download of Gladius, which has no local registration (no confirmation):

```text
Atlas Welcome:
  You: Download vault Gladius
  Assistant: Downloading and loading vault Gladius.
    [existing context recovery]
Gladius Welcome:
  ...Welcome history from the downloaded Gladius...
  Notice: Downloaded and loaded vault Gladius.
```

Download of Gladius, which is registered and inactive:

```text
Atlas Welcome:
  You: Download vault Gladius
  Assistant: This replaces the local database of vault Gladius, including
      local notes and folders. The previous database replaces the existing
      .bac backup. Download?
  You: Yes
  Assistant: Downloading and loading vault Gladius.
    [existing context recovery, once after the switch, once after the refresh]
Gladius Welcome:
  ...Welcome history from the downloaded Gladius...
  Notice: Downloaded and loaded vault Gladius.
```

Local load:

```text
Atlas Welcome:
  You: Load vault Gladius
  Assistant: Loading local vault Gladius.
    [existing context recovery; no R2 request]
Gladius Welcome:
  ...existing Gladius Welcome history...
  Notice: Loaded vault Gladius.
```

After a switch, the request and the start message stay in the source vault's
Welcome. The new vault's Welcome shows its own history and the notice. Each
notice is application-authored, not a claim generated before completion. Use
the existing notice rendering. If later implementation needs new controls or
a different layout, present that visual change for approval under `AGENTS.md`
before implementing it.

## 10. Implementation map

| Files / area | Planned change |
| --- | --- |
| `src/providers/tool_calls.cpp` | Two strict schemas and the new inspection topic. |
| `src/providers/provider_client.cpp` | Return from `perform` at once after an accepted native vault action. |
| `src/app/assistant_service.h`, `.cpp` | Narrow application callbacks, strict validation, error mapping, discovery dispatch, and lock handoff. |
| `src/app/application.cpp`, `application_internal.h`, `application.h` | Bind callbacks, schedule through `BackgroundJobs`, and record finished steps, error text, and persistence warnings. Keep helpers private unless tests or an existing application boundary need them. |
| `src/app/vault_operations.cpp` | Orchestrate download-and-activate; for an existing inactive target, run switch and then active refresh; reuse existing active refresh and switch; start D1 with `SyncStart::recover` in `switch_vault`; report partial outcomes. |
| `src/app/r2_database_transfer.cpp` | Check cancellation after validation and before publication; preserve existing transfer mechanics. |
| `resources/assistant-maintenance.md` | Exact language rules, confirmation rule, tool descriptions, scope exception, failures, and password restriction. |
| `src/daemon/chaweb_adapter.*`, `scgi.*`, daemon initialization | Instance/epoch context token, captured-epoch admission, bootstrap `context` field, and `QUERY_STRING` parsing for all context-bound requests, including direct audio. |
| `webapp/src/chaweb/client.ts`, `useChaweb.ts`, related note/voice/audio hooks | Token transport as a query parameter, late-response rejection, complete context reset without draft changes, and Welcome recovery. |
| `webapp/src/components/App.tsx` and native context tests | Handle context events without `causing_request_id` and show Welcome after recovery. Do not reset drafts. |
| Existing unit and integration tests | Validate tool routing, actual provider handoff, transfer failure behavior, and both clients' context recovery. |

Do not edit `docs/` as part of this work. Do not change the existing desktop
download-only picker contract to match the new Assistant language contract.
Share native transfer code, not unrelated UI behavior.

## 11. Validation and acceptance

Use existing fake providers, local HTTP R2 fixtures, temporary vaults, and
short test maintenance deadlines. No production bucket or live model is
needed for automated correctness tests.

### Assistant and provider tests

- Schemas are present for each supported provider API and absent from ordinary
  characters, user-defined Assistant forums, and non-Welcome requests.
- Reject extra fields, null Load names, invalid names, path traversal, and
  oversized arguments. Reject stale epochs and cancellation before scheduling.
- Verify the three requested phrases map to the documented tools in a manual
  model smoke test, and that the Assistant asks for confirmation before it
  replaces a registered vault. Automated tests use scripted tool calls, not
  keyword parsing as a substitute for model understanding.
- An accepted tool ends the provider loop; a second mutation or queued web
  call in the same response is never executed. An immediate rejected tool
  can still receive a normal model explanation.
- Exercise a real provider worker calling the real application callback while
  the job drains that worker. Assert completion within the existing deadline,
  no self-wait, and no dependence on a second model response.
- Two accepted operations: the second one runs after the first, or fails
  with `stale_context` when the first one changed the context. Thread-launch
  failure returns an immediate error; shutdown stops and joins the job.
  Accepted work never borrows the worker's cancellation flag or stack storage.

### Vault and R2 tests

- Active A + `Download vault`: A is refreshed, reopened, and still named A;
  new content is visible and the epoch changes.
- Active A + `Download B`, B new: B is registered and active; A's files remain.
- Active A + `Download B`, B local/inactive: B becomes active, then B's files
  and `.bac` backups are updated from the bucket in B's own R2 key; A's files
  remain.
- B local/inactive, switch succeeds, download fails (no R2 key in B, missing
  object, stale context): B stays active with its previous content, and the
  notice states both facts.
- `Load B` uses local content and performs zero R2 requests, even with no R2
  key. `Load A` when A is active is a no-op.
- Local display name differs from database stem; new remote companion name
  differs from stem; case-sensitive remote keys; duplicate/ambiguous names.
- Missing object/companion, bad credentials, invalid SQLite, unsupported
  schema, protected target, encrypted download, invalid parent, registry
  collision, existing unregistered path, and held target lease.
- Download failure leaves the installed target and active vault intact.
  Publication failure exercises backup rollback. Reopen failure exposes the
  existing restart-required state rather than reporting success.
- New registration succeeds but switch fails: saved download remains,
  failure notice is accurate, and later local Load works.
- Context changes between phases: no accidental switch or refresh of the new
  current vault. Cancellation before publication leaves files intact.
- Preserve notes/folders/configuration, clear downloaded outgoing D1 work,
  update ETag, and resume the appropriate synchronization mode. Every switch,
  including an ordinary local Load, creates this machine's missing D1 queue
  before syncing. A Load with D1 unreachable still succeeds, with warnings.
- Successful destination load with an unusable Assistant provider still emits
  a native completion notice. Startup-selection save failure is a warning
  about persistence, not a false live-load failure.

### ChaWeb and native client tests

- SCGI parses/forwards context correctly; duplicate/malformed parameters and
  stale/missing tokens are rejected before any vault-owned operation.
- Bootstrap `context` field and presentation describe one captured context;
  a daemon restart changes token identity even if the numeric epoch repeats.
- Switch A to B where Welcome and ordinary session IDs overlap. An old-tab
  submit/stop/delete cannot affect B. Test direct media URLs as well.
- Refresh A with another version of A. Bootstrap reloads and all old session,
  note, and media state is invalidated despite the unchanged name. Drafts
  stay.
- Fast switch between polls, long maintenance, an old 404, delayed successful
  responses, multiple tabs, hidden/visible transitions, and reconnect during
  the operation do not mix contexts or replay user commands.
- Native bridge publishes a context change for Assistant-originated work and
  displays the resulting Welcome notice without a bridge mutation request.
  A desktop upload or export keeps drafts.
- Verify in a packaged ChaWeb smoke test, not just direct adapter tests, that
  the existing nginx configuration forwards the `context` query parameter.

Run the affected C++ application/provider/daemon tests, then the existing
daemon-labelled CTest suite. Run `npm run check`, `npm run build:chaweb`, and
`npm run build` in `webapp` for an implementation. Finish with the three
requested commands against disposable vaults in ChaWeb and one desktop
Assistant smoke test. Do not declare success from a tool-acceptance result;
verify the loaded content and resulting active identity.

## 12. Implementation order and limits

1. Add the switch-then-refresh sequence, `SyncStart::recover` in
   `switch_vault`, and the cancellation check before publication; verify
   transfer and partial-failure behavior with fixture vaults.
2. Add Assistant schemas, callbacks, background ownership, terminal provider
   handoff, and native outcome notices; test the actual drain interaction.
3. Add ChaWeb context transport and reset behavior, including direct media
   URLs, before enabling the tools in a shipped ChaWeb build.
4. Update the embedded Assistant instructions and run end-to-end acceptance.

Known limits: protected targets are unsupported; R2 download replaces local
changes rather than merging them, after a chat confirmation that native code
does not verify; a valid download can remain saved after a failed activation;
a registered inactive target stays active with its previous content when its
download fails; desktop and ChaWeb requests wait while a transfer runs, and
transfers over 75 s give 504 errors in ChaWeb; a failure after file
installation can be reported as a failed download, and a UI switch at the
moment a job ends can put its notice in that vault's Welcome (section 5);
existing post-publication runtime failures can require restart. The feature
does not add crash recovery or automatic operation replay. It also cannot help
until some vault is open and its Assistant provider can interpret the
initiating request.
