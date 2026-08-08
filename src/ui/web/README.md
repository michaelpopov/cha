# Web frontend boundary

Workspace characters in the web frontend inherit the shared `[provider]`
configuration in `workspace.toml`; `host`, `port`, and the workspace path live in
the application directory's `app.toml` and remain the web server's listener and
root settings. Web discovery has its own HTTP projection, including
Guest, Assistant, Entrance, and Welcome, but does not use terminal
presentation result types.

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination, including the textual grammar accepted by the browser's
chat box. The composition root builds one immutable `WorkspaceModel` and one
`SessionRepository`; routes read discovery from the model and storage from the
repository, and the registry callback opens every session — including the
built-in Welcome — through `open_session()` with the model's Guest-plus-
workspace persona roster. It depends on core `SessionIdentity`, `SessionDescriptor`,
`OpenedSession`, `ControllerView`, and `ControllerUpdate`, but puts no
HTTP or protocol type in `cha_core`. Its permanent session-owner thread is the sole owner of
a `SessionController`; HTTP workers exchange only owning commands and results
with it.

Personas are workspace-wide authors, not forum or session members. `GET
/api/v1/bootstrap` returns the immutable discovery view, including stable IDs,
display summaries, built-ins, and Recent; it deliberately exposes no prompt
text. A submitted input body is exactly `{"persona": "<id>", "text":
"<text>"}`. The HTTP boundary accepts only that stable ID; the browser cannot
supply the persisted display name. The owning command carries the ID to
`SessionController`, which resolves it against the process-wide effective roster
captured when the web session opened. That roster is Guest plus every workspace
persona, independent of forum membership, so Guest and every configured persona
can author in any forum. Persona selection is attribution, not authentication.
A live session still serves one browser
connection at a time; changing personas happens between prompts on that same
shared session.

## Chat input grammar

The raw-input owner path recognizes optional leading character mentions and
the commands `/clear`, `/hide-on`, `/hide`, `/hide-off`, `/mcast`, `/info`,
`/agents`, `/@Name`, `/stop`, and `/exit`. Mentions and multicast recipient
handles remain unresolved until `SessionController` applies the forum's
authoritative character rules. While generation is active, only a bare
`/stop` is dispatched; other input remains in the browser editor.

This grammar is web policy, not a reusable core or terminal abstraction.
`/exit` explicitly requests that `WebSessionRuntime` close the live web
session. `handle_text_input()` returns the same `CommandResult` completed back
to the HTTP request: it owns the controller's `ControllerUpdate`, `clear_input`,
and the internal `close_session` decision. JSON exposes only `clear_input` and
the update's optional notice.

`SessionRegistry` owns one permanent thread per runtime and invokes the
threadless `WebSessionRuntime` on it. The runtime owns an `OwnerWakeSignal`,
which implements core's producer-only `WakeNotifier` contract and adds the
condition-variable wait consumed by the owner loop, plus a bounded
multi-producer command queue. HTTP-facing callers get
only owning command results; the owner thread alone reaches a controller and
continues draining agent notifications without a browser connection. The
runtime builds every full snapshot on demand: it borrows a `ControllerView` and
passes it straight to `to_snapshot()`, which copies the descriptor, the view,
and web presentation state into an owning protocol `SessionSnapshot` before the
borrow ends. The snapshot owns copies of the core `TranscriptEntry` values, so
there is no parallel web transcript model and the SSE writer never borrows
controller state or blocks the owner. The runtime keeps no snapshot or cursor
cache and never compares two protocol values to discover what changed: it
consumes the controller's own classification.

