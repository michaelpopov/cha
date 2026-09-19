# Block 2 — Extract Application and the session engine

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** not started. **Environment:** a supported native compiler and Node/npm; platform feasibility must already be established.

## Objective and scope

Extract a transport-neutral Application, shared DTO/options, asynchronous session owners, explicit retirement, and bounded ordered session output while keeping the temporary HTTP application functional.

Implement common application/session behavior and headless tests. Do not implement the platform bridge, migrate every CRUD route, or expose new native maintenance operations yet. Native bindings and full maintenance admission are later work.

## Prerequisites to verify before editing

- Both platforms have demonstrated local packaged assets, microphone/WebRTC,
  local audio fetch/Blob playback, trust/CSP, and an executable native test runner.
  Confirm the recorded working loaders/public APIs and platform evidence.
- A checked inventory maps current operations and source/test files to preserved
  behavior or eventual transport-only deletion. Repeatable baseline measurements
  and representative real C++ wire examples exist.
- The current HTTP application and its useful owner/runtime/mailbox tests build;
  recorded baseline failures can be distinguished from new failures.

Confirm these in the current checkout and test records. Record actual entry points
and commands if earlier work renamed them. If evidence is unavailable, perform the
relevant check when possible; otherwise keep the prerequisite and gate pending.

## Execution rules and 400K context budget

Plan for **250–320K tokens of working context** within a **400K-token window**,
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
direction is host → bridge → app → core. The final products are macOS/WKWebView and
Windows/WebView2; Linux retains common tests, not a server release or new GUI.
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

Start with `src/web/{application_runtime,
application_config,web_settings,protocol,json,current_vault}.*`,
`src/web/{live_session,live_session_manager,command_queue,owner_wake_signal,
browser_connection_state,sse_mailbox,sse_stream,session_projection}.*`, bootstrap
helpers, `server_shutdown.*`, and their focused runtime/owner/mailbox tests.

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

### Bounded Delivery and Coalescing

Extract the useful mailbox behavior from `SseMailbox`; remove its stream-writer,
heartbeat, takeover, and final socket-drain machinery. Move its compatible-append
merge, snapshot fallback, and one-in-flight behavior with their tests into the
application layer. Adapt consumption signalling and sequence semantics to the
native channel; this is an extraction, not an unchanged file rename. A cadence
timer alone is not flow control.

For each live document, allow one outstanding native-to-JavaScript delivery
batch. Keep a bounded set of pending RPC replies, limited by admitted requests,
and at most one pending session update. Lifecycle/context notifications can
replace older notifications for the same state. Never drop an RPC reply for a
still-live document merely to coalesce session output.

The pending session update is either compatible accumulated text or a request
for a fresh snapshot:

- Merge adjacent appends only when the controller says they share a valid target.
- If a snapshot is pending, later changes are absorbed into a newer snapshot.
- On incompatible appends or a pending-text byte threshold, mark the projection
  dirty and discard the intermediate text. Ask the owner for a fresh snapshot
  when delivery can resume.
- Snapshot capture and subsequent append production remain serialized by the
  owner; dirty-state replacement must preserve that boundary.
- Keep only one scheduled UI drain. Do not enqueue a UI task for each provider
  delta or eagerly serialize every superseded snapshot.

The JavaScript receiver acknowledges a delivery batch after synchronously
validating/dispatching its messages, including safely discarded stale messages.
This small internal acknowledgement is needed to bound queued work inside a
stalled renderer; returning from WebView2's post call alone is not consumption.
Use a small control message containing only the connection and delivery ID;
there is no RPC result or pending-request slot for an acknowledgement. A receiver
handler failure triggers projection/bridge recovery but still releases the batch
in a finally path. The acknowledgement bypasses ordinary application work
admission. Scope acknowledgements to the
connection and batch so late acknowledgements cannot release another document's
delivery. macOS may use the receiver invocation's completion if it provides the
same consumption guarantee.

This is not a reliable-message protocol: no retransmission, replay storage,
heartbeat, or acknowledgement timeout ladder. A detached/failed document releases
its delivery state and later recovers from a snapshot. Application progress,
persistence, and shutdown never wait for acknowledgement.

A single snapshot can still be large. Measure capture, serialization, bridge
transfer, and React update costs for long conversations. Do not truncate
authoritative state or add pagination during this migration without a separately
demonstrated need. Bound pending work rather than retaining many snapshots.

Default to consumer-driven delivery: schedule a drain as soon as work is pending
and no batch is outstanding; consumption releases the next drain. Coalesce while
the consumer is busy, without imposing a fixed delay on every token. No periodic
timer or timer thread is required. Add a cadence on the existing UI event loop
only if measurements justify it, preserving prompt lifecycle delivery and the
same bounds. Measure Stop latency under provider bursts and stalled presentation.

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

