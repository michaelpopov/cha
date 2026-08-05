# Web frontend boundary

Workspace characters in the web frontend inherit the shared `[provider]`
configuration in `app.toml`; the top-level `host` and `port` remain this web
server's listener settings. Web discovery has its own HTTP projection, including
Guest, Assistant, Entrance, and Welcome, but does not use terminal
slash-navigation commands or their presentation results.

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination. The composition root builds one immutable `WebDiscovery`
and one process-wide `WelcomeStorage`; the registry uses them to open the
built-in Welcome session and gives every web-opened session the Guest-plus-
workspace persona roster. It depends on core `SessionIdentity`, `SessionDescriptor`,
`OpenedSession`, `SessionState`, append proof, and `SessionChange`, but puts no
HTTP or protocol type in `cha_core`. Its permanent session-owner thread is the sole owner of
a `SessionController`; HTTP workers exchange only owning commands and results
with it.

Personas are workspace-wide authors, not forum or session members. `GET
/api/v1/bootstrap` returns the immutable discovery view, including stable IDs,
display summaries, built-ins, and Recent; it deliberately exposes no prompt
text. A submitted input body is exactly `{"persona": "<id>", "text":
"<text>"}`. For every submission the HTTP boundary resolves that ID against the
application-wide workspace-plus-built-ins discovery view and constructs an owning,
server-trusted author identity. Only that resolved identity crosses the
owner-thread queue; the browser cannot supply its display name.

`SessionController` records the supplied author identity but owns no persona
roster and performs no persona-membership check. Built-in Guest can therefore
author a message in an ordinary workspace forum. Persona selection is
attribution, not authentication. A live session still serves one browser
connection at a time; changing personas happens between prompts on that same
shared session.

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
the registry or session storage is consulted; creation reaches only the shared,
immutable `Workspace`, while opening first asks the registry for a disk-free
reattach and directly reads only the selected session's stored metadata before
a new open. `AssetHandler` separately owns the HTML/asset boundary and serves
the same client-routed shell at the root and session deep links.
`configure_http_server()` owns the
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
