# CHA Native WebView Bridge Migration Handover

## 1. Decision, Scope, and Expected Benefit

Replace CHA's internal HTTP/SSE interface with a small asynchronous native
WebView bridge. React remains the presentation layer. C++ remains responsible
for application state, domain behavior, providers, and persistence.

The supported release applications in this migration are macOS (WKWebView) and
Windows (WebView2). The standalone application becomes the only supported
deployment model. Retire the existing browser/server package at cutover. An iOS
product is a separate project.

The primary benefit is removing an operating model and its failure modes:
localhost listeners, access cookies, HTTP request workers, browser-facing SSE,
heartbeats, browser takeover, and network reconnect policy. Expect a moderate
net reduction in production code, not deletion of everything under `src/web`.
Application orchestration and data validation currently live there too.
Platform messaging and resource delivery replace some transport code.

The migration must demonstrate a net simplification. Record production-code,
test, dependency, and configuration changes at the final cutover. Do not count
moving files, renaming namespaces, or deleting behavior tests as simplification.
Do not make a promised line-count reduction a reason to remove necessary behavior.

Keep the implementation small:

- One fixed operation dispatcher, asynchronous replies, and an ordered session
  event channel.
- One native window and at most one selected session event subscription per
  document. No general publish/subscribe or multi-window framework.
- Reuse the existing session owner, command queue, projection, and bounded
  mailbox behavior.
- Extract application operations into ordinary functions or existing objects.
  Additional service classes are optional, not migration requirements.
- No general RPC framework, replay log, plugin system, durable command ledger,
  compatibility negotiation, or IPC heartbeat.

Preserve session semantics, multicast, reasoning, cover/uncover, delete-turn,
default-character changes, configuration constraints, vault behavior, provider
execution, cancellation, audio, and restart persistence. Keep the database
format and SQLCipher integration unchanged.

## 2. Architecture and Ownership

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

## 3. Inventory Before Extraction

Create a checked migration inventory before deleting any adapter. Each entry
must identify its current caller, application behavior, native replacement,
public result/error contract, cancellation/lifetime behavior, and retained
verification. Names in this document are proposed vocabulary; the inventory
of actual code is the completion checklist.

Inspect:

- Every `ChaClient` method and every HTTP route.
- The separately injected `SessionEventsConnector`, snapshots, appends, and
  session lifecycle notifications.
- Direct frontend `fetch` calls, audio URL helpers, downloads, file inputs,
  navigation/deep links, browser storage, and clipboard use.
- Voice-input WebRTC setup, microphone permission, and its authenticated
  provider request, which currently runs in JavaScript.
- Native runtime entry points and menus, including pre-WebView vault unlock,
  import/export, R2 transfer, shutdown, and post-maintenance reload.
- Public errors, operation timeouts, generated TypeScript types, build scripts,
  package checks, upgrade checks, and the deterministic fake provider.

Classify each entry as an application operation, session event, resource,
native UI action, or transport-only behavior. Preserve useful behavior found
under a transport name. Also classify each affected source and test file as
keep, move, split, or delete, recording where retained behavior/assertions go.
For example, `src/web/sse_mailbox.cpp` and `tests/web/unit_sse_mailbox.cpp`
contain coalescing and backpressure behavior that must survive extraction;
neither file is disposable merely because it lives under `web`.

Minimum coverage:

