# chaweb design brief

Status: proposed design, ready to guide implementation.

Last updated: 2026-07-31.

This document summarizes the intended behavior and public shape of `chaweb`.
The detailed design is available in [`web-design.md`](web-design.md).

## Overview

`chaweb` is a trusted-network web interface for browsing forums, opening stored
chat sessions, and interacting with agents.

It uses two kinds of processes:

- A **lobby process** serves the starting page, lists forums and sessions,
  creates sessions, and launches workers.
- A **session worker process** serves one active chat session and owns its
  controller, transcript, persistence, agent activity, and web API.

Each worker listens on an available port. After a worker is ready, the lobby
returns that port as JSON. The lobby page replaces the port in its current URL
through the browser URL API and navigates to the worker. Browser traffic then
goes directly to that worker; the lobby does not proxy chat requests or retain
session routing information.

Several sessions can be active at once because each has an independent worker.
A single stored session can be open in only one application process at a time.

The browser technology, component system, styling, and visual design are
defined separately.

## Main functionality

The lobby provides:

- Forum and stored-session discovery.
- Creation of stored sessions without opening them in the lobby.
- Launching a worker for a new or existing session.
- Returning a worker port only after the worker is ready.
- Retaining each ready worker's control-pipe endpoint so both processes can
  detect peer exit.

Workers are fire-and-forget processes. On POSIX, the lobby requests automatic
child reaping and starts workers with `posix_spawn()`. On Windows, it starts
workers with `CreateProcessW()` and `CREATE_NO_WINDOW`, then immediately closes
the returned native process handles. Session workers never open visible console
windows or inherit the lobby's console streams; lobby mode may use its normal
console. The lobby retains no worker PID or process handle and never waits for,
reaps, or force-terminates a worker. The private worker invocation receives the
workspace root explicitly so it loads the same application configuration and
session data as the lobby.

The session worker provides:

- A complete session snapshot for initial display or recovery.
- Submission of ordinary prompts, slash commands, and `@` addressing using the
  shared text-input grammar.
- Typed controls for Stop and stable-ID default-agent selection. Clear and
  off-record controls use the shared text-input grammar.
- Live transcript and generation updates through Server-Sent Events (SSE).
- Reconnection and state resynchronization.
- Disconnect-driven and automatic lifecycle management.

## Expected behavior

### Opening and creating sessions

The lobby launches a new worker for each open request. The worker acquires the
session, restores its state, starts its listener, and reports readiness. Only
then does the lobby return `{"port":<worker-port>}` to the browser for
navigation.

Creating a session stores it first and then launches its worker. If another
process opens the newly created session first, the session remains created and
the lobby reports that it could not be opened. It does not create a replacement
session automatically.

### Session exclusivity

Every stored session has a companion lock file. An operating-system lock held
on that file means the session is in use; the file's mere existence does not.

The lock applies to all frontends, including `cha`, `chacon`, and `chaweb`.
Opening an active session fails immediately with a clear “session already in
use” result. The operating system releases the lock if the owning process
exits or crashes.

### Browser usage

A worker supports one interactive browser page at a time. The first SSE stream
is accepted; another is rejected while it remains active. A rejected page
disables its controls and explains that the session is already open elsewhere.

This is a lightweight usage rule for a personal application, not an
authentication or browser-identity system.

Reloading or reconnecting may briefly race with cleanup of the previous SSE
connection. The browser retries for a short, bounded period. Each accepted
connection starts with a full current snapshot, so reconnecting does not depend
on replaying old events.

### Commands and updates

Commands use ordinary HTTP requests. Model output and session changes are sent
to the browser over SSE.

`POST /api/v1/input` accepts the supported text grammar shared with the
terminal frontends, including ordinary prompts, leading `@Name` addressing,
`/mcast`, `/clear`, `/hide-on`, `/hide`, `/hide-off`, `/stop`, and
default-agent selection.

Clear and off-record buttons submit `/clear`, `/hide-on`, `/hide`, and
`/hide-off` through the raw-input endpoint without replacing or clearing an
existing prompt draft. Stop remains a typed action so cancellation is
out-of-band from input submission. Default-agent selection remains typed and
uses a stable persona ID because valid multi-word display names cannot always
be represented by the whitespace-delimited `/@Name` command. Both typed
actions use the same authoritative session semantics as text commands.

The worker continues processing and persisting agent events while the browser
is disconnected or slow. If the browser misses updates, it replaces its local
state from a new snapshot.

### State consistency

Snapshots contain owning, presentation-oriented data rather than database or
internal C++ structures. They include session and forum information, personas,
the default agent, transcript entries, generation state with an optional stable
foreground request ID and streamed reasoning, the current notice, worker
coarse lifecycle phase, and the configured lobby port for a return link.

SSE uses only complete `snapshot` payloads and text `append` payloads. An
append targets an established answer entry or reasoning stream and carries its
UTF-8 byte length before the new text. A mismatch closes the old stream and
reconnects for a fresh snapshot. SSE heartbeats do not change session state.

### Worker lifetime

A worker remains tied to the lobby that created it. If the lobby shuts down or
crashes, its workers detect the lost control connection, stop their sessions,
release their locks, and exit.

After startup, neither side sends control messages. Both sides keep an
asynchronous read active only to detect EOF. Worker EOF lets the lobby close
its local endpoint and forget the control record. Lobby EOF makes the worker
responsible for orderly cleanup and exit. A defective worker that does not
react to EOF cannot be force-terminated by the lobby and may remain alive until
it exits or is stopped externally.

A worker may also stop because:

