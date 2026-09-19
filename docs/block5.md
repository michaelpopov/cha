# Block 5 — Maintenance, vaults, transfers, and native actions

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** not started. **Environment:** a supported native compiler, macOS, and Windows/WebView2.

## Objective and scope

Implement one application maintenance/context gate and migrate vault, protection, transfer, native-menu, and controlled open/save behavior through it.

Finish maintenance and native actions here. Session export will bind to the save action during session/workspace migration; audio resources and voice cleanup will bind to invalidation during media migration. Do not rebuild those future features or duplicate lifecycle policy in hosts.

## Prerequisites to verify before editing

- Both native hosts pass create/open/submit/stream/Stop/reload/restart and actual
  document/renderer recovery. Request/delivery bounds and asynchronous shutdown
  have working race tests and platform evidence.
- Application owns repository/store/providers/actors and preserves the existing
  vault/transfer code, protected startup, lock order, leases, and persistent format.
- Every native connection and domain request carries checked context identity;
  shared queues/callbacks and typed frontend errors exist.
- Existing native menus/dialogs and HTTP vault/transfer assertions are available
  to migrate. Use configured fixtures for R2 settings until their UI is migrated.

Confirm these in the current checkout and test records. Record actual entry points
and commands if earlier work renamed them. If evidence is unavailable, perform the
relevant check when possible; otherwise keep the prerequisite and gate pending.

## Execution rules and 400K context budget

Plan for **240–320K tokens of working context** within a **400K-token window**,
reserving at least 80K for debugging, final validation, and handoff. These are
planning estimates, not measured guarantees. Search/read only relevant entry points
and direct dependencies; avoid entire generated schemas, build/vendor trees, and
complete test logs. Reuse contracts and fixtures across related operations.

If occupied context approaches 320K before completion, checkpoint a buildable
numbered step and record the exact continuation. Resume this same block in a fresh
context; do not add more blocks or waive the acceptance gate.

CHA is a personal, small application. Prefer the smallest readable change,
ordinary functions, and existing owners. Do not add a general RPC/service/event
framework, durable replay ledger, generic filesystem API, or unnecessary classes.

The migration keeps React presentation and C++ domain state. The final dependency
direction is host → bridge → app → core. The only supported products are
macOS/WKWebView and Windows/WebView2.
Keep provider networking, controller semantics, workspace transactions/publication,
leases, SQLCipher/database format, mirroring, and final persistence unchanged.

- Read applicable `AGENTS.md` and `CLAUDE.md` instructions and inspect the current
  diff before edits. Preserve unrelated changes. Paths below are relative to the
  CHA repository root and may have moved in earlier implementation; use `rg` to
  locate the real current definitions rather than recreating old files.
- Keep the temporary HTTP application working until the final removal stage.
  Select transport explicitly at launch; never fall back for an unsupported native
  method or replay a mutation after timeout. An incomplete native mode is a
  development milestone, not a release substitute.
- Keep one owner thread per session controller and common lifecycle policy.
  UI callbacks must not block on owners, providers, stores, dialogs, or joins.
  Preserve validation, errors, limits, reference checks, and unknown-outcome semantics.
- Move behavioral assertions with code. Keep outbound provider HTTP/SSE, R2, and
  deterministic provider test listeners. They are not the application server.
- Use isolated temporary vaults and dedicated test resources. Live provider/R2
  verification needs explicit test configuration; never overwrite a user's vault
  or ordinary cloud object. Do not log secrets, prompts, or raw sensitive replies.
- Keep UI interaction/copy changes limited to what this migration needs. Do not add
  panel title labels, legends, or explanatory helper copy for obvious controls.
- Ignore unused obsolete configuration with warning logs; do not block an otherwise
  valid operation because an unused setting has a wrong value or type.
- Work in small buildable increments. When migrating related operations before
  transport removal, reuse this extraction pattern: domain function → thin legacy
  adapter → fixed native binding and typed client → schema/real serializer fixture
  → caller and behavior tests.
- A completed prerequisite must be supported by the current code and evidence,
  not merely a status label. Inspect available records or rerun focused checks.
  If a substantive prerequisite is absent, report it and do not recreate earlier
  blocks inside this one. Missing platform evidence remains pending.
- Run checks relevant to actual changes; broaden after new failures, risk, or
  incomplete coverage. A skipped platform check, fake bridge, or successful page
  load is not equivalent to a passing real native user flow.
- Implement this block only. Update its execution record with artifacts and exact
  results. Do not change unrelated documentation, publish/merge a release, or
  proceed into the next block implicitly. Keep a reviewable diff/checkpoint using
  the user's existing workflow; no branch/PR per block is required.

## Code entry points

Start with retained runtime/vault code,
`src/web/{vault_routes,r2_database_transfer}.*`, workspace/repository maintenance
and manager guards, both hosts' menus/dialogs, and runtime/transfer/upgrade tests.

