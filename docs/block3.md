# Block 3 — Implement the bridge and native frontend

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** complete. **Environment:** a supported native compiler and Node/npm.

## Objective and scope

Implement the fixed asynchronous common bridge, bounded request/delivery lifecycle, typed native frontend client and event projection, and fragment-only native routing.

Bind the minimal Application/session flow already implemented. Keep other native methods explicitly unavailable in development; do not forward them to HTTP. Real platform-host integration and wider operation migration are outside this block.

## Prerequisites to verify before editing

- A listener-free Application exposes bootstrap and initial context, selected
  session state, create/open/submit/Stop/close, and owner-thread snapshot reads.
  Shared DTO serializers and actual CommandResult/error semantics exist.
- Owner commands complete asynchronously. Session retirement and an extracted
  mailbox provide scoped, owner-ordered snapshots/appends with bounded pending
  state and delivery-time sequencing.
- Headless app/owner tests and real C++ wire examples pass. The temporary HTTP
  adapter still operates on the same domain behavior.
- Working native loader/message probes and runner choices exist for both hosts;
  this block uses their contracts but does not need to attach a real WebView.

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

Start with extracted app/serializer/mailbox
APIs, `webapp/src/api/{client,events,guards}.ts`,
`webapp/src/components/{App,ChatScreen}.tsx`,
`webapp/src/state/{route,sessionRecovery,bootstrap,view}.ts`, and focused tests.

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

### Packaged Assets and Development

Prove the production asset/origin choice on both platforms before broad
application extraction. Loading a page is not sufficient: the chosen origin
must support the actual React build, module/assets paths, navigation/reload,
storage used by CHA, microphone/WebRTC, audio, and enforceable CSP.

#### macOS

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

#### Windows

Use HTTPS virtual-host-to-folder mapping for immutable packaged frontend assets.
Use controlled resource handling for dynamic application media. Neither opens
a TCP listener. Restrict cross-origin access and map only the packaged asset root.

Microsoft documents secure-context support for virtual HTTPS content and
different limitations for file loading, as well as potential media-loading
limitations for virtual-host mappings. Verify dynamic media separately:
[WebView2 local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content).

#### Shared asset requirements

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

#### Development

Vite may serve only frontend assets/HMR. Domain calls still use the native
bridge. Require an explicit development build/launch mode and one exact
allowlisted origin, including scheme and port. Release builds must not honor
a development-origin override or expose debug listeners.

A browser-only fake bridge remains useful for component/client tests. Production
behavior must also be tested in actual native hosts. During coexistence, select
HTTP or native transport explicitly at startup; never automatically retry an
uncertain mutation through the other transport.

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

## Implementation steps

1. Define fixed native request/reply/error/event envelopes and exact
   `bridge.info` version checking. Add envelope types to contract checks while
   keeping existing authoritative DTOs and real C++ serializer fixtures.
2. Create native-owned document connection state. Validate envelopes, method
   allowlists, required/unknown fields, safe numbers, IDs, encoded sizes, parameter
   shapes, and context before effects. Reject duplicate outstanding request IDs;
   a matching JSON connection ID does not establish actual sender trust.
3. Bind only the first-flow operations from the application/session extraction. Decode, call Application,
   and encode approved results/errors. Preserve CommandResult and timeout meaning;
   do not acknowledge mere queue admission or automatically replay mutations.
4. Bound admitted calls and ordinary work. Preserve separate admission for Stop,
   unsubscribe, acknowledgement, disposal, and shutdown. Do not block session
   completion behind provider work. An over-limit sender gets bounded reporting/
   invalidation rather than an unbounded queue of rejection replies.
5. Retain reply capacity until consumption or connection invalidation. Batch
   replies, bounded session output, and coalesced lifecycle notices through one
   outstanding native-to-JS delivery. Use connection/batch-scoped consumption
   acknowledgement outside RPC admission, with no retransmission/heartbeat ladder.
6. Track the latest requested subscription before owner work is enqueued. Guard
   all completions with checked lifetimes; delayed subscribe/unsubscribe cannot
   replace or remove a newer subscription. No raw WebView pointer enters common
   application/bridge work.
7. Inject a native `ChaClient` and separate `SessionEventsConnector` at frontend
   startup. Keep platform globals out of React. Install the receiver early,
   correlate promises, check real serialized data, and reject local promises on
   disposal; `invoke<T>` casts are not runtime validation.
