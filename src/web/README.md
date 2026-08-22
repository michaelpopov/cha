# Web frontend boundary

Workspace characters select their providers in their own `character.toml`;
`host`, `port`, and the workspace path in the application directory's `app.toml`
remain the web server's listener and root settings. Web discovery has its own HTTP projection, including
Guest, Assistant, Entrance, and Welcome, but does not use terminal
presentation result types.

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and
live-session coordination, including the textual grammar accepted by the
browser's chat box. The composition root builds a `WorkspaceRuntime`, whose
current immutable generation contains a `WorkspaceDefinition` and matching
`SessionRepository`; routes take one generation per request, and the
`SessionOpener` opens every session — including the built-in Welcome — through
`open_session()` with the persona that session's forum configures. It depends on
core `SessionIdentity`, `SessionDescriptor`,
`OpenedSession`, `ControllerView`, and `ControllerUpdate`, but puts no
HTTP or protocol type in `cha_core`. Its permanent session-owner thread is the sole owner of
a `SessionController`; HTTP workers exchange only owning commands and results
with it.

A persona is a property of the session, not of the submitter. A session starts
from its forum's configured persona and `/!Name` changes it. `GET
/api/v1/bootstrap` returns the immutable discovery view, including stable IDs,
display summaries, built-ins, and Recent; it deliberately exposes no prompt
text. Each `ForumSummary` carries its `default_persona_id` and
`default_persona_display_name`, which is how the browser names the author it is
about to write as.

Bootstrap also carries the workspace persona roster as summaries, and `GET
/api/v1/personas/{id}` serves one persona's `PERSONA.md` the way `GET
/api/v1/characters/{id}` serves a character definition. That is discovery for
reading, not selection: neither endpoint takes part in attribution, and
`persona_markdown` is empty for a persona that configures no `PERSONA.md`.
Character detail also carries the character's current provider and style names
(null when a setting cannot be read), the lists of options that resolve, and
`writable`, which is false for the built-in Assistant. A provider option is only
an id and a label — never
host, model, or credential — so the response stays discovery-safe.

`PATCH /api/v1/characters/{id}` requires a provider name and takes an optional
style (`null` erases only the style), writes the file, and asks live sessions in
every forum containing that character to shut down with `reloading`. The server
does not reopen anything: the browser's existing stream-recovery ladder does that.
`reloading` is ranked above `browser_disconnected` in
`shutdown_reason_priority()`, or the reason never reaches the wire.

The fan-out runs over `LiveSessionManager::active_sessions()`, not over
`snapshot()`'s `running_sessions`. A session reads its definitions while it is
still Starting, so one that is opening when the save commits already holds the
old settings and has to be reloaded like any other; both `running_sessions` and
`lookup()` admit only Running actors, which is why that method exists and why it
returns actors rather than identities. The write commits before the fan-out, so
an actor that appears in neither is one that has yet to read the file at all.

`POST /api/v1/workspace/reload` is the equivalent whole-workspace operation. It
requires the same empty JSON body and origin policy as other mutations. It
first shuts every starting or running session down with `workspace_reloading`
and joins their owners, then loads and publishes the candidate generation. On
validation failure it returns `422 workspace_reload_failed` and leaves the
current generation alone. It does not reload `.env`. This is distinct from a
character-settings save, which re-reads only affected forum definitions at the
next session open and never publishes a workspace generation.

A submitted input body is exactly `{"text": "<text>"}`. Naming a persona is
rejected rather than ignored, so a client written against an older shape fails
visibly. `LiveSession` supplies the session's current persona ID from the
controller view, and `SessionController` resolves it against the workspace
roster, so a submitter still cannot choose who a message is attributed to. A
live session serves one browser connection at a time — the newest one, because
the reader moves between devices — and the persona changes
only through `/!Name`, which also saves the choice as the forum's default and
reloads the forum's live sessions so agent prompts carry that persona.

## Chat input grammar

The raw-input owner path recognizes optional leading character mentions and
the commands `/clear`, `/hide-on`, `/hide`, `/hide-off`, `/mcast`, `/info`,
`/characters` (`/agents` is a legacy alias), `/@Name`, `/!Name`,
`/style`, `/stop`, and `/exit`. Mentions and multicast recipient
handles remain unresolved until `SessionController` applies the forum's
authoritative character rules. While generation is active, only a bare
`/stop` is dispatched; other input remains in the browser editor.

This grammar is web policy, not a reusable core or terminal abstraction.
`/exit` explicitly requests that the `LiveSession` close the live web
session. `handle_text_input()` returns the same `CommandResult` completed back
to the HTTP request: it owns the controller's `ControllerUpdate`, `clear_input`,
and the internal `close_session` decision. JSON exposes only `clear_input` and
the update's optional notice.

## The live-session actor

`LiveSession` is one complete session actor. It owns its permanent
`std::thread`, the concrete `SessionController` that thread exclusively uses,
the bounded multi-producer `CommandQueue`, an `OwnerWakeSignal` (which
implements core's producer-only `WakeNotifier` contract and adds the
condition-variable wait consumed by the owner loop), its `SseMailbox`,
`BrowserConnectionState`, web presentation state, and one lifecycle record.
There is no second per-session object owning the loop, no adapter around the
controller, and no virtual output port around the mailbox.

The controller is constructed, called, shut down, and destroyed only on that
one thread. The actor calls `SessionOpener` from `owner_main()` and moves the
resulting `OpenedSession` into owner-thread-only state; that opener exists
because session construction combines application-owned model and repository
data, and it always returns the production-shaped result with a concrete
controller. HTTP-facing callers get only owning command results; the owner
thread alone reaches a controller and continues draining generation notifications
without a browser connection. It builds every full snapshot on demand: it
borrows a `ControllerView` and passes it straight to `to_snapshot()`, which
copies the descriptor, the view, and web presentation state into an owning
protocol `SessionSnapshot` before the borrow ends. The snapshot owns copies of
the core `TranscriptEntry` values, so there is no parallel web transcript model
and the SSE writer never borrows controller state or blocks the owner. The
actor keeps no snapshot or cursor cache and never compares two protocol values
to discover what changed: it consumes the controller's own classification.

Its lifecycle is one synchronized record guarded by a single mutex and
condition variable:

```text
                 open succeeds
