# Block 4 — Integrate both hosts and prove lifetime behavior

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** waiting for evidence. **Environment:** macOS and Windows/WebView2 plus common native tests.

## Objective and scope

Integrate the shared app/bridge/frontend into both native hosts and prove the real first chat flow, document recovery, bounded presentation, actor lifetime, and asynchronous shutdown.

Use the existing Swift and WebView2 hosts. Bind the initial chat flow only; do not use this block to migrate the remaining workspace/settings/media operations. Do not delete legacy deployment before package parity.

## Prerequisites to verify before editing

- Listener-free Application, selected actors, asynchronous commands, retirement,
  ordered/coalesced output, and shared serializer fixtures are implemented and
  tested without a WebView.
- The common bridge has fixed envelopes/version checking, bounded admission,
  connection/batch acknowledgements, subscription replacement, and stale-work checks.
- The injected native ChaClient/event connector, frontend receiver/projection,
  and fragment routing pass frontend tests without HTTP fallback.
- Both platform feasibility modes and native test runners have passed local
  origin, microphone/audio, trust/CSP, and safe message probe checks.

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

Start with the platform-feasibility work's probes/runners,
`packaging/macos/main.swift`, `packaging/macos/runtime_bridge.{h,cpp}`,
`packaging/windows/main.cpp`, and the common APIs from the application/session and common bridge/frontend implementations.

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

1. Replace probe messaging with the shared Application/dispatcher, first on
   macOS and then Windows. Extend the existing C boundary with explicit string,
   callback, and handle ownership. Windows currently also compiles the runtime
   bridge under `packaging/macos`; move shared code once if useful, never fork it.
2. Preserve native protected-vault prompting, early errors, and runtime checks.
   Start native Application and packaged assets without port/cookie setup; keep
   temporary HTTP launch explicitly available for migration verification.
3. Marshal WebKit/WebView2 calls to the UI thread and application work off it.
   Install the receiver before React, validate real sender/main-document scope,
   and block unauthorized navigation/popups. Use structured JSON/arguments for
   delivery, never interpolated executable strings.
4. Bind each real document connection, bounded delivery, and acknowledgement.
   WebView2 post success is not consumption. Invalidate old deliveries before
   replacement and guard queued callbacks after view disposal. Hash navigation
   does not create a new document connection.
5. Wire WK process termination and WebView2 `ProcessFailed` to document recovery
   while preserving Application/selected actors. Limit repeated failure to an
   actionable reload/restart state instead of an endless retry loop.
6. Extend each runner to actual UI create/open/submit/visible-stream/Stop/reload/
   resubscribe/restart flows with temporary vaults and the deterministic provider.
   Assert transcript and persisted state; page-load success is insufficient.
7. Exercise subscribe/snapshot races, dirty snapshot replacement, gaps/duplicates,
   delayed subscription cleanup, and StrictMode. Compare final projection with
   authoritative state using deterministic scheduling controls, not arbitrary sleeps.
8. Stall JS consumption while providers produce bursts. Assert bounded pending
   bytes/calls/drains, one renderer delivery outstanding, continued owner/persistence
   progress, and responsive Stop/disposal under ordinary-work saturation.
9. Reload with requests pending, reuse numeric IDs in the new connection, and
   deliver old replies/acks. Test actual renderer failure and view disposal on
   both hosts; assert no replay, cross-document resolution, or destroyed-object
   access. Recheck retirement/lease release after repeated navigation.
10. Quit with generation, queued work, providers, and stalled presentation active.
    Use one overall grace, no UI-thread joins, persistence after state settles,
    and safe provider/store destruction. Preserve controlled failure when a worker
    cannot finish; never detach work that still accesses destroyed objects.
11. Measure long-conversation snapshot capture/serialization/transfer/render and
    Stop latency. Fix demonstrated unbounded work; do not add truncation,
    pagination, or timers without evidence. Run focused race/sanitizer checks and
    record both host results, keeping automation hooks out of shipping builds.

## Verification for this block

- Drive actual create/open/submit/visible streaming/Stop/reload/resubscribe/restart
  flows through WKWebView and WebView2 with temporary vaults and the deterministic
  provider. Assert transcript and persisted state rather than page load.