Read the steps and embedded requirements below before changing behavior. Names of
new modules are illustrative; reuse the current implementation and avoid parallel
copies. Braces and `*` in paths denote small related file groups, not literal names.

## Embedded design requirements

The following requirements are included directly so this brief can be executed
alone. They constrain behavior this block implements or touches. They do **not**
expand the scope beyond the numbered steps and acceptance gate; requirements for
later capabilities describe the boundary those capabilities must eventually meet.

### Architecture and Ownership

```text
React components
    |
    +--> ChaClient / presentation actions
    +--> SessionEventsConnector
                |
        Native client and event adapter
                |
        Platform WebView adapter
                |
        Common bridge dispatcher
                |
        C++ Application
                |
        Existing core and providers
```

This is a logical ownership diagram, not a single-process guarantee. A WebView
has browser/renderer processes managed by its platform runtime; its document or
renderer can fail independently of the C++ application. Native IPC removes
localhost networking, not document replacement or renderer failure.

Dependency direction:

```text
platform host -> bridge -> app -> core
```

- Core/domain code must not depend on WebView, bridge, or platform UI APIs.
- Application code must not depend on inbound HTTP request/response types,
  routing, cookies, or a listener. Outbound HTTP/HTTPS through providers and R2
  remains valid.
- Bridge code decodes allowed operations and maps results. Application
  operations enforce product rules and state preconditions.
- Platform hosts own window/WebView creation, UI-thread scheduling, packaged
  assets, document trust checks, permissions, native dialogs, and lifecycle.
  They do not implement session, vault, or provider policy.

Use the existing Swift host and its C/C++ interoperability boundary on macOS.
No host-language rewrite is required. WebKit/WebView2 types stay in platform
adapters, including resource-task and callback types.

Production applications must not start an application TCP listener, expose a
localhost API, serve assets through a local server, use an access cookie, or
create EventSource for CHA updates. Outbound provider traffic is unchanged.
A virtual HTTPS URL resolved inside WebView2 is allowed: the prohibition is on
a listening server, not on the spelling of a resource URL.

### Application Extraction and Frontend Boundaries

Start with a transport-neutral `Application` composition root, the existing
session manager/actors, and maintenance coordination. Group settings, workspace,
credential, and audio operations in ordinary modules. Split a service class
only when ownership or substantial behavior justifies it.

An illustrative small layout is:

```text
src/app/
    application.*
    application_options.h
    application_error.h
    active_session.*
    session_manager.*
    database_maintenance.*
    settings_operations.*
    audio_operations.*
src/bridge/
    bridge_router.*
    bridge_protocol.*
    session_events.*
```

Exact names and file counts are not requirements. Move existing implementation
where possible instead of wrapping it in another forwarding layer.

`Application` owns or coordinates workspace configuration, the repository,
providers, session actors, credentials/OAuth, audio work, vault state, mirroring,
and maintenance. It does not own an HTTP server, listener, port, or access token.
Keep construction and destruction dependencies explicit.

During extraction, HTTP handlers and native menus may call the same application
operations. The desired adapter is:

```text
decode -> application operation -> encode public result
```

Move provider tests, reference/usage checks, editable settings materialization,
workspace publication, and active-session invalidation out of route handlers.
Do not move these rules into the bridge. Input shape checks belong at the wire
boundary; semantic validation belongs in the operation and also protects native
menu callers.

Keep `ChaClient` as the typed frontend operation interface. Session events are
currently a separate injected interface; preserve that seam and supply a native
implementation explicitly. React must not import WebKit/WebView2 globals.
Resource, voice setup, and native save actions must also enter through typed
client/action interfaces instead of new direct platform calls from components.

An internal bridge primitive can remain small:

```ts
interface NativeBridge {
  invoke<T>(method: string, params?: unknown): Promise<T>;
  on<T>(event: string, handler: (payload: T) => void): () => void;
  dispose(): void;
}
```

The generic `T` is an implementation convenience, not runtime validation or a
typed wire contract. Native client methods provide the real operation types.
Disposal rejects locally pending promises and removes listeners; it does not
imply that an already admitted native mutation was cancelled.

Keep the current OpenAPI DTO definitions and type-generation/check scripts
during coexistence. Add native envelope types separately. Before deleting the
HTTP contract, move the retained DTO definitions into a transport-neutral schema
and update generation/check scripts in the same change. Reuse the existing
schema tooling if it can consume those definitions; no new code-generation
framework is required. Maintain one authoritative DTO source and check actual
C++ serialized examples against frontend expectations.

### Runtime State, Vault Context, and Maintenance

The application has explicit running, maintenance, stopping, and unavailable
states. State changes and operation admission are coordinated outside platform
UI code. Native menus use the same gate as bridge calls.