8. Allocate subscription IDs/install handlers before subscribe. Accept initial
   snapshot and subscribe reply in either order. Discard stale/duplicate scoped
   output, require consecutive appends, and allow valid newer snapshots to set
   the baseline. After a gap/unknown target, invalidate that subscription and
   replace it; do not splice a standalone snapshot into the live stream.
9. Acknowledge delivered batches in a finally path after synchronous dispatch,
   including stale messages; receiver failure triggers recovery without holding
   the slot forever. Preserve StrictMode cleanup and stale-navigation guards.
10. Use fragment-only native routes with the existing validated grammar. Update
    initial parsing, all history writes, Welcome resets, and back/forward; a hash
    change keeps the same connection. Retain old path routing/network recovery
    only in temporary HTTP mode.
11. Mark unmigrated native capabilities explicitly unavailable in development,
    never silently successful or forwarded to HTTP. Add common bridge/client/
    projection/component tests for malformed input, scope/sequence/ID races,
    timeout/late completion, saturation, and stalled acknowledgement; run schema,
    TypeScript, and frontend build checks.

## Verification for this block

- Test the common dispatcher with actual serializers: unknown/malformed requests,
  invalid fields/numbers/limits, stale context, duplicate outstanding IDs, approved
  public errors, no effects on rejected requests, and exactly one terminal reply
  at most per live admitted call.
- Exercise timeout then late completion without replay, subscription replacement,
  stale unsubscribe, delayed acknowledgement, connection invalidation, saturation,
  and reply-capacity retention until consumption.
- Test native frontend code with a fake bridge and real C++ wire examples, not
  only independently handwritten expected DTOs.
- Exercise both subscribe-reply/initial-snapshot orders, append gaps and unknown
  targets, newer snapshots, stale actor/document output, StrictMode cleanup, and
  receiver exceptions that still acknowledge the batch.
- Verify native fragment parsing/writes/back/forward/reset/reload and unchanged
  temporary HTTP path routing. Run schema freshness, TypeScript, affected component
  tests, and the production frontend build.

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

Common bridge and native client/projection work with bounded
admission/delivery and verified C++ wire examples without platform libraries.
Native routing/recovery tests pass while the temporary HTTP frontend still works.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide protocol version/contracts, limits, injection points, callbacks/
acknowledgements, native capability gaps, and exact focused test commands.

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
Status: complete
Starting and resulting revision/checkpoint:
  start bc5f3a3 (Block 2). Result is uncommitted working tree on redesign.
Files changed/moved and actual new APIs/targets:
  src/bridge/{bridge_protocol,bridge_router}.*
  tests/bridge/unit_bridge_{protocol,router}.cpp
  cha_bridge / cha_bridge_tests
  src/web/json.cpp moved into cha_app (parsers reused by the bridge)
  CommandReply::{set_ready_callback,peek,abandon}
  Application::{unsubscribe,check_context}
  webapp/src/api/native{Bridge,Client,Events}.ts
  webapp/src/main.tsx injects native client when __CHA_NATIVE_POST__ exists
  OpenAPI native envelope schemas + invalid_argument/operation_cancelled/application_unavailable
Implemented behavior and key ownership/contract decisions:
  protocol_version=1, application_version="0"
  Native-owned connection_id via BridgeRouter::open_connection(); JSON id must match
  Allowlist: bridge.info, app.bootstrap, session.{create,open,submit,stop,close,snapshot,subscribe,unsubscribe}
  Other methods: invalid_argument "That method is not available."
  Ordinary in-flight 16, control (stop/unsubscribe) 8; overflow invalidates with one error
  Session commands complete asynchronously via CommandReply callback; no UI-thread wait
  One outstanding native-to-JS delivery batch; ack is connection_id+delivery_id, not an RPC
  Latest subscription_id recorded before owner enqueue; stale subscribe -> operation_cancelled
  Native ChaClient does not fall back to HTTP; unmigrated methods throw invalid_argument
  Native stream recovery is 'replace' (new subscription), not the HTTP reconnect ladder
  Hash routing already existed; writes/reload tests extended
Prerequisites verified and evidence used:
  cha_app_tests (16) passed: bootstrap, create/open/submit/stop/snapshot/close, async submit, subscribe seq 0
  HTTP adapter still linked and ApplicationRuntime/LiveSession/WebWireFixtures tests passed