- Test real renderer process failure/recreation and view disposal; inject queued
  old callbacks, reused numeric IDs in a new connection, stale replies/acks, and
  delayed subscriptions. Verify no replay, cross-document completion, or use-after-free.
- Stall JS consumption while providers burst. Assert one outstanding delivery,
  bounded pending bytes/calls/UI drains, continued persistence, and responsive
  Stop/disposal under ordinary request saturation.
- Exercise owner subscribe/snapshot/append races, StrictMode cleanup, repeated
  navigation beyond the actor cap, reselection, retirement, and journal lease release.
- Quit with generation, provider work, queued commands, and stalled presentation.
  Verify one overall grace, no UI-thread waits, final persistence after settled
  state, safe teardown order, and the existing controlled stuck-worker policy.
- Run focused sanitizers on supported toolchains and measure reproducible long-
  conversation capture/serialize/transfer/render costs and Stop latency. Keep
  test hooks/CDP separate from shipping builds.

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

Both hosts pass the complete first native flow and lifetime/
saturation/shutdown tests without a CHA listener. Actors, persistence, and quit
never wait for renderer acknowledgement. Subsequent blocks extend these same
checks to their new maintenance, provider, and media work.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide ABI/host ownership, runner evidence, reproducible races, measurements,
sanitizer results, and remaining operation capabilities to migrate.

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
Status: waiting for evidence
Starting and resulting revision/checkpoint:
  start 9ad12e7 (Block 3). Result is uncommitted working tree on redesign.