`app.bootstrap` returns a monotonically increasing `context_epoch` identifying
the current usable application/vault context. Every domain request includes the
epoch obtained from bootstrap. The bridge captures that context when admitting
work and the operation checks it again before applying effects. The context check and the
mutation/resource acquisition share exclusion with maintenance through the
existing guards; a check followed by an unprotected use is insufficient. Never resolve
an old queued operation against whichever vault happens to be current later.

A context epoch is a stale-work check, not a credential or a replacement for
origin validation. Do not add persistent identifiers or a registry for it.

Operations that switch/replace the database, change protection, or close handles
and rebuild live state follow this order:

1. Reserve global maintenance and stop admitting conflicting domain work.
2. Invalidate the old context for queued work, subscriptions, resource handles,
   and presentation results. Already executing work must settle/cancel under
   the existing ownership and maintenance rules before retargeting.
3. Stop/release affected session actors; pause/cancel audio and voice setup as
   required. Prevent resource readers from retaining handles being closed.
4. Acquire the workspace-store maintenance lock before repository/database
   guards, preserving the existing lock order and checkpoint/reopen behavior.
5. Perform the operation and reopen/resynchronize repository and workspace state.
6. Publish a new usable context epoch, or mark the application unavailable if
   safe reopen fails. Only then reopen admission.

If work stopped actors or invalidated handles before failing, recovery still
publishes a new epoch after safe reopen, even when the active vault is unchanged.
Read-only operations that leave live state and handles intact need not invalidate
the context.

A stale command fails with `vault_changed`; it must not execute in a different
vault. Stale session events/results and resource URLs are discarded or rejected.
Existing callers using `vault_name` remain compatible during migration, but a
name alone is not sufficient across switch-away/switch-back or database replacement.

The initiating maintenance call receives its own terminal result despite the
context invalidation. That result identifies the resulting context/state and
must not be treated as ordinary old-context data. Also send a bounded
`app.contextChanged` notification for native-menu operations. The frontend accepts this notification for its live connection even when it
announces a newer epoch. It rejects remaining old-context promises (their
mutation outcomes may be unknown), clears context-bound state, stops
voice/playback, bootstraps, and reopens the
selected conversation if it still exists. A full WebView reload is an acceptable
initial implementation. Repeated notifications for the same epoch are harmless.

If safe reopen fails, ordinary domain operations consistently return
`application_unavailable`. Keep the native shell and diagnostics available to
report the error and quit/restart; do not leave apparently usable commands
operating on partially reopened state.

### Session Ownership and Retirement

Preserve the current concurrency invariant:

> One owner thread exclusively owns and mutates each active SessionController.

Retain bounded command admission, provider-event draining, wake notification,
projection, notices, persistence/mirroring transitions, cancellation, and orderly
controller destruction. Session snapshots are copied on that owner thread.
Provider callbacks notify the owner; they do not call WebView APIs.

The manager remains the authority for actor creation, lookup, limits, maintenance,
and joining. Preserve its existing one-way lock relationship with actors.

Distinguish these operations:

| Operation | Meaning |
|---|---|
| `session.open` | Open/select a conversation and ensure its actor exists |
| `session.subscribe` | Attach the selected document's event projection |
| `session.unsubscribe` | Detach presentation only |
| `session.stop` | Request cancellation of generation through the owner |
| `session.close` | Explicitly settle/cancel work and retire the actor; retain stored conversation |
| `session.delete` | Reserve the identity, stop/release its actor, then delete persistent state |

Opening alone does not attach events. Subscription establishment is explicit so
the frontend can install its receiver first.

Retirement policy for the initial single-window application:

- Retain the selected actor across WebView reload or renderer loss.
- When another conversation becomes selected, retire the old actor once idle.
  A generation already running there may finish, then the actor retires unless
  selected again. Existing provider deadlines still bound that work.
- A selected idle actor may remain alive until another selection, explicit
  close, maintenance, or application shutdown.
- Unsubscribe/React effect cleanup does not close an actor or cancel generation.
- Selection and retirement must be coordinated so an actor cannot be retired
  concurrently with being selected again. A rejected open leaves the previous
  selection intact. Preserve frontend guards against stale navigation results.
- Retire eligible idle actors before rejecting an open at the active-session
  limit. Starting/stopping actors continue to count until safely released.
  Repeatedly visiting idle conversations must not exhaust the current limit.
- If genuinely busy actors fill the limit, retain an actionable
  `session_limit_reached` error. Do not silently cancel their work.

Store the current selection in application-owned state so bootstrap can restore
it after document replacement. Reopening a retired actor restores existing
persistent state and identity.

Remove browser takeover, network orphan timers, disconnect grace, SSE drain
waits, and HTTP worker interaction. Preserve bounded delivery independently.
Capture any terminal snapshot while the controller still exists; a pending
snapshot-materialization request must not outlive its owner. No actor waits for
a renderer acknowledgement to persist, finish, or shut down.

### Bridge Requests and Command Completion