| Area | Behavior that must be accounted for |
|---|---|
| Bootstrap | Initial/selected conversation, summaries, recent sessions, active vault, capabilities |
| Sessions | List/create/open/rename/delete, submit/multicast, stop, cover/uncover, delete-turn, default character, export |
| Characters/personas | CRUD, character definitions and Markdown files, reference validation |
| Forums | CRUD, members, forum Markdown files, effects on active sessions |
| Providers | CRUD/test, usage constraints, keys, external request diagnostics |
| OAuth | Status/start/poll/disconnect and opening the authorization URL externally |
| Styles/voices | CRUD, usage constraints, voice-input/output settings and runtime capabilities |
| Credentials | List metadata, create/rename/replace/delete, R2 settings |
| Vaults | List/create/update/delete/switch/merge, password/protection, R2 listing/download |
| Maintenance | Configuration import/export, database upload/download, reopen failure |
| Audio | Uncached speech/preview, cache lookup/clear, single/batch generation, status, playback and cancellation |
| Native host | Startup unlock, menu state/actions, file dialogs, external links, close/quit/reload |
| Packaging | Native assets, type generation, release verification, upgrade compatibility, legacy server retirement |

Retain `src/chat`, `src/characters`, `src/providers`, `src/session`,
`src/workspace`, and non-transport utilities. Provider-side SSE decoding,
libcurl, leases, transcript journals, immutable workspace publication, mirroring,
SQLite, and SQLCipher are not browser transport and must survive.

## 4. Application Extraction and Frontend Boundaries

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

## 5. Runtime State, Vault Context, and Maintenance

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

## 6. Session Ownership and Retirement

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

## 7. Bridge Requests and Command Completion

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

## 8. Dispatch, Threads, and Shutdown

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

## 9. Ordered Session Events

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

## 10. Bounded Delivery and Coalescing

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

## 11. Document Lifetime and Rehydration

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

## 12. Packaged Assets and Development

Prove the production asset/origin choice on both platforms before broad
application extraction. Loading a page is not sufficient: the chosen origin
must support the actual React build, module/assets paths, navigation/reload,
storage used by CHA, microphone/WebRTC, audio, and enforceable CSP.

### macOS

Start with a restricted packaged resource scheme implemented through
WKURLSchemeHandler. Local-file loading is an alternative only if it passes the
same tests; a restricted file root does not prove secure-context or microphone
compatibility. Use public WebKit APIs and test on the supported minimum macOS
version as well as the development machine.

Do not assume a custom scheme supports getUserMedia merely because its HTML
loads. The early platform spike must establish a working supported origin and
record the chosen loader and permission behavior. If neither candidate preserves
voice input, native cutover is blocked until a small supported solution is proven.
Do not quietly disable voice, enable private WebKit flags, or turn this transport
migration into a native media-stack rewrite.

### Windows

Use HTTPS virtual-host-to-folder mapping for immutable packaged frontend assets.
Use controlled resource handling for dynamic application media. Neither opens
a TCP listener. Restrict cross-origin access and map only the packaged asset root.

Microsoft documents secure-context support for virtual HTTPS content and
different limitations for file loading, as well as potential media-loading
limitations for virtual-host mappings. Verify dynamic media separately:
[WebView2 local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content).

### Shared asset requirements

Use hash routes for the native UI on both platforms: the packaged document's
path stays fixed, with `#/` or `#/s/{forum}/{session}/` identifying the view.
Keep the existing route grammar/identifier validation, but read the fragment
and make history writes fragment-only. Update initial navigation, back/forward,
reload, reset-to-Welcome, and route tests together. A same-document route change
does not replace the bridge connection. Keep path routing only in the temporary
HTTP frontend during coexistence; no new routing library is needed.

