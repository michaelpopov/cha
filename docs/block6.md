# Block 6 — Complete session and workspace operations

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** not started. **Environment:** a supported native compiler, Node/npm, and both native host runners.

## Objective and scope

Migrate all remaining session, character, persona, forum, membership, and bounded text-file operations, including native session export, while preserving workspace transactions and live-session effects.

Use the established bridge, owners, maintenance gate, and native file actions. Do not migrate provider/settings or audio/voice execution here, and do not delete legacy transport. Avoid changing UI interaction design or domain semantics during extraction.

## Prerequisites to verify before editing

- Both hosts already run the initial chat flow with native client/event adapters,
  scoped ordered output, actor retirement, document recovery, and safe shutdown.
- A tested maintenance/context gate protects queued and executing work across
  vault/database replacement; native menus and bridge calls share it.
- Vault/transfer operations and controlled native open/save actions exist,
  including cancellation, post-dialog context checks, and atomic replacement.
- Shared DTO generation/real serializer examples, operation inventory, and
  native runners exist. Existing WorkspaceConfigStore transactions and publication
  APIs remain authoritative and are available for extraction from route handlers.

Confirm these in the current checkout and test records. Record actual entry points
and commands if earlier work renamed them. If evidence is unavailable, perform the
relevant check when possible; otherwise keep the prerequisite and gate pending.

## Execution rules and 400K context budget

Plan for **220–300K tokens of working context** within a **400K-token window**,
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

Start with `src/web/{lobby_routes,
session_routes,text_input,text_command,text_mention,text_multicast,
session_markdown}.*`, `src/workspace` editing APIs, session/character/persona/forum
client methods and UI, and associated domain/component/E2E tests.

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

### Ordered Session Events

This section defines the native wire contract. The current SSE sequence resets
after each snapshot. The native protocol deliberately uses one sequence across
both snapshots and appends within a subscription, avoiding an additional
snapshot-base identifier. It does not change stored session semantics.

A subscription is scoped by:

```text
connection_id + context_epoch + subscription_id + full session identity
```

The frontend allocates a new subscription ID before invoking
`session.subscribe` and installs its local handler first. IDs are never reused
within that document. Only the latest requested subscription can become active;
a delayed older subscribe must not replace a newer request. The bridge tracks
that selection before owner work is enqueued. The initial application supports
one active session subscription per document, not an arbitrary subscriber graph.

On the owner thread, subscribing atomically installs the output sink and
establishes its initial snapshot. The initial snapshot is the first event,
with sequence zero. The subscribe RPC confirms installation; receipt of the
initial snapshot is what makes the frontend projection ready. These may arrive
in either order, so the frontend must already be listening.

Initial event shape (the payload placeholder denotes an object):

```text
{
  "connection_id": "view-9",
  "context_epoch": 3,
  "subscription_id": "sub-5",
  "event": "session.snapshot",
  "forum_id": "history",
  "session_id": "session-1",
  "seq": 0,
  "payload": <SessionSnapshot DTO>
}
```

A subsequent append uses the same envelope:

```json
{
  "connection_id": "view-9",
  "context_epoch": 3,
  "subscription_id": "sub-5",
  "event": "session.append",
  "forum_id": "history",
  "session_id": "session-1",
  "seq": 1,
  "payload": {
    "target": { "kind": "entry", "entry_id": 123 },
    "text": "..."
  }
}
```

Reasoning targets retain the existing request identity. Structural changes,
notices, generation completion/cancellation, and lifecycle changes use full
snapshots unless the current controller proves a safe incremental representation.

Rules:

1. One ordered producer/delivery path carries both snapshots and appends.
   A snapshot reflects all preceding owner changes; later appends are relative
   to that state. Do not send a separately computed snapshot through another
   channel and splice it into a live event projection.
2. Assign sequence numbers when payloads are committed to the ordered outgoing
   stream, after pending coalescing/replacement. Every emitted snapshot or append
   consumes one number. A later snapshot does not reset it.
3. The frontend first checks all scope identifiers. It discards stale deliveries
   and sequence numbers already applied. A valid newer snapshot replaces state
   and establishes its sequence as the baseline, even if it supersedes a gap.
4. Apply an append only after a snapshot and only at the next expected sequence.
   An unknown target, malformed payload, or a gap invalidates the projection.