Use a fixed allowlist of domain operations. Do not expose arbitrary C++ objects,
function lookup, shell execution, SQL, or generic filesystem paths.

Each document gets a native-created `connection_id`. Native code associates it
with the trusted loaded document; accepting a matching value in JSON alone does
not establish trust. Each request ID is unique within that connection.

Example request:

```json
{
  "connection_id": "view-9",
  "id": 42,
  "context_epoch": 3,
  "method": "session.submit",
  "params": {
    "forum_id": "history",
    "session_id": "session-1",
    "input": { "text": "..." }
  }
}
```

Example completed short command:

```json
{
  "connection_id": "view-9",
  "id": 42,
  "context_epoch": 3,
  "ok": true,
  "result": { "clear_input": true }
}
```

Retain the actual public CommandResult DTO, including draft-clearing and notice
behavior. The example illustrates that result; it is not permission to substitute
a generic `accepted: true` for it.

`session.submit` completes when the owner has executed the short command and
determined its result. It must not complete merely because the queue accepted it,
and must not wait for model generation to finish. Generation and Stop remain
separate application operations. Reply/event arrival order must not be used as
a substitute for the session event ordering contract.

Example failure:

```json
{
  "connection_id": "view-9",
  "id": 42,
  "context_epoch": 3,
  "ok": false,
  "error": {
    "code": "session_not_live",
    "message": "That session is not open."
  }
}
```

Each admitted RPC produces at most one terminal reply for its live document.
Reject duplicate outstanding request IDs. Do not reuse IDs within a connection;
replace the connection before exhausting the JavaScript-safe integer range.
Validate all numeric identifiers/sequences without lossy conversion; wire values
that can exceed JavaScript's safe integer range must use an explicit string form.

Correlation does not provide exactly-once execution. Preserve the existing
`command_timeout` meaning: the result is unknown and the command may still
execute. Document disposal, timeout, or loss of a reply does not roll back a
mutation. Never automatically replay mutations after reload, timeout, or a
transport fallback. Refresh authoritative state before offering a deliberate
user retry. No durable deduplication ledger is needed.

`bridge.info` returns a protocol version, application version, and platform.
`app.bootstrap` returns context and initial UI state. These operations work
without a previously bootstrapped epoch. An incompatible protocol stops domain
calls with a clear reload/rebuild error; it does not negotiate multiple protocol
versions. Matching packaged frontend/native code is the supported deployment.

### Dispatch, Threads, and Shutdown

The common router decodes the envelope, checks connection/context and allowed
method, invokes the application operation, and maps a terminal result. It must
not perform blocking application work in a platform message callback.

Use the smallest execution arrangement that preserves existing ownership:

| Work | Execution and admission |
|---|---|
| WebView APIs, navigation, permission callbacks, dialogs | Platform UI thread; no blocking waits for C++ work |
| Session mutations, snapshots, subscription establishment | Existing session owner queue, with asynchronous completion to the bridge |
| Settings reads/writes and other ordinary operations | Small bounded application work queue; retain store locks and semantic validation |
| Provider tests and authenticated voice setup | Background/native provider work; do not occupy session command execution |
| Database/vault maintenance | Serialized background lifecycle operation and common maintenance gate |
| Outbound generation and audio | Existing provider/audio workers and cancellation mechanisms |
| Native-to-JavaScript delivery | Bounded mailbox drained via the UI thread |

An HTTP adapter may temporarily wait on an application result on its worker
thread. The native path must not fill a shared worker pool with threads waiting
for session replies. Adapt owner completion asynchronously. In particular,
Stop, unsubscribe, disposal, and shutdown must not wait behind slow provider
tests, dialogs, audio synthesis, or database transfers in a general FIFO.

Bound outstanding bridge calls and ordinary queued work with small internal
limits; reuse existing limits where appropriate rather than adding configuration.
Keep control admission separate from bulk work so saturated ordinary calls
cannot prevent cancellation/disposal. A full queue returns a stable public error.
An admitted call holds its reply-capacity slot until consumption or connection
invalidation, not merely until its worker finishes. If a sender exceeds the
native in-flight limit, invalidate that connection with bounded error reporting
instead of allocating an unbounded queue of rejection replies.
During maintenance, reject conflicting commands promptly instead of accumulating
work to run against a later context.

An event sink or completion must carry a checked connection lifetime, not an
unguarded raw pointer to a platform view. Queued callbacks may run after
invalidation, but then do nothing. Before delivering, recheck the live document
and context. Destroying a view does not destroy the C++ Application.

Shutdown is asynchronous from the native UI's perspective:

1. Publish stopping; reject new bridge and native-menu domain operations.
2. Invalidate document deliveries/subscriptions and cancel pending native dialogs.
   Settle local promises if the document still exists, without requiring receipt.
3. Cancel/settle admitted application tasks, maintenance, provider tests, voice
   setup, generation, and audio according to their existing cancellation policy.
   Wake abandoned waiters; do not hold a lifecycle mutex while joining work that
   needs it.
