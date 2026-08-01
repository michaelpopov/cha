# Web frontend boundary

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination. It may depend on `session/` presentation values but does
not put web types in `cha_core`. A future session runtime is the sole owner of a
`SessionController`; HTTP workers exchange only owning web values with it.

`WebSessionRuntime` owns one permanent thread, a condition-variable wake
notifier, and a bounded multi-producer command queue. HTTP-facing callers get
only owning command results; the owner thread alone reaches a controller and
continues draining agent notifications without a browser connection. It copies
controller state into owning, presentation-neutral snapshots and publishes
them through a small abstract sink, so the SSE writer never borrows controller
state or blocks the owner. Append candidates cross that seam without a sequence
number; the mailbox assigns sequence values only to payloads
it actually stores. Its idempotent owner-thread teardown
uses registry hooks only for lifecycle notifications, drains a final snapshot
for a bounded interval, and contains controller failures to that runtime.
`SessionRegistry` is the sole process-local liveness authority. It serializes
open requests by validated forum/session identity, counts starting and stopping
entries against the configured bound, and owns the owner threads. It publishes
only running runtimes through owning `SessionHandle` values, and sweeps finished
entries in two phases so joins and runtime destruction occur outside its mutex.
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
reattach and validates stored metadata only before a new open. `AssetHandler`
separately owns the root HTML/asset boundary and currently serves only a
framework-neutral lobby placeholder. `configure_http_server()` owns the
server-global request pool, read/write timeouts, payload limit, and fallback error/exception
handlers so route installers cannot silently replace one another's policy.
`ServerShutdownCoordinator` implements the bounded process shutdown sequence:
it waits for signal notification, sets the registry stopping flag, stops HTTP
acceptance, wakes opening waiters, requests published runtimes to stop, joins
owners under one grace deadline, logs stuck session identities and forces the
no-destructor exit on expiry, then joins cpp-httplib's listener/request pool.
Keeping that ordering in the coordinator makes the forced path directly
testable and prevents a stuck HTTP worker from suppressing it.
`ProcessShutdownSignal` is the portable signal bridge; its handler only records
`sig_atomic_t` state and normal code performs the shutdown work.
`web_main.cpp` is only the composition root for the one server listener.
Server-scoped log records use `web server`, while session-scoped records always
carry `forum_id` and `session_id`; neither form includes prompt, answer,
transcript, provider-message, or credential text.