Temporary compatibility code and when it can be removed:
  HTTP frontend (createChaClient, EventSource, path routes) until transport-removal block
  OpenAPI HTTP paths retained; native envelopes live beside them
  window.__CHA_NATIVE_POST__/__CHA_NATIVE_RECEIVE__/__CHA_NATIVE_CONNECTION_ID__ host hooks; real WebView attachment is later
Exact commands, working directories, platform/runtime versions, and results:
  repo /Users/mpopov/projects/cha, macOS arm64, Apple clang 21.0.0, cmake 4.4.0, Node v26.9.0 / npm 11.19.1
  cmake --build --preset ninja --target cha_bridge_tests cha_app_tests cha_web_tests
  ./build/ninja/cha_bridge_tests  -> 11 passed
  ./build/ninja/cha_app_tests     -> 16 passed
  ./build/ninja/cha_web_tests --gtest_filter='WebWireFixtures.*:LiveSession.*:ApplicationRuntime.*' -> 89 passed
  npm --prefix webapp run check   -> 361 passed
  npm --prefix webapp run build   -> production build ok
Known failures, checks not run, and exact missing evidence:
  asan-ubsan / tsan not run
  npm --prefix webapp run e2e not run (HTTP baseline; not native proof)
  No real WKWebView/WebView2 user-flow in this block (explicitly out of scope)
  Windows host evidence still pending for later platform work
Inventory/coverage changes and remaining work:
  First-flow session operations bound on the common bridge and typed native client
  Settings/vault/audio/OAuth/native menus remain unavailable in native mode
  Platform hosts must call BridgeRouter from a non-UI thread and post take_delivery batches
Next unfinished numbered step if this block needs continuation:
  none for this block
```

Maintain concise rows for the operations/files/assertions touched by this block.
For the inventory block, enumerate the complete migration; for later blocks,
carry forward existing evidence and record the relevant updates.

| Operation/caller or source/test path | Retained behavior/result/errors | Native destination or deletion reason | Context/cancellation/lifetime | Verification and status |
|---|---|---|---|---|
| bridge.info | protocol_version 1, application_version, platform | BridgeRouter allowlist | no epoch | unit_bridge_protocol / BridgeRouterTest passed |
| app.bootstrap | ApplicationBootstrap DTO + context_epoch | app.bootstrap | no prior epoch; sets connection epoch | BridgeRouterTest passed |
| session.create/open/submit/stop/snapshot/close | existing CommandResult / OpenSessionSuccess / Snapshot DTOs and public errors | bound to Application | epoch checked at admit and before effects | BridgeRouterTest passed |
| session.subscribe/unsubscribe | SubscribeResult; monotonic seq 0 snapshot | owner enqueue; latest sub recorded first | stale subscribe cancelled; late unsub of old id ignored | SubscribeSnapshotAndAppendUseOneSequence passed |
| unmigrated ChaClient methods | invalid_argument, not HTTP | explicit native gap | n/a | nativeClient.test.ts passed |
| HTTP createChaClient / EventSource / path routes | unchanged | coexistence | n/a | App.test / events.test / ApplicationRuntime passed |

| Required flow/assertion | Common test evidence | macOS evidence | Windows evidence | Remaining limitation |
|---|---|---|---|---|
| Dispatcher malformed/unknown/stale/duplicate/one reply | cha_bridge_tests | n/a this block | n/a this block | no real WebView |
| Timeout then late completion without replay | CommandReply abandon + expire_timeouts | n/a | n/a | not measured under provider burst |
| Subscribe reply vs snapshot either order, gaps, newer snapshot | C++ wait across batches; nativeEvents.test.ts | n/a | n/a | host fragment nav untested here |
| Fake bridge + C++ wire fixtures | wireFixtures.test.ts, nativeClient.test.ts | n/a | n/a | |
| Native hash writes/back-forward/reload; HTTP path routes | route.test.ts, App hashchange listener | n/a | n/a | actual host fragment nav later |
| Schema/TS/frontend build | npm check 361, vite build | n/a | n/a | |

The final response must state what was implemented, why, what was actually tested,
and any unresolved limitation. If incomplete, give the exact next step and missing
prerequisite/evidence. A context limit or a mostly working platform is not success.

## Optional provenance

The [migration plan](plan.md) and [design proposal](redesign.md) explain the overall
sequence and original rationale. They are reference material, not additional
required instructions for executing this brief.