Files changed/moved and actual new APIs/targets:
  packaging/macos/runtime_bridge.{h,cpp} — native Application+BridgeRouter,
    pump thread, connection/message/ack/delivery callback, async shutdown
  packaging/macos/native_bridge.swift — WK receiver, document connection,
    callAsyncJavaScript delivery with JSON arguments
  packaging/macos/main.swift — native default (cha://app), --http coexistence,
    process-terminate recovery, UI-thread-free shutdown join
  packaging/windows/main.cpp — native default (https://app.cha.local), --http,
    ProcessFailed recovery, PostMessage delivery, async shutdown
  tests/native/macos/test_host.swift + run.sh — flow/reload/stall/quit/renderer-fail
  tests/native/{prepare_test_vault,unit_runtime_bridge}.cpp
  cha_native_runtime_tests, cha_prepare_test_vault
  webapp/src/api/nativeBridge.ts — drain __CHA_NATIVE_QUEUE__ before React
Implemented behavior and key ownership/contract decisions:
  Packaged hosts start Application + BridgeRouter with no listener (port 0)
  Temporary HTTP remains --http; database menus stay HTTP-only until Block 5
  One pump thread runs tasks/timeouts/take_delivery; UI only posts JSON
  Document connection is native-owned; hash navigation keeps it; reload/process
    failure replaces it before the new document runs
  Delivery uses structured JS arguments / PostWebMessageAsJson, never interpolation
  Renderer failures retry 3 times then show a quit/reopen error
  Shutdown: request_shutdown on UI, join_shutdown off UI, one 10s grace
Prerequisites verified and evidence used:
  Block 3: cha_bridge_tests / cha_app_tests / native client already present
  Block 1: cha://app WKURLSchemeHandler and https://app.cha.local mapping reused
Temporary compatibility code and when it can be removed:
  --http / cha_runtime_create(..., http_mode=1) until HTTP removal
  Feasibility probe receiver/scripts until Block 9/10
  Database import/export/R2 C APIs require HTTP runtime (Block 5)
Exact commands, working directories, platform/runtime versions, and results:
  repo /Users/mpopov/projects/cha, macOS 26.7 arm64, Apple clang 21.0.0, cmake 4.4.0
  ./build/ninja/cha_bridge_tests --gtest_filter='BridgeRouterTest.*:CommandReply.*' -> 12 passed
  ./build/ninja/cha_native_runtime_tests -> 2 passed
  ./build/ninja/cha_app_tests -> 16 passed
  ApplicationConfigTest.Native* -> 2 passed
  snapshot capture/serialize: 9152 bytes, 4 ms (gtest properties)
  asan-ubsan same BridgeRouter+NativeRuntime tests passed
  tsan BridgeRouter stall/reload/shutdown + NativeRuntime passed
  npm --prefix webapp run check -> 362 passed; npm run build ok
  tests/native/macos/run.sh pass|flow|reload|stall|quit|renderer-fail passed
    (runtime_listener=none, loader=WKURLSchemeHandler cha://app)
Known failures, checks not run, and exact missing evidence:
  Windows/WebView2 host not executed (no Windows machine in this session)
  WebContent XPC is not a child of the test host (ppid/pgid 1); renderer-fail
    recovered via the same invalidate+reload path, not a confirmed SIGKILL
  npm --prefix webapp run e2e not run (HTTP baseline, not native proof)
  Database menus disabled in native mode until maintenance work
Inventory/coverage changes and remaining work:
  First native chat flow is bound on both packaged hosts
  Settings/vault/audio/OAuth/native menus remain unavailable in native mode
  Windows runner: build CHA.exe, then tests/native/windows/run.ps1 plus a
    --user-data native launch against cha_prepare_test_vault output
Next unfinished numbered step if this block needs continuation:
  Run the Windows WebView2 first-flow/lifetime suite on a Windows machine
```

Maintain concise rows for the operations/files/assertions touched by this block.
For the inventory block, enumerate the complete migration; for later blocks,
carry forward existing evidence and record the relevant updates.

| Operation/caller or source/test path | Retained behavior/result/errors | Native destination or deletion reason | Context/cancellation/lifetime | Verification and status |
|---|---|---|---|---|
| cha_runtime_create http_mode=0 | Application+BridgeRouter, port 0 | replaces HTTP listener default | pump thread; destroy joins owners | NativeRuntimeTest passed |
| cha_runtime_create http_mode=1 | previous loopback+cookie runtime | `--http` coexistence | existing ApplicationRuntime shutdown | code path retained, not re-run as product default |
| session.create/open/submit/stop | CommandResult; submit != generation | BridgeRouter allowlist | owner queue, async reply | C++ + macOS WK flow passed |
| document connection | native-owned view-N | hash keeps it; reload/process replace | late ack/reply dropped | reload restored transcript; C++ reuse-id passed |
| delivery + ack | one outstanding batch | JS finally-ack / PostWebMessage | no UI wait, no replay | stall host + StalledAck C++ passed |
| quit | one 10s grace, no UI join | Application::join_shutdown | actors do not wait for ack | ShutdownDoesNotWait + run.sh quit passed |
| database menus | import/export/R2 | still HTTP-only | n/a | disabled in native; Block 5 |

| Required flow/assertion | Common test evidence | macOS evidence | Windows evidence | Remaining limitation |
|---|---|---|---|---|
| create/open/submit/stream/Stop | NativeRuntimeTest, BridgeRouterTest | run.sh flow PASS, listener=none | implemented, not run | Windows host pending |
| reload/resubscribe, no replay | NewConnectionReusesIds | run.sh reload restored transcript | implemented, not run | |
| stall JS, one outstanding, Stop | StalledAckKeepsOneOutstandingDelivery | run.sh stall PASS | implemented, not run | host stall flushes after 4 held acks |
| renderer failure / view recovery | n/a | terminate delegate + fallback reload PASS | ProcessFailed wired, not run | WebContent XPC not SIGKILL-proven |
| quit with work, no UI join | ShutdownDoesNotWaitForRendererAck | run.sh quit PASS | async WM shutdown wired | |
| snapshot cost / sanitizers | 9152 B / 4 ms; asan+tsan passed | n/a | n/a | not a long-conversation corpus |

The final response must state what was implemented, why, what was actually tested,
and any unresolved limitation. If incomplete, give the exact next step and missing
prerequisite/evidence. A context limit or a mostly working platform is not success.

## Optional provenance

The [migration plan](plan.md) and [design proposal](redesign.md) explain the overall
sequence and original rationale. They are reference material, not additional
required instructions for executing this brief.