Do not assume a custom scheme permits the current path-changing `pushState`.
The [History API URL-rewrite rules](https://html.spec.whatwg.org/multipage/nav-history-apis.html#can-have-its-url-rewritten)
restrict path changes for both file URLs and non-HTTP(S) schemes. Serving the
shell from a scheme handler does not itself relax those rules. With hash routes,
the asset loader serves a fixed shell and known assets; missing assets fail
instead of becoming an arbitrary shell fallback. Test fragment navigation in
both actual hosts.

Update Vite's base/module asset paths for the selected loader. Do not assume the
current root-relative asset paths work with file loading. Keep application
assets immutable and separate from vault files and generated media.

### Development

Vite may serve only frontend assets/HMR. Domain calls still use the native
bridge. Require an explicit development build/launch mode and one exact
allowlisted origin, including scheme and port. Release builds must not honor
a development-origin override or expose debug listeners.

A browser-only fake bridge remains useful for component/client tests. Production
behavior must also be tested in actual native hosts. During coexistence, select
HTTP or native transport explicitly at startup; never automatically retry an
uncertain mutation through the other transport.

## 13. Local Resources, Voice Input, and Speech

Large media and database exports do not travel as JSON/base64 RPC. Small control
data, including WebRTC setup descriptions, can use ordinary bounded messages.

For document-owned voice setup and speech synthesis, cancellation names the
original bridge request ID within its connection. The native client knows that
ID before submission and maps its AbortSignal/stop action to the specific cancel
operation; callers do not have to wait for a resource handle to cancel. A cancel
received while the operation is queued prevents it from starting. A late cancel
cannot affect a newer request. This uses the existing pending-call record, not
a new general job registry. Application-owned audio cache jobs keep their
existing independent identity and lifetime.

### Resource lifetime and isolation

Return opaque resource handles/URLs scoped to a connection and context epoch.
The application validates session/vault ownership and supplies bytes; the host
adapts that reader to its resource API. The application never sees WebKit/COM
resource-task objects.

- Resolve only registered resource types and opaque IDs, never JavaScript paths.
- Reject handles after release, context change, deletion/cache invalidation, or
  document teardown. Do not allow browser caching to bypass current access.
- Cancel/release native reads when a platform resource task stops. A callback
  completing after cancellation must not use a destroyed task or old database.
- Set the correct MIME type and length. Start with complete-resource reads into
  the existing Blob playback path described below; do not build a range-serving
  media adapter unless a proven platform or performance need requires it.
- Keep decrypted/private media out of a persistent WebView disk cache. Use the
  chosen handler's effective no-store policy and verify it; temporary plaintext
  files require controlled private storage and cleanup.
- Do not expose user-controlled HTML/JavaScript or arbitrary vault files under
  the privileged packaged-application origin.

A virtual HTTPS URL is allowed when resolved entirely by the WebView adapter.
The requirement is no socket-served application resources.

### Voice input and secrets

The current voice runtime endpoint returns the credential value and
`voiceInput.ts` performs the authenticated setup fetch in JavaScript. That path
must change to satisfy native credential ownership.

Keep microphone capture, the WebRTC peer, transcription data channel, and
presentation normalization in the WebView when the platform spike proves them.
Add a specific `voiceInput.connect` operation: JavaScript supplies the bounded
session description needed for setup; native code selects validated settings
and credentials, performs the existing authenticated provider exchange, and
returns the answer description. JavaScript never supplies an arbitrary
destination or receives the stored credential. Preserve existing transcription
settings and external protocol behavior.

Cancel pending setup on user cancel, document loss, context change, or shutdown.
After a late reply, a cancelled JavaScript voice session must not attach it to a
new peer. Stop microphone tracks and close the peer/data channel on all terminal
paths. Permission denial and unavailable capture remain actionable states.

This moves a small authenticated exchange, not the full real-time media stack,
into C++. `voiceInput.runtime` returns only non-secret settings/capabilities.
Verify the existing provider's setup behavior end to end before removing the
old route. Do not introduce a different provider protocol as part of migration.

### Voice output and cached audio

Preserve both uncached speech/preview and persistent entry-audio generation.
`getEntryAudioSource` alone does not cover the existing feature.

Use operation-specific speech start/cancel/release calls for ephemeral synthesis,
returning an opaque resource handle when ready. A cancelled document must not
leave a provider request or private temporary resource retained indefinitely.
A late completion after cancellation is released without starting playback.

Keep cache generation as application-owned jobs with single/batch submission,
status, errors, cached-entry notifications, and cache clearing. Reload may
reattach to those jobs; it must not submit the same batch automatically.

Preserve the existing `textToSpeech.ts` playback flow: read complete audio into a
Blob, create an object URL, and use that URL for playback/pause/resume. The typed
audio/resource client fetches the opaque local resource through the WebView
handler, replacing server audio URL construction. This binary resource read is
allowed; application RPC still goes through the native bridge. Bytes do not
travel through JSON/base64 messages.

Prove this resource fetch, its origin/CSP permissions, and Blob playback/seek in
both hosts during Phase 1; custom-scheme HTML loading alone does not prove fetch
support. This retains today's whole-audio memory cost and avoids implementing
Range/206 handling merely to preserve seeking. Direct resource-URL playback is
an alternative only if the spike or measurements justify it; it must then prove
the platform's range/seek requirements before adoption.

Preserve AbortSignal cancellation, object-URL revocation, and native resource
release on stop/error/disposal. Also abort reads, stop playback, and clear Blob
references on context/resource invalidation: revoking a native handle does not
revoke bytes already copied into JavaScript. Test generation cancellation
separately from stopping playback and cancelling an application-owned cache job.
Preserve outbound provider networking and current audio provider concurrency/
diagnostic policy.

## 14. File Operations, Native Menus, and Operation Coverage

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

## 15. Security and Credential Boundaries

JavaScript input remains untrusted even when frontend/native code ship together.
Keep domain-scoped operations; never expose `executeShell`, `rawSql`,
`readAnyFile`, `writeAnyFile`, pointers, or generic native object access.

### Document and message trust

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

### CSP and resources

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

### Secrets

Stored API keys, R2 secrets, vault passwords retained for native operations, and
OAuth tokens remain native. Runtime DTOs expose IDs, names, presence, and
non-secret settings. Secret creation/replacement is an explicit write-only flow;
clear UI buffers promptly and never return the stored value in normal reads.

Do not log bridge parameters, raw replies, prompts, responses, credentials,
session descriptions, or sensitive configuration. Resource caches must not
persist plaintext from protected vaults through the browser after release.

## 16. Validation, Errors, Timeouts, and Diagnostics

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

## 17. Startup, Bootstrap, and Configuration Compatibility

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

## 18. Build and Packaging

Final logical targets:

```text
cha_macos   ----+
               +--> cha_bridge --> cha_app --> cha_core
cha_windows ----+
```

Core includes existing chat, characters, providers, session, workspace, and
utilities. Outbound libcurl and provider protocol decoding stay. Remove
cpp-httplib from the standalone production dependency graph; deterministic fake
provider/test targets may retain it as an isolated test dependency.

Package the built React assets directly. Preserve supported OS/runtime checks,
icons, signing, resource paths, and the current startup/upgrade behavior. Keep
WebView2 runtime checks in the Windows host.

Both desktop packaging scripts currently verify production web files using
`chaweb`; macOS also reuses legacy package-check machinery. Replace those checks
with application-level compatibility tests plus real native-host verification
before removing the server target. Do not relabel a browser test with a fake
bridge as a packaged native end-to-end test.

At cutover remove the server executable, listener/runtime cookie wiring, port
flags, production HTTP asset serving, Vite API proxy, server launch scripts, and
legacy server package target. Retain reusable seed data and compatibility tests;
move shared assets out of package-specific locations when necessary rather than
deleting them with the package.

## 19. Migration Plan and Gates

Each phase leaves a working application. Keep the HTTP path explicitly selected
for development/verification until native behavior has replacement coverage.
Extract only what the next native flow needs; do not first build every proposed
service class.

### Phase 0 — Inventory and contract fixtures

Complete Section 3's inventory, settle public DTO/error mappings, record native
startup/menu behavior and supported platform versions, and retain representative
serialized snapshots/commands as compatibility fixtures. Record baseline code,
tests, dependencies, and startup behavior for the simplification assessment.

### Phase 1 — Platform feasibility before broad extraction

On macOS and Windows prove the packaged asset/origin choice, a minimal native
request/reply, data-safe JavaScript delivery, microphone permission and WebRTC
setup, local resource fetch and Blob playback/seek/release, fragment navigation/
reload, and CSP enforcement. Establish the native automation paths in Section 20
and run an asserted bridge round trip through each actual WebView.

Use the existing voice provider contract and native credential-bearing setup
exchange. Record the working public APIs and supported OS/runtime versions.
A failure here blocks broad cutover; resolve it while the current application
still works. Do not defer this risk until after removing most HTTP behavior.

### Phase 2 — One application/session flow

Extract Application startup, the required session operations, and the actor/
mailbox separation while the HTTP adapters continue using the same behavior.
Add the common dispatcher and native client/event adapter. Preserve the
short-command result and asynchronous generation.

Prove this flow on macOS, then Windows, before widening scope:

```text
launch/unlock -> bridge.info -> app.bootstrap
    -> session.open -> session.subscribe -> initial snapshot
    -> session.submit -> streaming -> session.stop
    -> reload -> bootstrap -> reopen/resubscribe -> current snapshot
```

Include unsubscribe/retirement and a standalone snapshot read. Use the
deterministic provider so correctness does not depend on a live model service.
Automate this flow in both native hosts before expanding the migration.

### Phase 3 — Lifetime and concurrency cutover gate

Implement and test subscription replacement, duplicate/gap handling, snapshot
ordering, bounded renderer delivery, old-document replies, pending-command
reload, saturation with responsive Stop, repeated idle-session navigation,
context invalidation, maintenance failure, and quit with work outstanding.

Integrate maintenance and native menus through the common lifecycle gate now,
before proliferating domain bindings. Do not remove the SSE recovery path until
the native event path passes these tests.

### Phase 4 — Remaining application operations

Migrate inventory groups in small changes: workspace/CRUD, settings and provider
tests, credentials/OAuth, vault/R2 operations, configuration/database transfers,
and native menu integration. Add wire bindings only where a frontend caller
needs them. Carry over reference checks and affected-session invalidation.

For each group, compare behavior through application tests and native client/
serializer tests before retiring its HTTP implementation.

### Phase 5 — Complete media, resources, and file flows

Finish the already-proven loaders and resource adapters. Migrate uncached speech,
cached single/batch audio generation/status/clear, voice setup/cancellation,
save/open operations, and resource invalidation. Verify playback/resume and
cancel/error paths in actual hosts. Remove frontend HTTP RPC and server audio
URL helpers as their replacements become complete; retain the typed client's
local binary-resource reads.

### Phase 6 — Packaged parity and upgrades

Run real packaged end-to-end flows on macOS and Windows, including microphone,
native dialogs, protected startup, persistence, maintenance, and old configuration.
Replace each existing server-based desktop package/upgrade check with its retained
application behavior or native-host equivalent. Verify the release works with
no CHA listener and with development mode disabled.

### Phase 7 — Remove HTTP/SSE and obsolete packaging

Delete server adapters/runtime, listener/security configuration, SSE/EventSource,
network recovery, HTTP client implementation, production server asset delivery,
Vite API proxy, server launch/package targets, and isolated transport-only tests.
Before deleting the `chaweb` Playwright launcher, account for each retained
end-to-end assertion in the native or browser/fake-bridge suites. Actual native
integration assertions cannot be replaced solely by fake-bridge tests.

Move retained DTO definitions/type generation before removing their old contract.
Remove cpp-httplib from release targets, retaining it only where tests actually
need it. Retire the legacy server release and preserve platform-independent tests
and seed data.

### Phase 8 — Verify the simplification

Audit the final dependency graph and the migration inventory. Report actual net
production/test changes and the removed configurations and failure modes. Remove
temporary fallback flags, duplicate operations, and unused adapters. Keep only
the chosen loaders and public protocol implementation, not every prototype.

Do not declare completion while a normal operation still falls back to HTTP or
a packaged verification path still depends on `chaweb`.

## 20. Verification and Acceptance

Retain current tests for transcript/model context, controller behavior,
repository/storage/leases, workspace/configuration, provider protocols and
cancellation, audio, mirroring, concurrency, and shutdown. Move assertions with
their behavior; directory or transport names do not determine whether a test
is obsolete.

Add focused application-operation tests without a WebView. Test the bridge
dispatcher with the real serializers and fixed envelopes, and the native client
with a fake bridge. Shared wire examples must catch mismatches those two
independent test suites would otherwise agree to overlook.

### Automated native end-to-end coverage

Migrate the behavior in `webapp/e2e`, not just its launch command. Replace HTTP
fixture setup and response interception with temporary fixture data or targeted
native test seams, and retain UI/persistence assertions. Use isolated temporary
vaults and the existing deterministic provider.

- Windows: launch a test build of the actual CHA host with packaged assets and
  a test-only CDP port and isolated WebView2 user-data directory. Attach using
  Playwright's `chromium.connectOverCDP` and reuse appropriate selectors and
  assertions. [Playwright supports WebView2 automation](https://playwright.dev/docs/webview2);
  no CHA HTTP API is required.
- macOS: add a small test runner to a test build of the existing host, using
  WKWebView's public JavaScript evaluation APIs for DOM actions and bounded
  assertion polling. It runs the actual React UI, native bridge, and application,
  and reports assertions/timeouts with a failing process exit status. This is a
  host-specific test harness to implement and prove, not an assumption that
  Playwright's standalone WebKit browser can attach to WKWebView. Keep it limited
  to CHA's tests rather than building a general automation framework.

On both platforms automate create/open, submit/stream/Stop, reload, vault switch,
and persisted-state restoration early, then port retained coverage from the
remaining suites. A successful page load alone does not satisfy these checks.
OS permission dialogs, real microphone input, and native file dialogs also need
platform checks; manual checks there supplement automated application coverage.

Keep automation entry points out of shipping builds. CDP is a test debugging
listener, not a retained application HTTP transport; separately verify the
uninstrumented release with development mode disabled and no CHA listener.
Retain the old HTTP test path only during migration. A permanent development
HTTP adapter would preserve routes, serializers, server startup, and maintenance
work while still missing native bridge failures. Include native harness code in
the net complexity assessment and require working automation before deletion.

### Required race and failure cases

- Subscribe while the owner is producing text: the initial snapshot plus later
  appends yields exactly the authoritative transcript.
- Replace a pending snapshot with newer state, merge appends, and complete
  generation while delivery is stalled.
- Duplicate or reorder an event; an old snapshot must not overwrite newer
  state, and an append gap must resubscribe without duplicating or losing text.
- Unsubscribe/resubscribe immediately, including React StrictMode cleanup.
  A delayed old subscribe/unsubscribe cannot replace/detach the current stream.
- Reload with a mutation pending and reuse its numeric request ID in a new
  connection. An old reply must not resolve the new promise or replay a mutation.
- Kill/recreate the renderer and dispose a view while native callbacks are queued.
  Keep live application state; do not access destroyed platform objects.
- Hold JavaScript consumption while providers continue. Native pending work and
  renderer deliveries stay bounded, and owner/persistence/shutdown make progress.
- Saturate ordinary bridge work with provider tests or audio setup and verify
  Stop/disposal admission and bounded rejection of additional calls.
- Visit more idle conversations than the actor limit; ensure retired actors
  release resources. Verify busy unselected sessions complete and then retire.
- Switch vaults with queued CRUD/session mutations, audio jobs, and resource reads,
  including overlapping IDs and switch-away/switch-back. Old work cannot affect
  or disclose the new context.
- Fail maintenance before and after handles close; safely reopen or consistently
  enter unavailable state. Verify native menus and React observe the same state.
- Quit during generation, provider testing, voice setup, audio, dialogs, and
  maintenance. Persist settled state and do not destroy resources still in use.
- Cancel voice setup/speech before and after a provider reply, deny microphone
  permission, and verify late completion does not restart capture/playback.
- Attempt bridge calls from remote/subframe content and stale documents; test
  script-looking text in results, path traversal, resource revocation, and
  effective CSP/private-media cache behavior.

### Required real-host/package checks

On both supported desktop platforms, verify:

- Packaged assets, module paths, UI navigation/back/reload, and used web APIs.
- Exact bridge-version checking and bootstrap after document replacement.
- Normal and protected-vault startup, wrong password, cancellation, and restart.
- Session create/open/rename/delete, submit/multicast, reasoning, visible
  streaming, Stop, cover/uncover, delete-turn, and default-character changes.
- Workspace/settings/provider tests, credential write-only flows, OAuth, and R2.
- Vault switch/merge/protection, configuration and database transfers, reopen
  failure, and persisted state across restart.
- Microphone/WebRTC, voice cancellation, uncached speech, cached/batch audio,
  playback/seek/resume, native save/open, and resource cleanup.
- External link isolation, enforced CSP, and release-disabled development access.
- Startup/upgrade with missing or obsolete server configuration and unchanged
  database format.
- No application listener or cookie, no EventSource, no fetch-based CHA RPC, and
  no production or package-verification dependency on `chaweb`.

Use the existing deterministic local OpenAI-compatible fake provider for
integration tests. Its test HTTP listener is outbound-provider infrastructure,
not a production CHA application listener. Browser/fake-bridge tests can remain
for fast presentation coverage, but do not replace the actual platform checks.

### Tests eligible for deletion

Delete assertions that only describe removed route matching, HTTP framing/status,
listener startup/ports, old cookie/Host authorization, request worker counts,
SSE heartbeat/takeover, Vite proxying, or server cache headers.

Rewrite the behavioral assertions embedded in those suites first: size limits,
malformed DTO handling, trusted-document checks, bounded updates, CSP, resource
privacy, persistence, and user-visible recovery still have native equivalents.
Do not count removal of these assertions as successful complexity reduction.

## 21. Final Boundary and Out-of-Scope Work

Completion means all inventory entries have a native replacement or an explicit
transport-only deletion, both packaged applications pass the checks above, and
the production build no longer needs an internal HTTP server.

The remaining application complexity is intentional:

```text
session owner and bounded commands
provider concurrency, streaming, and cancellation
workspace validation and immutable publication
SQLite/SQLCipher, leases, persistence, and vault maintenance
audio generation/cache/playback and native resource access
short asynchronous dispatch, document/context invalidation
ordered snapshots/appends and bounded presentation delivery
application shutdown and public error semantics
```

The removed complexity includes:

```text
application TCP listener, ports, server startup and shutdown
HTTP routes, inbound request framing, status mapping, and worker pool
localhost access cookie and old Host/Origin API authorization
browser-facing SSE, heartbeat, socket writers, and stream takeover
EventSource, network probes, reconnect delay ladder, orphan timers
production HTTP asset server and Vite API proxy
server executable/package/launch configuration
```

Retain outbound HTTP, targeted validation, native document trust checks, and the
small amount of queue/lifetime machinery required by asynchronous presentation.
The design succeeds by removing unnecessary mechanisms, not by renaming them
or pretending all asynchronous failure modes disappear.

Do not combine this migration with a backend language rewrite, database/schema
migration, replacement of SQLCipher/provider transport, character/session
redesign, plugin architecture, multi-window behavior, a full native media-stack
rewrite or iOS lifecycle implementation. Keep platform
types out of common code so a later host can reuse it without making that future
product part of the current implementation.