- Its browser-disconnection deadline expires.
- An accepted command misses its generous completion deadline.
- A fatal persistence or runtime failure occurs.
- The application receives a shutdown signal.

Browser absence uses no multi-state lifetime machine. At worker readiness or a
matching SSE close, the worker records `disconnected_since`. While disconnected
it applies one rule:

```text
deadline = is_generating() ? orphan_limit : idle_grace
```

Both limits are measured from `disconnected_since`; `orphan_limit` must be at
least `idle_grace`. The worker reevaluates the rule whenever generation state
changes. Active generation and persistence continue until the applicable
deadline so the page can reconnect. Lobby loss triggers shutdown immediately.

When orderly shutdown begins while SSE remains writable, the worker sends a
best-effort final snapshot with lifecycle `stopping` and a safe reason. A slow
browser cannot delay shutdown beyond the bounded final drain deadline.

The web interface has no close endpoint or Close control. Closing the tab is
observed only as SSE disconnection, which is indistinguishable from reload,
navigation, browser failure, network loss, or device suspension. The worker
therefore ends only after the disconnect lifetime rule permits it; tab closure
does not guarantee immediate release of the session lock.

## HTTP API structure

The API is versioned under `/api/v1`. The bundled browser and server are
released together, so the first version is not a stable third-party API.

### Lobby API

| Method and path | Purpose |
| --- | --- |
| `GET /health` | Report lobby liveness. |
| `GET /api/v1/forums` | List forums and display metadata. |
| `GET /api/v1/forums/{forum}/sessions` | List stored sessions in a forum. |
| `POST /api/v1/forums/{forum}/sessions` | Create a session, launch its worker, and return its ready port. |
| `POST /api/v1/forums/{forum}/sessions/{session}/open` | Launch a worker for an existing session and return its ready port. |

Forum and session identifiers are validated as application identifiers and are
never used directly as filesystem paths.

### Session-worker API

| Method and path | Purpose |
| --- | --- |
| `GET /health` | Report worker readiness, coarse lifecycle phase, and stream presence. |
| `GET /api/v1/session` | Return the complete current snapshot. |
| `POST /api/v1/input` | Submit one line through the shared input grammar. |
| `POST /api/v1/actions/stop` | Stop active generation. |
| `POST /api/v1/actions/default-agent` | Change the run-local default agent. |
| `GET /api/v1/events` | Open or reconnect the SSE update stream. |

Command responses use owning JSON values and may report whether the submitted
input should be cleared and an optional notice. They do not invent an
`applied` field that is absent from `SessionUpdate`. Snapshots are authoritative
for the page's current notice; a delayed HTTP response notice is request-scoped
and cannot overwrite newer snapshot state.

### SSE events

The update stream may emit:

| Event | Meaning |
| --- | --- |
| `snapshot` | Complete replacement state. |
| `append` | Text was appended to an established answer entry or reasoning stream. |

Every new SSE connection begins with `snapshot`. The service does not maintain
an event replay log. The writer holds at most one immutable in-flight payload
and one replaceable pending payload. Compatible appends merge; every structural
or ambiguous change collapses the pending state to a fresh snapshot. The owner
thread never waits for network output.

### Errors

Browser-facing errors contain a stable code and a safe message. Important
cases include:

- `409 Conflict` when a session is already in use.
- `409 Conflict` with `session_created_but_busy` when creation succeeded but
  the new session could not be opened.
- `409 Conflict` with `browser_stream_in_use` when another SSE stream is
  active.
- `503 Service Unavailable` with `worker_unresponsive` when an accepted command
  misses its generous completion deadline. Its outcome is unknown and the
  worker begins orderly shutdown.
- Validation errors for malformed input or unknown identifiers.
- Server errors for worker startup, listener, persistence, or fatal runtime
  failures.

If a command times out or the client disconnects after enqueue, the command may
still have applied. The browser resynchronizes from a snapshot and must not
automatically repeat non-idempotent commands. A completion that arrives after
the HTTP handler leaves is harmless because the completion state is owned
independently of that handler.

## Network and trust model

`chaweb` is intended for a trusted local network. It has no accounts,
authentication, authorization, built-in TLS termination, or Internet-facing
hardening. Any device that can reach the configured listeners can view and
operate sessions.

The lobby and workers bind to the configured interface. Workers serve their
chat page and API from the same origin. Successful create/open responses return
only a validated port. Browser code constructs worker and return-to-lobby URLs
with the URL API, preserving the hostname through which the browser reached the
application. The lobby does not emit permissive CORS headers.

Requests and connections have bounded sizes, queues, and timeouts. Mutating
requests require the expected content type and same-origin browser requests.
Transcript content, labels, notices, and provider errors are treated as
untrusted text by the browser.

## Code organization

Web code belongs in `src/ui/web/`, with `src/apps/web_main.cpp` acting as the
composition root. The main responsibilities are:

- Lobby routes and browser assets.
- Platform process spawning with explicit descriptor/handle inheritance and
  no retained process identity.
- Worker launch, startup handshake, and control-pipe EOF tracking.
- Session-worker routes, SSE, and lifecycle.
- The single-owner session runtime and command queue.
- Session snapshots, request/response types, errors, and event serialization.
- Cross-frontend session locking in the session layer.

The web implementation should build as a separate `cha_web` library linked by
the `chaweb` application. Web transport dependencies remain outside the core
session, agent, and transcript layers.

## Scope limits

The initial design does not provide authentication, collaborative sessions,
multiple viewers of one session, worker rediscovery after lobby restart, a
single public port for all workers, an integrated session-switching chat shell,
or a stable public client API.

Browser implementation technology and detailed visual, accessibility, and
mobile interaction design remain separate work.