1. Establish app/bridge build boundaries without introducing empty service
   classes. Separate retained DTOs/domain errors and serializers from SSE-only
   payloads and HTTP status mapping. Keep old adapters using shared definitions
   and preserve OpenAPI type-generation/checks during coexistence.
2. Separate application options from server options. Preserve meaningful session,
   queue, prompt, command, and shutdown limits. Native configuration ignores
   obsolete `[web]` values with warnings, including wrongly typed unused fields;
   validate server settings only in explicitly selected temporary HTTP mode.
3. Move actor/manager/command ownership into the application boundary. Keep one
   owner per controller, the manager-to-actor lock direction, bounded admission,
   and manager-owned joins. Move browser attach/disconnect policy to the temporary
   adapter; native lifetime must not depend on orphan timers or SSE drain waits.
4. Add asynchronous owner-command completion with the actual CommandResult when
   the short command executes, before generation finishes. Preserve notices,
   draft rules, queue limits, and unknown timeout outcomes. Only a temporary HTTP
   worker may synchronously wait; native execution must not occupy a pool waiting
   for owner replies.
5. Implement selected-session ownership and retirement: keep the selected actor
   over reload; retire deselected idle actors; let existing background generation
   finish, then retire unless reselected. Coordinate selection/open/deletion,
   preserve selection on failed open, reap eligible idle actors before limit
   errors, count starting/stopping actors, and never silently cancel busy actors.
6. Introduce the narrow owner-output seam and extract mailbox merging, snapshot
   fallback, and one-in-flight behavior with their tests. Leave SSE writer,
   heartbeat, takeover, and final-drain policy only in the legacy adapter.
7. Atomically establish subscriptions and initial snapshots on the owner. Scope
   output by connection/context/subscription/full session identity. Assign native
   sequence numbers after coalescing when output is committed: initial snapshot
   zero, one shared sequence for snapshots/appends, no reset on later snapshots.
8. Bound pending append bytes; on incompatible/oversized updates mark the
   projection dirty and capture fresh state on the owner when delivery resumes.
   Preserve the snapshot/following-append boundary and capture terminal state
   before owner destruction. Use consumer-driven draining, one scheduled drain,
   and no periodic timer. Preserve old SSE sequence semantics in its adapter.
9. Split Application composition from the optional HTTP runtime: workspace,
   repository, providers, credentials, actors, audio dependencies, and mirroring
   open without a server, port, cookie, or listener thread. Move existing vault/
   transfer implementations with their ownership, leaving native bindings for
   the maintenance/vault/native-action implementation. Running state must not depend on binding a port.
10. Extract the minimal bootstrap/session create/open/submit/stop/snapshot/close
    operations for the first native flow. Provide selected-session state and an
    initial context epoch; check captured context under the lifecycle guard.
    Preserve protected-vault discovery/unlock, semantic validation, and errors.
11. Establish safe ordinary shutdown and early-failure cleanup with correct
    owner/provider/store dependency order and final persistence. Expose nonblocking
    host shutdown initiation; the native host-integration stage stress-tests it before wider migration.
12. Run headless Application tests, reused owner/mailbox/manager/projection tests,
    and affected HTTP regression tests. Add races for initial snapshot, terminal
    capture, reselect/retire, async completion, deletion, and visiting more idle
    sessions than the actor limit. Verify obsolete native configuration handling.

## Verification for this block

- Build shared app/core without inbound HTTP or platform UI dependencies; exercise
  Application startup/bootstrap/create/open/submit/Stop/snapshot/persist/shutdown
  headlessly with temporary data and deterministic providers.
- Preserve and move the assertions in owner/manager/command/projection/mailbox/
  runtime/configuration tests. The existing HTTP adapter must still use the same
  domain implementation and pass affected regressions.
- Assert the real CommandResult arrives after command execution and before model
  completion; exercise queue full, timeout/late completion, early startup failure,
  and safe provider/store teardown.
- Visit more idle sessions than the cap, reselect during retirement, finish busy
  deselected generation, delete while opening, and verify lease release and
  retained selection after a failed open.
- Race subscribe with output, coalesce snapshots/appends under stalled consumption,
  validate monotonic delivery sequences, and capture terminal state before owner
  destruction. Pending work stays bounded without a renderer drain requirement.
- Check missing/valid/obsolete malformed native `[web]` values are ignored with
  warnings while malformed TOML and active configuration errors still fail.

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

Headless Application creates/opens, submits/stops, snapshots,
persists, and shuts down without a listener. App/core has no inbound HTTP or UI
API dependency. Native event/retirement invariants pass; legacy HTTP still uses
the same domain behavior through temporary adapters.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide actual targets/types/APIs, ownership and completion rules, configuration
split, event sequence/bounds, focused test commands, and temporary compatibility
code whose removal belongs to the final server-removal stage.

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