For each update the owner thread applies the notice, then publishes a full
snapshot if presentation changed (notice lives only in a snapshot under the
current protocol), publishes nothing for `NoStateUpdate`, publishes a full
snapshot for `SnapshotRequired`, and otherwise offers the exact `TextAppend` to
the snapshot sink. A sink returns `AppendPublishResult::Accepted` when it can
represent the update exactly with its current base and pending payload, or
`SnapshotRequired` when it cannot — an unset or different base target, an empty
append, or an incompatible pending payload. Rejection leaves the sink's pending
work untouched and obliges the owner to project one fresh snapshot, so no eager
fallback snapshot is ever built for the common append path. Append acceptance is
an optimization boundary, not a correctness promise: mailbox pressure may turn a
controller-proven append into a full snapshot at any time. Core `TextAppend`
targets cross the boundary unchanged; the mailbox adds sequence values only to
payloads it actually stores. Its idempotent owner-thread teardown
uses registry hooks only for lifecycle notifications, drains a final snapshot
for a bounded interval, and contains controller failures to that runtime.
`SessionRegistry` is a web host registry and the sole process-local liveness authority.
It is not a core session abstraction: it serializes
open requests by `SessionIdentity`, counts starting and stopping entries against
the configured bound, and owns the owner threads. Its outcomes describe only
owner lifecycle; `LobbyRoutes` validates URL components, returns stable open
identities,
and maps lifecycle failures to HTTP errors. It publishes only running runtimes
through owning `SessionHandle` values, and sweeps finished entries in two phases
so joins and runtime destruction occur outside its mutex.
`SessionRoutes` resolves path-scoped live handles and uses their owner queue
for snapshots and commands; it never reaches a controller directly. It serves
the session API and chunked SSE route.
`SseMailbox` holds at most one immutable in-flight payload and one replaceable
pending payload; its writer is the HTTP thread, never the session owner. Each
stream begins with a fresh snapshot, then receives only snapshot or
target/sequence-aware append events plus comment heartbeats.
`BrowserConnectionState` is owner-thread-only state: it accepts one SSE stream
per session, assigns an opaque server-local connection ID, and ignores stale
close notifications. A runtime starts disconnected, cancels its one deadline
on stream acceptance, and on matching close unloads at `idle_grace` or the
absolute `orphan_limit` from that same disconnection timestamp while generation
is active.
`LobbyRoutes` is the HTTP boundary for bootstrap discovery, character detail, stored-session discovery, create-only,
and registry-backed open/reattach. It validates route identifiers before either
the registry or session storage is consulted; creation reaches only
`SessionRepository`, while opening first asks the registry for a disk-free
reattach and otherwise strictly validates only the selected session's stored
metadata before a new open. `AssetHandler` separately owns the HTML/asset boundary and serves
the same client-routed shell at the root and session deep links.
`configure_http_server()` owns the
server-global allowed-host check, request pool, read/write timeouts, payload
limit, and fallback error/exception handlers so route installers cannot
silently replace one another's policy. The allowed-host check runs before
routing and admits only the configured listener authority; loopback listeners
also admit the equivalent `localhost`, IPv4, and IPv6 loopback authorities.
A wildcard listener (`0.0.0.0` or `::`) cannot name the authority a browser
will send, so it instead admits any `Host` on the listener port whose host part
is an IP address literal or `localhost`, and rejects every DNS name. Refusing
names is what denies a hostile page the easiest route to this API: pointing its
own domain at the machine and letting the browser send that domain as `Host`.
The `Origin`-must-equal-`Host` rule for JSON mutations is unaffected, because a
real browser sends the address the user typed in both headers.
`ServerShutdownCoordinator` implements the bounded process shutdown sequence:
it waits for signal notification, sets the registry stopping flag, stops HTTP
acceptance, wakes opening waiters, requests published runtimes to stop, joins
owners under one grace deadline, logs stuck session identities and forces the
no-destructor exit on expiry, then joins cpp-httplib's listener/request pool.
Keeping that ordering in the coordinator makes the forced path directly
testable and prevents a stuck HTTP worker from suppressing it.
`ProcessShutdownSignal` is the portable signal bridge; its handler only records
`sig_atomic_t` state and normal code performs the shutdown work.
`web_main.cpp` is only the composition root for the one server listener. It
destroys the registry, repository, and model in an inner scope so their teardown
records still reach the sink, and shuts logging down only afterwards, which is
the order Section 19.1 step 7 requires.
Server-scoped log records use `web server`, while session-scoped records always
carry `forum_id` and `session_id`; neither form includes prompt, answer,
transcript, provider-message, or credential text. Route exceptions are recorded
with their message so a 500 stays diagnosable, and the response itself always
uses the common error envelope, which carries no exception detail.
Generation logging retains only whether a generation was active and its request
ID, not a snapshot. It treats an active request-ID change as a terminal event
for the old request followed by a start event for the new request, covering
multicast handoffs that never pass through an inactive snapshot; the terminal
status is read from the freshly projected transcript.
