# chaweb design brief

Status: proposed design, ready to guide implementation.

Last updated: 2026-07-31.

This document summarizes the intended behavior and public shape of `chaweb`.
The detailed design is available in [`web-design.md`](web-design.md).

## Overview

`chaweb` is a trusted-network web interface for browsing forums, opening stored
chat sessions, and interacting with agents.

It runs as one server process with one configured listener and one browser
origin. The lobby is the server's root page and routes, not a separate process.
Each live session has an independent runtime and a permanent owner thread that
exclusively owns its `SessionController`, transcript, journal, wake notifier,
browser connection state, and SSE mailbox. The controller continues to own its
session-local agent workers.

A process-wide session registry maps `forum/session` identities to live
runtimes. It serializes opening and unloading, prevents duplicate in-process
controllers, and makes an already-live session reachable again. Several
different sessions can be live concurrently, but each stored session can be
open in only one application process at a time.

Every page and API is served from the same host and port. Session pages use
stable paths such as `/s/{forum}/{session}/`; opening returns that path and the
browser navigates without changing origin or reconstructing a URL.

The browser technology, component system, styling, and visual design are
defined separately.

## Main functionality

The lobby provides:

- Forum and stored-session discovery, including advisory live-session marking.
- Creation of a stored session without opening it.
- Explicit opening of a new or existing stored session.
- Reattachment to a session already live in this server process.
- Same-origin navigation to the session's stable path after it is ready.

A live session provides:

- A complete session snapshot for initial display or recovery.
- Submission of ordinary prompts, slash commands, and `@` addressing through
  the shared text-input grammar.
- Typed controls for Stop and stable-ID default-agent selection. Clear and
  off-record controls use the shared text-input grammar.
- Live transcript and generation updates through Server-Sent Events (SSE).
- Reconnection and authoritative state resynchronization.
- Continued agent-event processing and persistence while the browser is slow or
  disconnected.
- Disconnect-driven unloading and session-local fatal-error containment.

## Expected behavior

### Creating and opening sessions

Creating and opening are separate lobby operations. Creation atomically
publishes a stored session and returns `201 Created` with its identity. It does
not initialize providers, acquire a session lease, construct a controller, or
start a live runtime.

The page keeps the returned identity and then submits an ordinary open request.
If opening fails, the session remains stored and visible in the listing; the
page may retry the open but must not repeat creation. This ensures that no
failure leaves behind a session whose identity was never returned to the
browser.

Opening goes through the session registry:

- A `running` entry returns its existing session path.
- A `starting` entry shares the same startup result with all waiters.
- A `stopping` entry reports a conflict until teardown finishes.
- An absent entry reserves capacity and starts one owner thread.

The owner thread acquires the cross-process lease, restores the session, and
constructs the controller before publishing the runtime as `running`. An open
deadline bounds each HTTP request, not the underlying open; a timed-out open may
still complete and be found by a later request.

Navigating directly to a session URL never opens it. If its runtime is no longer
live, the page explains that the session is not open and links back to the
lobby.

### Session exclusivity

Every stored session has a deterministic companion lock file. An exclusive
operating-system lock held on that file means the session is in use; the file's
mere existence does not.

The lease applies to `cha`, `chacon`, and `chaweb`. It is acquired before
restoring session state, held through controller shutdown, and released
automatically if the process exits or crashes. A held lease makes another
process fail immediately with a clear `session_busy` result.

Within `chaweb`, the registry is the first line of exclusivity. It inserts a
`starting` entry before slow open work begins, so concurrent requests for the
same session can never construct two runtimes. The operating-system lease is the
backstop against other processes.

### Thread ownership and registry lifetime

HTTP request threads have no session affinity and never access a controller
directly. They resolve an owning session handle, enqueue an owning typed command,
wake the session's owner loop, and wait within a bounded completion deadline.
Borrowed transcript views, spans, and domain references never cross the owner
thread boundary.

The registry tracks `starting`, `running`, and `stopping` entries. Every entry
counts against the configured session limit because all three states still own
or are acquiring significant resources. A session handle keeps a runtime alive
for an in-flight request even after registry removal.

Finished entries are reclaimed in two phases: the registry removes them under
its mutex, then joins their threads and releases runtime references after
unlocking. No controller call, socket write, shutdown, thread join, or runtime
destruction occurs while the registry mutex is held.

The number of registry entries is bounded. The cpp-httplib request pool is sized
to that limit plus headroom for commands, snapshots, lobby requests, and assets,
because each connected SSE stream occupies one request thread.

### Browser usage

One interactive browser page per live session is the supported model. The first
SSE stream is accepted; another is rejected with `browser_stream_in_use` while
the first remains active. A rejected page disables its controls and explains
that the session is already open elsewhere.

This is a lightweight usage rule for a personal application, not an
authentication or browser-identity system. REST requests carry no attachment
IDs, page IDs, epochs, or browser-storage credentials.

The page consumes SSE with `fetch` and a streaming reader rather than
`EventSource`, allowing it to read a rejected stream's HTTP status and error
body and to own reconnection behavior. Reloading or reconnecting may briefly
race with cleanup of the previous stream, so the page retries for a short,
bounded interval. Every accepted connection starts with a full snapshot.