5. Once invalidated, ignore all further events from that subscription, including
   snapshots. Recovery removes its local handler,
   and subscribes again using a new ID. Only the replacement subscription's
   initial snapshot resumes updates. Do not loop through network retry delays.
6. `session.snapshot` remains a standalone owner-thread read for read-only
   callers and pre-subscription inspection. Its RPC result is not an incremental
   stream repair mechanism. A live repair always replaces the subscription.
7. Unsubscribe names its subscription. A late unsubscribe cannot detach a newer
   one. Frontend cleanup is immediate even if the native acknowledgement is late.
8. Events and replies from an old actor/subscription cannot be applied to a
   reopened actor with the same forum/session ID.

Reuse the existing snapshot DTO, controller-proven text append logic, and
projection tests. Update sequence tests explicitly for the native wire semantics;
do not mechanically reuse the old SSE sequence reset behavior.

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

1. Complete session list/create/rename/delete/open/close and standalone snapshot
   reads. Preserve labels, Recent ordering, Welcome behavior, canonical IDs, and
   running state. Reserve deleted identities and release journal leases before
   removing stored data.
2. Migrate cover/uncover, delete-turn, default-character, and all submit/input
   commands. Preserve mentions, multicast, persona attribution, reasoning,
   notices, draft-clearing, and provider/cancellation transitions. Use full
   snapshots for structural changes that lack a safe append representation.
3. Keep sidebar/catalog/selection truthful after rename/delete and rapid navigation;
   late results cannot restore a deleted/unselected session. Extract session-export
   content and wire it to the maintenance/vault/native-action implementation's save action, retaining native dialog/error
   behavior without exposing arbitrary paths.
4. Migrate character/persona get/create/update/delete and character-definition
   edits. Preserve generated IDs, reserved/built-in/read-only rules, canonical
   settings, descriptions, and reference/use restrictions.
5. Migrate character Markdown file CRUD and persona Markdown replacement. Retain
   traversal/filename checks, allowed files, encoded/text-size limits, and public
   validation errors. Keep appropriate small text content in bounded DTOs.
6. Migrate forum get/create/update/delete, forum-file CRUD, and membership/defaults
   edits. Preserve reference validation, overrides/order, built-in constraints,
   and selected/recent-session behavior when definitions change.
7. Use WorkspaceConfigStore's existing materialize/validate/transaction/publish
   path for every edit. Never mutate the imported source directory or publish
   partially validated state. Preserve affected-session invalidation, including
   actors still opening, through application operations rather than route helpers.
8. Bind typed clients/forms/file actions and canonical results to immediate roster,
   sidebar, and detail updates. Use the native picker/save path already tested;
   do not add a generic filesystem API or duplicate HTTP/native business rules.
9. Move validation/storage/rollback/reference/controller assertions to application
   tests and retain focused wire/client/UI checks. Automate native create/reload,
   rename/delete, rapid Recent navigation, streamed commands, workspace/member/file
   edits, failed validation, and restart persistence. Preserve useful assertions
   from old takeover/reconnect tests in the new native lifetime tests.

## Verification for this block

- Assert session rename updates every visible catalog, active/closed deletion
  releases leases and handles selection correctly, failed/rapid navigation keeps
  the last valid selection, and restart restores canonical conversation state.
- Preserve submit command behavior for persona attribution, mentions, multicast,
  reasoning, cover/uncover, delete-turn, default character, notices, draft clearing,
  and Stop. Compare live projection and authoritative state after structural edits.
- Exercise character/persona/forum/member/file CRUD, generated/reserved IDs,
  read-only built-ins, invalid references, filenames/traversal, encoded/text limits,
  canonical results, and public errors.
- Verify failed workspace validation rolls back SQLite and publication atomically;
  edits invalidate affected active and starting sessions and never mutate the
  imported source directory as live state.
- Run native form/file/edit/export workflows on both host runners, including
  immediate roster/detail updates, picker/save cancellation and failure, reload,
  and persisted changes. Keep focused common serializer/client/component checks.
- Confirm every operation in this block is accounted for in the inventory and
  no retained semantic behavior exists solely inside an HTTP handler.

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

All session, character, persona, forum, membership, and text-file
operations work natively, including export. Constraints, transaction rollback,
active-session effects, immediate UI updates, and persistence retain coverage;
no domain behavior remains solely in these HTTP handlers.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide completed operation/assertion rows, tests, native-flow evidence,
and temporary route/recovery code ready for deletion in the final server-removal stage.

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