4. Let owners finish their required persistence/mirroring and destroy their
   controllers; join actors and other application jobs. Final persistence occurs
   after the relevant state has settled, not as a one-time flush before it changes.
5. Close repository/store/database resources only after their users have stopped.
   Preserve provider completion/supervisor dependencies during cancellation and
   joining, then shut down the provider supervisor.
6. Destroy Application and platform adapters, keeping logging available through
   teardown. Destroy the view/window according to native quit behavior.

Use one bounded shutdown grace rather than restarting a full deadline for every
actor. Preserve the existing controlled process-failure policy if an owner or
worker cannot safely finish. Never detach a live worker and then destroy objects
it can access. No shutdown step depends on the renderer draining its event queue.

### Document Lifetime and Rehydration

Install the trusted bridge receiver before application JavaScript starts. Bind
its native connection lifetime to a specific loaded main-frame document.

On navigation, reload, renderer failure, or view disposal:

- Stop admitting requests from the old document.
- Invalidate its connection before any new document can use the bridge.
- Reject pending JavaScript promises when possible and remove local listeners.
- Detach native subscriptions and release pending presentation batches/resources.
- Cancel document-owned voice setup and speech preview work. Already executed
  mutations and application-owned generation/cache jobs retain their own policy.
- Ignore late replies, resource completions, and callbacks from that connection.

Request IDs can start again only in the new connection. A late reply to old
request 42 must never resolve new request 42.

Recovery:

```text
new trusted document/receiver
    -> bridge.info
    -> app.bootstrap
    -> restore application-selected session if still available
    -> session.open
    -> session.subscribe with a new ID
    -> initial snapshot
```

Preserve useful frontend guards against old navigation results and React
StrictMode cleanup. Remove EventSource status, SSE supersession, stream-slot
waiting, server probes, and network reconnect delay ladders. A native renderer
failure is handled by the host's reload/recreation path; repeated failure becomes
an actionable reload/restart state instead of an endless loop.