### Commands and updates

Commands use ordinary HTTP requests. Model output and session changes are sent
to the browser over the session's SSE stream.

The raw-input route accepts the supported grammar shared with the terminal
frontends, including ordinary prompts, leading `@Name` addressing, escaped
`@@`, `/mcast`, `/clear`, `/hide-on`, `/hide`, `/hide-off`, `/info`, `/agents`,
`/stop`, and `/@Name` default-agent selection.

Clear and off-record buttons submit their exact grammar strings without
replacing or clearing an existing prompt draft. Stop remains a typed action so
cancellation is independent of editor input. Default-agent selection remains
typed and uses a stable persona ID because valid multi-word display names cannot
always be represented by the whitespace-delimited `/@Name` command.

A command accepted by the queue has an owning completion object. If its response
deadline expires, the server returns `command_timeout` but does not cancel the
command or stop the session. The outcome is unknown, so the browser must not
retry automatically. In contrast, `command_queue_full`, `session_not_live`, and
`server_stopping` prove that a rejected or drained command did not execute.

Command responses may contain `clear_input` and an optional request-scoped
notice. They do not invent an `applied` field. The snapshot stream is the only
source of the page's current notice and all other session state, so a delayed
HTTP response cannot overwrite a newer snapshot.

### State consistency and SSE

Snapshots contain owning, presentation-neutral data rather than database or
borrowed C++ structures. They include forum and session identity, personas, the
default agent, transcript entries, generation state with a stable request ID and
streamed reasoning when active, the current notice, and coarse lifecycle state.
They contain no host, port, or lobby address; return-to-lobby is the path `/`.

SSE has exactly two state-bearing event types:

| Event | Meaning |
| --- | --- |
| `snapshot` | Complete replacement state, sent on connection and every structural change. |
| `append` | Text appended to an established answer entry or reasoning stream. |

An append identifies its answer-entry or reasoning target and carries `text`
plus a per-target `seq` counter. A snapshot resets the target counter to zero.
If the target or sequence does not match, the browser discards the stream and
uses the ordinary reconnect path to obtain a fresh snapshot. The sequence is
not a byte offset, avoiding UTF-8 versus JavaScript UTF-16 length mismatches.

The SSE writer may hold one immutable in-flight payload, while the per-session
mailbox holds at most one replaceable pending payload. Compatible appends merge;
structural or ambiguous changes replace pending state with a current snapshot.
The owner thread never waits for network output, and no presentation backlog is
retained while disconnected.

SSE comment heartbeats help detect dead idle connections. Socket writes use a
bounded lack-of-progress timeout so a browser that stops reading cannot occupy a
request thread or the session's stream slot indefinitely. No event IDs, replay
log, or `Last-Event-ID` behavior is used.

### Session lifetime and failures

When a runtime becomes `running`, it starts disconnected and records
`disconnected_since`. Accepting its SSE stream clears that timestamp. Closure of
the matching stream records a new timestamp; duplicate or stale close callbacks
cannot detach a newer stream because connection IDs are session-local.

While disconnected, one rule determines unloading:

```text
deadline = is_generating() ? orphan_limit : idle_grace
if now - disconnected_since >= deadline: begin_shutdown()
```

Both limits are absolute durations from `disconnected_since`, and
`orphan_limit` is at least `idle_grace`. Generation and persistence continue
while disconnected until the applicable deadline. A reconnect before shutdown
begins cancels the deadline and receives a fresh snapshot.

The web interface has no close endpoint or Close control. Tab closure, reload,
navigation, browser failure, network loss, and device suspension all appear as
SSE disconnection, so closing a tab does not promise immediate lease release.

A thrown fatal session error is caught at the owner-thread boundary. That
session publishes a best-effort stopping snapshot, shuts down its controller,
releases its lease, and unloads; other live sessions continue. Undefined
behavior, `std::terminate`, `std::abort`, and unrecoverable heap exhaustion remain
process-fatal.

An owner thread that stops responding cannot be killed safely in process. Its
session remains live and leased, and later commands time out until `chaweb` is
restarted. Process shutdown has a bounded grace period and exits immediately if
such a thread cannot be joined, ensuring that restart remains available.

## HTTP API structure

The bundled browser and server are released together, so the first API version
is not a stable third-party contract.

### Lobby API

| Method and path | Purpose |
| --- | --- |
| `GET /` | Serve the lobby page. |
| `GET /health` | Report server liveness and the live-session count. |
| `GET /api/v1/forums` | List forums and display metadata. |
| `GET /api/v1/forums/{forum}/sessions` | List stored sessions and mark which are live. |
| `POST /api/v1/forums/{forum}/sessions` | Create a stored session and return its identity without opening it. |
| `POST /api/v1/forums/{forum}/sessions/{session}/open` | Open or reattach to a session and return its path. |