Starting ------------------------------> Running
   |                                       |
   | open fails                            | idle/orphan, command, controller
   |                                       | failure, or shutdown request
   v                                       v
Finished <----------------------------- Stopping
```

A shutdown request that arrives while the opener is still running wins the
commit race: `Running` is never published and teardown proceeds straight to
`Finished`. The owner thread is the sole writer of the startup result; the
manager only sets the stop-request flag and wakes waiters, so an open timeout
is a waiter outcome that never cancels a shared startup. `Finished` is
published last, after the final drain, mailbox close, queued-command
replies, controller shutdown, controller destruction, and lease release —
which is what lets a same-identity actor start immediately afterwards.

For each update the owner thread applies the notice, then publishes a full
snapshot if presentation changed (notice lives only in a snapshot under the
current protocol), publishes nothing for `NoStateUpdate`, publishes a full
snapshot for `SnapshotRequired`, and otherwise offers the exact `TextAppend` to
the mailbox. The mailbox returns `AppendPublishResult::Accepted` when it can
represent the update exactly with its current base and pending payload, or
`SnapshotRequired` when it cannot — an unset or different base target, an empty
append, or an incompatible pending payload. Rejection leaves the mailbox's
pending work untouched and obliges the owner to project one fresh snapshot, so
no eager fallback snapshot is ever built for the common append path. Append
acceptance is an optimization boundary, not a correctness promise: mailbox
pressure may turn a controller-proven append into a full snapshot at any time.
Core `TextAppend` targets cross the boundary unchanged; the mailbox adds
sequence values only to payloads it actually stores. The actor's idempotent
owner-thread teardown drains a final snapshot for a bounded interval and
contains controller failures to that one session.

## The live-session collection

`LiveSessionManager` is the process-local liveness authority and owns nothing
per-session beyond the map. It is not a core session abstraction: it serializes
open requests by `SessionIdentity`, counts starting and stopping actors against
the configured bound, and holds each actor through a
`std::map<SessionIdentity, std::shared_ptr<LiveSession>>`. Its outcomes describe
only owner lifecycle; `LobbyRoutes` validates URL components, returns stable
open identities, and maps lifecycle failures to HTTP errors. It publishes only
running actors, and sweeps finished actors in two phases so joins occur outside
its mutex.

Ownership flows one way. An actor is inserted into the map before its owner
thread starts, and the thread captures a raw `LiveSession*` rather than a
shared pointer, so an owner can never destroy itself by releasing the last
reference and the map entry always outlives that raw pointer. No actor ever
calls back into the manager; the manager learns state only by taking an actor's
short lifecycle lock or waiting on its condition variable, and never waits,
opens storage, runs controller work, or joins while holding its own mutex.
Sweeping erases only actors that already published `Finished` and joins them
afterwards, which is bounded by invariant: from that publication to the thread
returning there is only non-blocking stack unwinding. A route handle may outlive
removal safely, because the thread is already joined and every call on a
finished actor returns the existing not-live result — which is also what lets
the same identity open a new actor immediately.

`SessionRoutes` resolves path-scoped `std::shared_ptr<LiveSession>` values and
uses their owner queue for snapshots and commands; it never reaches a controller
directly. It serves the session API and chunked SSE route. The SSE close
callback retains the actor so the callback's target stays alive even after the
manager has removed and joined it.
`SseMailbox` holds at most one immutable in-flight payload and one replaceable
pending payload; its writer is the HTTP thread, never the session owner. Each
stream begins with a fresh snapshot, then receives only snapshot or
target/sequence-aware append events plus comment heartbeats.
`BrowserConnectionState` is owner-thread-only state: it holds one SSE stream
per session, assigns an opaque server-local connection ID, and ignores stale
close notifications. CHA serves one reader who may move between devices, so a
connect never fails and never waits: it takes the session over and reports the
connection it displaced. Consulting a deadline there would make the reader wait
out an abandoned device's socket, which is the delay this design exists to
avoid, and ignoring the displaced device's late close is what keeps that
teardown from starting a deadline against the connection that replaced it. The
displaced stream ends with a `superseded` record so that browser parks instead
of reconnecting and taking the session straight back. An actor starts
disconnected, cancels its one deadline on stream acceptance, and on matching
close unloads at `idle_grace` or the absolute `orphan_limit` from that same
disconnection timestamp while generation is active.
`LobbyRoutes` is the HTTP boundary for bootstrap discovery, character detail,
stored-session discovery, create, rename, recoverable delete,
and manager-backed open/reattach. It validates route identifiers before either
the live-session map or session storage is consulted; creation reaches only
`SessionRepository`, while opening first asks the manager for a disk-free
reattach and otherwise strictly validates only the selected session's stored
metadata before a new open. `AssetHandler` separately owns the HTML/asset boundary and serves
the same client-routed shell at the root and session deep links.

Live rename is serialized through the actor's owner queue and republishes its
descriptor snapshot. Delete first acquires a manager maintenance reservation,
which blocks open and reattach for that identity, then requests the
`session_deleted` shutdown reason and waits under the configured deadline before
the repository moves the database into `deleted/` without replacement.
`configure_http_server()` owns the server-global request pool, read/write
timeouts, payload limit, and fallback error/exception handlers so route
installers cannot silently replace one another's policy. It does not restrict
the `Host` header: clients may reach the configured listener through a DNS
name, IP address, proxy, or other network path. JSON mutations that send an
`Origin` header must still use an origin authority equal to `Host`.
`ServerShutdownCoordinator` implements the bounded process shutdown sequence:
it waits for signal notification, sets the manager's stopping flag, stops HTTP
acceptance, wakes opening waiters, requests every live actor to stop, waits for
all owners under one absolute grace deadline rather than a fresh period per
actor, logs stuck session identities and forces the no-destructor `std::_Exit`
on expiry, then joins cpp-httplib's listener/request pool. A wedged owner is
never joined: `std::thread` and explicit join points are retained precisely
because a `std::jthread` destructor could hide an unbounded join after that
grace period, and a stop token cannot interrupt SQLite, filesystem calls, or a
model backend. Keeping that ordering in the coordinator makes the forced
path directly testable and prevents a stuck HTTP worker from suppressing it.
`ProcessShutdownSignal` is the portable signal bridge; its handler only records
`sig_atomic_t` state and normal code performs the shutdown work.
`web_main.cpp` is the composition root for the listener and one process-owned
`Providers`. Process shutdown stops HTTP admission, joins every live-session
owner, then calls `Providers::shutdown()` before diagnostic logging is closed.
Provider shutdown waits for request transport cleanup, so curl callbacks and
easy handles cannot outlive logging or process-owned provider state.
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