Use platform lifecycle notifications, including WKWebView web-content process
termination and WebView2 ProcessFailed. Do not assume the main application dies
when its renderer does. See the
[WebView2 process model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model)
and Apple's
[web-content termination callback](https://developer.apple.com/documentation/webkit/wknavigationdelegate/webviewwebcontentprocessdidterminate%28_%3A%29).

### File Operations, Native Menus, and Operation Coverage

Where the current operation uses a configured import/export directory, preserve
that behavior. Where a user-selected source or destination is required, the host
opens a native dialog on the UI thread and gives a controlled native handle/path
to the application operation. Do not return arbitrary filesystem access to
JavaScript or change existing menu semantics merely to unify APIs.

Dialog cancellation completes as cancellation without side effects. No worker
holds a database lock while waiting for user input. After a dialog returns,
revalidate document/context before starting work. Save failures must not destroy
an existing destination; retain the current temporary-file/replace behavior and
cleanup. Shutdown cancels pending dialogs without blocking the UI thread.

Native menu actions call the same application operations and maintenance gate
as the bridge. Their enabled state follows application capabilities and lifecycle.
Do not put a second copy of vault/password/transfer policy in each host.

The operation inventory must cover at least these groups:

| Group | Proposed operations |
|---|---|
| Application | `bridge.info`, `app.bootstrap` |
| Sessions | `session.list/create/rename/delete/open/close/snapshot/submit/stop`, `session.cover/uncover/deleteTurn/setDefaultCharacter/export`, `session.subscribe/unsubscribe` |
| Characters | `character.get/create/update/updateDefinition/delete`, `character.file.get/create/update/delete` |
| Personas | `persona.get/create/update/delete` |
| Forums | `forum.get/create/update/delete`, `forum.members.update`, `forum.file.get/create/update/delete` |
| Providers | `provider.list/get/create/update/delete/test` |
| OAuth | `openaiAuth.get/start/poll/disconnect` |
| Styles/voices | `style.list/create/update/delete`, `voice.list/create/update/delete` |
| Voice input | `voiceInput.get/save/runtime/connect/cancel` |
| Voice output | `voiceOutput.get/save/runtime`, `speech.start/cancel/release` |
| Entry audio | `audio.start/startBatch/status/source/clearCache/release` |
| Keys | `apiKey.list/create/rename/replaceValue/delete` |
| R2 settings | `r2Storage.get/save/delete` |
| Vaults | `vault.list/create/update/delete/switch/merge`, `vault.r2.list/download` |
| Transfers | `configuration.import/export`, `database.upload/download` |

Slash-separated names above are shorthand for individual fixed operations, not
a dynamic namespace or generic CRUD framework. Final spellings are settled in
the inventory. Native-only operations need not gain a React button or wire
binding when no frontend caller needs one.

Keep OAuth state and tokens native, preserving the current authorization flow.
Open approved authorization links in the system browser; do not expose the CHA
bridge to an embedded login page. Display user-facing verification details when
the existing flow requires them, not stored provider credentials.

### Security and Credential Boundaries

JavaScript input remains untrusted even when frontend/native code ship together.
Keep domain-scoped operations; never expose `executeShell`, `rawSql`,
`readAnyFile`, `writeAnyFile`, pointers, or generic native object access.

#### Document and message trust

- Install the bridge only for the packaged application document or explicitly
  enabled exact development origin.
- In each message handler, validate the actual sender, main frame, expected
  document connection, and canonical origin/asset scope. Do not rely on a
  sender-supplied URL, a JSON connection ID, or origin string prefixes.
- Block unauthorized top-level and frame navigation, redirects, and popup
  creation. External approved links open through the existing system-browser
  policy. Remote pages and user-controlled HTML never inherit bridge privileges.
- Recheck connection/document identity before sending replies, events, or private
  resources. Navigation cancellation alone does not validate queued callbacks.
- Keep resource IDs and protocol version separate from authorization.
- Do not add localhost cookies, random access tokens, or Host-header checks to
  the native interface.

On Windows use JSON messages constructed with the JSON library. On macOS use a
fixed receiver body with values supplied as arguments, or equivalently safe
serialization; never interpolate prompts, responses, filenames, or error text
into executable script.

Relevant platform guidance:
[WebView2 security](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security)
and [WebKit JavaScript arguments](https://developer.apple.com/documentation/webkit/wkwebview/callasyncjavascript%28_%3Aarguments%3Ain%3Acontentworld%3A%29?changes=_7_2_7_5).

#### CSP and resources

CSP currently comes from the HTTP asset response; copying the existing HTML to
a bundle does not retain it. Establish the replacement CSP enforcement in the
chosen loader before removing that response header.

Keep a restrictive script/style/resource policy. Permit only the selected local
asset/media sources and actual required external connections. Move the voice
setup request native so it no longer requires a JavaScript credential-bearing
fetch. Permit the exact local audio resource source in `connect-src` and Blob
playback in `media-src`; verify these with the selected loader rather than
broadening the policy to arbitrary schemes. Any development HMR allowances
belong only to development mode.

The platform spike must record how CSP is applied before application scripts
run and verify it in the packaged hosts. If a meta policy is used, account for
directives it cannot enforce and implement those restrictions in the host.
Preserve navigation/frame isolation independently. Do not allow remote scripts,
unsafe evaluation, or broad filesystem access just to make an asset loader work.

Test policy behavior, not only the presence of a header or tag. Rewrite useful
existing asset/injection tests for the native loader instead of deleting them
with the HTTP-header assertions.

#### Secrets

Stored API keys, R2 secrets, vault passwords retained for native operations, and
OAuth tokens remain native. Runtime DTOs expose IDs, names, presence, and
non-secret settings. Secret creation/replacement is an explicit write-only flow;
clear UI buffers promptly and never return the stored value in normal reads.

Do not log bridge parameters, raw replies, prompts, responses, credentials,
session descriptions, or sensitive configuration. Resource caches must not
persist plaintext from protected vaults through the browser after release.

### Validation, Errors, Timeouts, and Diagnostics

Keep stable application errors and public messages independent of HTTP status.
Preserve distinctions the UI uses, including:

```text
invalid_argument / existing mapped input errors
not_found
prompt_too_large
session_not_live
session_stopping
session_limit_reached
session_open_timeout
command_queue_full
command_timeout
operation_cancelled
vault_changed
vault_password_required
source_vault_password_required
speech_busy
application_unavailable
internal_error
```

The inventory defines any deliberate old-to-new code mapping and updates callers
and tests together. Do not collapse password prompts, unavailable state, and an
unknown mutation outcome into one generic failure. Internal exception details
go to logs; only approved public messages reach React.

Bridge shape validation covers envelope/method, required/unknown fields according
to the DTO, types, safe numeric ranges, enums, identifiers, filenames, and encoded
message/text size. Keep existing product limits, including prompt and editable
file sizes. Bound input before expensive parsing/copying where the platform
allows it. Do not discard size safeguards just because HTTP body framing goes.

Application operations independently validate permissions/capabilities,
references, provider configuration, state preconditions, and context before
side effects. Native-menu callers must not bypass those rules.

Keep targeted runtime checks for critical envelopes, snapshots/appends, and
foreign/persisted data. TypeScript generics and a version match do not validate
C++ serialization. Native client tests using a fake bridge are useful but must
be complemented by shared wire examples produced by the real C++ serializer.

Keep provider total/idle timeouts, bounded owner-command waits, applicable
maintenance deadlines, and shutdown grace. Remove HTTP read/write deadlines,
SSE heartbeat/drain timers, and network reconnect/orphan grace.

Distinguish timeout/caller abandonment from operation cancellation. Speech and
voice setup use explicit cancellation; generation uses `session.stop`; a
maintenance operation follows its safe commit/reopen boundary. Do not promise
rollback just because a UI stopped waiting.

Log lightweight method/result/duration and lifecycle diagnostics. Remove routine
listener, HTTP status, heartbeat, and reconnect logging. Avoid per-token info
logs; retain provider diagnostics under their current policy and measure event
delivery with aggregate/debug instrumentation when needed.

### Startup, Bootstrap, and Configuration Compatibility

Preserve the native pre-WebView startup sequence for protected vaults:
load usable configuration, determine whether a password is needed, prompt
natively, open Application, and then initialize the UI. Cancellation or a wrong
password must not require exposing an unusable database through the bridge.

`app.bootstrap` returns only initial presentation state:

- Application state and `context_epoch`.
- Active vault and vault summaries.
- Selected/initial conversation, including a safe fallback if it was deleted.
- Forums, character/persona summaries, and recent sessions needed for navigation.
- Relevant non-secret runtime settings and capabilities.
- Version information needed for diagnostics.

Details remain specific operations, not an unbounded dump of repositories or
credentials. Bootstrap after reload reuses live application state; it must not
reset generation or replace a usable actor.
During maintenance or an unavailable state, bootstrap reports that state without
reading closed database handles or advertising a usable domain context. Resume
from the context-change notification or a deliberate reload, not a polling ladder.

Remove server settings from newly generated configuration, examples, options,
and UI. Existing files remain valid:

- `[web]` is optional and unused in the standalone application.
- If it is present, ignore the entire obsolete section and log a warning.
  Do not require host/port or validate their obsolete types/ranges.
- Apply the same treatment to retired server-only settings. A syntactically
  valid configuration must not prevent a usable operation solely because it
  contains unused or obsolete values.
- Continue validating settings actually used by the requested operation.
  Malformed TOML or invalid active vault/provider settings still need actionable
  errors.
- Avoid rewriting unrelated user settings merely to remove obsolete keys.

Preserve vault/database configuration, logging, genuine input/session limits,
and provider settings. During HTTP coexistence, validate server settings only
when explicitly running the temporary server target; a native launch must not
depend on them.

Add upgrade cases for missing `[web]`, existing valid `[web]`, and obsolete
values that would previously have failed server validation. Verify database and
protected-vault compatibility independently from configuration cleanup.

## Implementation steps

1. Establish running/maintenance/stopping/unavailable states and one gate shared
   by bridge calls, native menus, and temporary HTTP operations. Do not duplicate
   lifecycle policy in each host.
2. Bind admitted work to its context epoch. Share exclusion between final context
   validation and mutation/resource acquisition; checking then using an unguarded
   pointer is insufficient. Reject conflicting new work instead of deferring it
   to whichever vault is current later.
3. Reserve maintenance and invalidate queued old-context work, subscriptions,
   resource access, and presentation. Settle/cancel executing users before closing
   handles. Preserve store-before-repository lock order, owner reservations,
   checkpoint, close/reopen, workspace publication, and error precedence.
4. Publish a fresh epoch after safe reopen, even for recovery in the same vault.
   If reopening fails, enter unavailable while bootstrap/status/quit work without
   touching closed handles. Keep existing audio pause/cancel hooks and provide
   the narrow invalidation hook the media implementation stage will use.
5. Complete the initiating maintenance call despite its old-context invalidation,
   reporting the resulting epoch/state. Coalesce context-change notifications
   for menu actions. Frontend rejects stale promises, clears scoped state, stops
   playback/capture when present, then bootstraps/reopens; a full reload is fine.
6. Migrate vault list/create/update/delete/switch/merge and protection/password
   operations. Preserve source-vault errors, registry/path persistence, mirroring,
   and session restoration. Do not change the database format.
7. Migrate R2 vault listing/download, database upload/download, and configuration
   import/export through existing native implementations. Preserve configured
   directories, transfer deadlines, safe replacement/cleanup, and reopen behavior.
   R2 settings UI migrates in the provider/settings operation stage; use configured test fixtures here.
8. Connect native menu capability/enabled state to Application. Run blocking
   operations asynchronously, with the same validation and maintenance gate as
   React. No second host-specific password/transfer policy.
9. Implement controlled native open/save actions where user selection is required.
   Open dialogs on the UI thread, revalidate context after selection, cancel
   without effects, and preserve temporary-file/atomic-replace behavior. Do not
   expose arbitrary JS filesystem paths or hold database locks during dialogs.
10. Account for existing file inputs/downloads. Preserve small bounded text uploads
    and verify the WK open-panel delegate and Windows picker. Provide the native
    save action that session export will use in the session/workspace operation stage; large exports must not
    become base64 RPC. Audio resource/read behavior follows in the media implementation stage.
11. Test queued session/settings work across switch-away/back and overlapping IDs,
    failure before/after handle closure, failed reopen, self-completion of the
    initiating call, menu/UI agreement, and quit during maintenance/dialogs.
    Add automated vault switching and persisted-state restoration in both hosts.
12. Run protected-startup, wrong-password/cancel, real native-dialog, transfer,
    and upgrade checks with temporary data; restrict live R2 verification to
    dedicated test objects. Record pending dependent UI bindings explicitly.

## Verification for this block

- Test queued CRUD/session work across vault switch-away/back with overlapping
  IDs; final context validation and effect/resource acquisition must share
  exclusion, so stale work never affects a different database.
- Inject failures before closure, after closure, during commit/reopen, and during
  recovery. Verify a fresh epoch after safe reopen, consistent unavailable state
  on failed reopen, and bootstrap/quit without closed-handle access.
- Verify the initiating maintenance call receives its own terminal outcome and
  menus/frontends observe the same state; coalesced context changes clear stale
  state and restore a usable selected session.
- Automate native vault switching and restart persistence on both hosts; exercise
  protected startup, wrong source/target passwords, creation/rename/delete/merge,
  configuration import/export, and database/R2 transfer behavior.
- Test real open/save cancellation and context revalidation after a dialog,
  atomic save failure, native menu capability changes, and quit with maintenance
  or a dialog outstanding. Use dedicated cloud test objects for live R2 checks.
- Verify the reusable native save action without requiring the later session-
  export or audio implementation. Record dependent caller bindings as future scope.

### Commands and platform evidence

Run from the repository root with the configured supported native toolchain.
These commands exist at the start of the migration; follow recorded target/script
renames in the current checkout. Do not invent test target names or resurrect a
retired target merely to run an old command.

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -N
ctest --test-dir build/ninja --output-on-failure
npm --prefix webapp run check
npm --prefix webapp run build
```

Use `ctest -R` with discovered names for focused iteration. Existing targets include
`cha_tests`, `cha_web_tests`, `cha_web_stress_tests`, and conditionally
`cha_web_process_tests`; app/bridge replacements may already exist. Use the pinned
Node/npm versions in `webapp/package.json` and `webapp/.node-version`; install locked
dependencies with `npm ci` from `webapp` when needed.

For ownership/concurrency changes use the `asan-ubsan` and, on supported toolchains,
`tsan` configure/build presets with focused tests. Record toolchain limitations;
do not add broad suppressions or weaken assertions to turn a failure green.

`npm --prefix webapp run e2e` originally starts `chaweb`. Use it only for retained
HTTP baseline/regression coverage while applicable; it is never native proof.
Locate or create the real host runner required by this task and record its exact
command and OS/WebView version. A native test must exercise actual bridge/app code.

## Acceptance gate

The shared maintenance/context gate and vault/transfer/menu
operations pass common and real-host checks. No stale work acts on a retargeted
or closed database. Native file actions are available for subsequent callers;
remaining session/media bindings are explicitly assigned to the session/workspace and media implementation stages.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide lock/admission protocol, operation coverage, dialog/transfer evidence,
failure-injection tests, context handling, and reusable native save API.

Include the implementation and retained/new tests in the working diff. Record actual
interfaces, lifetime rules, commands, and compatibility leftovers so later work
can use the result without assumptions about filenames or implicit conversation state.

## Execution record and final response

This record is intentionally uncompleted. Fill it with actual evidence as work
proceeds, or provide the same fields in the task handoff if editing this record
is outside the active user's document scope. Prior evidence may be reused only
when its revision/coverage remains applicable. Do not require the next agent to
read an entire conversation to recover decisions.

```text
Status: not started | in progress | waiting for evidence | complete
Starting and resulting revision/checkpoint:
Files changed/moved and actual new APIs/targets:
Implemented behavior and key ownership/contract decisions:
Prerequisites verified and evidence used:
Temporary compatibility code and when it can be removed:
Exact commands, working directories, platform/runtime versions, and results:
Known failures, checks not run, and exact missing evidence:
Inventory/coverage changes and remaining work:
Next unfinished numbered step if this block needs continuation:
```

Maintain concise rows for the operations/files/assertions touched by this block.
For the inventory block, enumerate the complete migration; for later blocks,
carry forward existing evidence and record the relevant updates.

| Operation/caller or source/test path | Retained behavior/result/errors | Native destination or deletion reason | Context/cancellation/lifetime | Verification and status |
|---|---|---|---|---|
| Populate during execution | | | | |

| Required flow/assertion | Common test evidence | macOS evidence | Windows evidence | Remaining limitation |
|---|---|---|---|---|
| Populate during execution | | | | |

The final response must state what was implemented, why, what was actually tested,
and any unresolved limitation. If incomplete, give the exact next step and missing
prerequisite/evidence. A context limit or a mostly working platform is not success.

## Optional provenance

The [migration plan](plan.md) and [design proposal](redesign.md) explain the overall
sequence and original rationale. They are reference material, not additional
required instructions for executing this brief.