Forum and session identifiers contain only RFC 3986 unreserved ASCII characters
(letters, digits, `-`, `.`, `_`, and `~`), excluding the complete names `.` and
`..`. They are validated as application identifiers and are never treated as
filesystem paths. Display names and session labels remain unrestricted
presentation text. Creation returns an owning session summary; opening returns
`{"path":"/s/{forum}/{session}/"}` only after the runtime is published as
`running`.

### Session API

| Method and path | Purpose |
| --- | --- |
| `GET /s/{forum}/{session}/` | Serve the live session's chat page. |
| `GET /s/{forum}/{session}/api/v1/session` | Return the complete current snapshot. |
| `POST /s/{forum}/{session}/api/v1/input` | Submit one line through the shared input grammar. |
| `POST /s/{forum}/{session}/api/v1/actions/stop` | Request cancellation of active generation. |
| `POST /s/{forum}/{session}/api/v1/actions/default-agent` | Change the run-local default agent. |
| `GET /s/{forum}/{session}/api/v1/events` | Open or reconnect the SSE update stream. |

There is no per-session health or status route. A non-live page route serves the
not-open page; non-live API and SSE requests return `session_not_live`.

### Responses and errors

Creation succeeds with `201 Created` and the session identity:

```json
{"id":"2026-07-31-14-02-11-session","label":"Notes"}
```

Opening succeeds with a same-origin path:

```json
{"path":"/s/{forum}/{session}/"}
```

Every JSON error response has one shape:

```json
{"error":{"code":"session_busy","message":"That session is open in another program."}}
```

Important stable errors include:

- `session_busy` when another process holds the session lease.
- `session_stopping` while an earlier runtime is tearing down.
- `session_limit_reached` when the registry has no capacity.
- `session_open_timeout` when an open request's deadline expires.
- `server_stopping` when process shutdown prevents an open or command.
- `session_not_live` when a session route cannot resolve a running runtime.
- `browser_stream_in_use` when another SSE stream is active.
- `command_queue_full` when a command was not admitted and is safe to retry.
- `command_timeout` when an accepted command has an unknown outcome and must not
  be retried automatically.
- Transport errors such as `not_found`, `bad_request`, `body_too_large`,
  `forbidden_origin`, and `internal_error`.

Messages are presentation-safe and never expose internal paths, credentials, or
exception details. Browser behavior depends on codes and status, never English
message text.

## Network and trust model

`chaweb` is intended for a trusted local network. It has no accounts,
authentication, authorization, built-in TLS termination, or Internet-facing
hardening. Any device that can reach the configured listener can view and
operate sessions.

The configured host and port form the only listener. Lobby pages, session pages,
assets, REST routes, and SSE streams share that origin. Successful opens return
only a path, so loopback, LAN-address, and mDNS access all preserve the authority
the browser originally used. No permissive CORS headers are emitted.

Mutating requests require the expected content type. Requests carrying an
`Origin` header must match their own `Host`; failures return
`forbidden_origin`. This is an accidental cross-site request check, not an
authentication boundary, and the design explicitly does not resist DNS
rebinding.

Requests and connections have bounded bodies, prompts, headers, queues, and
timeouts. Transcript content, streamed reasoning, labels, notices, and provider
errors are untrusted text and must not be inserted as executable markup without
an explicit sanitization boundary.

## Process shutdown

On shutdown, the server sets a registry-wide stopping flag, rejects new opens,
stops accepting requests, and wakes requests waiting on session startup. An
owner thread reaching its startup commit point observes that flag and tears down
instead of publishing `running`.

Every running session is asked to execute its idempotent owner-thread shutdown
sequence. Accepted commands already executing complete; queued commands that
have not begun are failed without execution. Sessions send a best-effort final
stopping snapshot, end SSE, shut down their controller, release their lease, and
finish.

The server joins all owner threads under one bounded grace period. On expiry it
logs the sessions that did not finish and exits without running destructors. On
ordinary completion it joins the HTTP request pool and destroys the registry,
workspace, and logging resources. No live state survives a process restart.

## Code organization

Web code belongs in `src/ui/web/`, with `src/apps/web_main.cpp` as the composition
root. The main responsibilities are:

- Lobby routes, browser assets, forum/session listing, create, and open.
- A process-wide session registry with lifecycle states, capacity accounting,
  startup coordination, owning session handles, and finished-entry sweeping.
- Path-scoped session routes and SSE connection handling.
- A per-session owner runtime, wake notifier, command queue, controller,
  snapshot builder, SSE mailbox, containment boundary, and shutdown coordinator.
- Owning web request, response, snapshot, error, and SSE payload types.
- Cross-frontend `SessionLease` support in the session layer.

The web implementation should build as a separate `cha_web` library linked by
the `chaweb` application. It links `cha_core` and cpp-httplib while keeping web
transport dependencies out of the core session, agent, transcript, and terminal
frontend code.

## Scope limits

The initial design does not provide authentication, collaborative sessions,
multiple simultaneous viewers or writers for one session, direct-URL opening,
live-session recovery after a process restart, an explicit web close operation,
an integrated session-switching chat shell, Internet-facing hardening, or a
stable public client API.

Browser implementation technology and detailed visual, accessibility, and
mobile interaction design remain separate work.
