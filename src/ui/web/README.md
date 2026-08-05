# Web frontend boundary

Workspace characters in the web frontend inherit the shared `[provider]`
configuration in `app.toml`; the top-level `host` and `port` remain this web
server's listener settings. The web frontend does not gain the terminal
Guest/Entrance/Welcome environment or terminal slash-navigation commands.

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination. It depends on core `SessionIdentity`, `SessionDescriptor`,
`OpenedSession`, `SessionState`, append proof, and `SessionChange`, but puts no
web type in `cha_core`. Its permanent session-owner thread is the sole owner of
a `SessionController`; HTTP workers exchange only owning commands and results
with it.

The lobby selects a workspace persona before a forum and session. `GET
/api/v1/personas` returns the roster's stable IDs and display names in roster
order; it deliberately exposes no prompt text. A submitted input body is
exactly `{"persona": "<id>", "text": "<text>"}`. The owner-thread adapter passes
that ID through the shared text-input funnel, where `SessionController` resolves
it against the roster captured when the session opened. Thus persona selection is
attribution, not authentication, and an unknown ID starts no batch. A live
session still serves one browser connection at a time; changing personas happens
between prompts on that same shared session.

`SessionRegistry` owns one permanent thread per runtime and invokes the
threadless `WebSessionRuntime` on it. The runtime owns a condition-variable wake
notifier and a bounded multi-producer command queue. HTTP-facing callers get
only owning command results; the owner thread alone reaches a controller and
continues draining agent notifications without a browser connection. The
runtime obtains an owning, transport-neutral `SessionState` from the controller
and explicitly projects it, with the descriptor and web presentation state,
into a protocol `SessionSnapshot`. The projection consumes state by move, so the
SSE writer never borrows controller state or blocks the owner. Web DTOs remain
API compatibility values, not a second transcript or generation domain model.
Core append candidates cross that seam without a sequence number; the mailbox
assigns sequence values only to payloads it actually stores. Its idempotent owner-thread teardown
uses registry hooks only for lifecycle notifications, drains a final snapshot
for a bounded interval, and contains controller failures to that runtime.
`SessionRegistry` is a web host registry and the sole process-local liveness authority.
It is not a core session abstraction: it serializes
open requests by `SessionIdentity`, counts starting and stopping entries against
the configured bound, and owns the owner threads. Its outcomes describe only
owner lifecycle; `LobbyRoutes` validates URL components, builds redirect paths,
and maps lifecycle failures to HTTP errors. It publishes only running runtimes
through owning `SessionHandle` values, and sweeps finished entries in two phases
so joins and runtime destruction occur outside its mutex.
`SessionRoutes` resolves path-scoped live handles and uses their owner queue
for snapshots and commands; it never reaches a controller directly. It serves
the minimal chat/not-open page boundary, session API, and chunked SSE route.
`SseMailbox` holds at most one immutable in-flight payload and one replaceable
pending payload; its writer is the HTTP thread, never the session owner. Each
stream begins with a fresh snapshot, then receives only snapshot or
target/sequence-aware append events plus comment heartbeats.
`BrowserConnectionState` is owner-thread-only state: it accepts one SSE stream
per session, assigns an opaque server-local connection ID, and ignores stale
close notifications. A runtime starts disconnected, cancels its one deadline
on stream acceptance, and on matching close unloads at `idle_grace` or the
absolute `orphan_limit` from that same disconnection timestamp while generation
is active. The later browser page must enable controls only after the accepted
stream's initial snapshot; a conflict retries briefly, then displays the
already-open message. An append target or sequence mismatch closes that stream
and uses bounded reconnect for a fresh snapshot rather than racing a REST
snapshot against the old stream.
`LobbyRoutes` is the HTTP boundary for stored-session discovery, create-only,
and registry-backed open/reattach. It validates route identifiers before either
the registry or session storage is consulted; creation reaches only the shared,
immutable `Workspace`, while opening first asks the registry for a disk-free
reattach and directly reads only the selected session's stored metadata before
a new open. `AssetHandler`
separately owns the root HTML/asset boundary and currently serves only a
framework-neutral lobby placeholder. `configure_http_server()` owns the
server-global allowed-host check, request pool, read/write timeouts, payload
limit, and fallback error/exception handlers so route installers cannot
silently replace one another's policy. The allowed-host check runs before
routing and admits only the configured listener authority; loopback listeners
also admit the equivalent `localhost`, IPv4, and IPv6 loopback authorities.
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
destroys the registry and `Workspace` in an inner scope so their teardown
records still reach the sink, and shuts logging down only afterwards, which is
the order Section 19.1 step 7 requires.
Server-scoped log records use `web server`, while session-scoped records always
carry `forum_id` and `session_id`; neither form includes prompt, answer,
transcript, provider-message, or credential text. Route exceptions are recorded
with their message so a 500 stays diagnosable, and the response itself always
uses the common error envelope, which carries no exception detail.
Generation logging treats an active request-ID change as a terminal event for
the old request followed by a start event for the new request, covering
multicast handoffs that never pass through an inactive snapshot.

The complete Block 8 verification matrix is specified in
[`docs/web-fix-plan.md`](../../../docs/web-fix-plan.md#12-block-8-documentation-and-final-verification).
