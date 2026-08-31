# Native architecture

CHA has one production composition root: `web_main.cpp`. It builds the
`chaweb` server, which serves the browser client and coordinates live sessions
through HTTP and server-sent events.

## Dependency shape

```text
chaweb_app -> cha_web
cha_web    -> cha_core

cha_core -> workspace / providers / characters / chat / session / util
cha_web  -> cpp-httplib / nlohmann-json
cha_core -> curl / sqlite / libuv / threads / toml++ / spdlog
```

`cha_core` contains no `web/` or executable sources. The browser chat grammar is
part of `cha_web`, including its editor-clearing and `/exit` lifecycle policy.
HTTP workers never access a `SessionController` directly; they submit bounded
commands to a registry-owned session thread.

## Composition root

`web_main.cpp` owns only process wiring and top-level error handling. It loads
and publishes the workspace, constructs the process-owned session repository,
live-session manager, routes, assets, HTTP listener, and shutdown coordinator,
then starts the server. Reusable workspace policy remains in `workspace/`, and
HTTP/SSE policy remains in `web/`.

Declaration order in the composition root also defines shutdown order: the HTTP
server and live-session owners are released before the repository and provider
supervisor, and diagnostic logging remains available until teardown finishes.

## Directories

| Directory | Responsibility |
| --- | --- |
| `web/` | HTTP/SSE transport, chat-input grammar, API DTOs, live-session registry, mailboxes, and lifecycle policy. |
| `workspace/` | The loaded workspace model, built-ins, and the one controller-opening operation. |
| `session/` | Session storage, databases and leases, controller state, persistence, and character resolution. |
| `providers/` | Provider transport, request execution, cancellation, protocol decoding, and event delivery. |
| `characters/` | Request-owned character/provider values, identity validation, and model context. |
| `chat/` | Stable domain IDs plus presentation-neutral transcript records, validation, and live mutation. |
| `util/` | Domain-neutral text, path, environment, logging, queue, and thread helpers. |
| `../webapp/` | React browser application and its browser tests. |

Dependencies point downward through those responsibilities. Core layers never
import HTTP or browser presentation types. Web serialization owns the browser
contract without introducing a second persistence model.

## Runtime ownership

One `LiveSession` actor owns one thread per live session. That thread
exclusively owns its controller, live transcript, journal mutation, and
provider event draining. HTTP threads communicate through `CommandQueue`; owner-produced state
is copied into protocol snapshots or append events and delivered through an
`SseMailbox`.

`web_main.cpp` owns one process-wide `Providers` instance. Each
`SessionController` retains only request handles while it applies streamed
events and persists turn transitions; every provider request owns its own
worker, client, curl handle, cancellation state, and event queue. The owner
thread never waits for provider cleanup during `/stop` or controller teardown.
`SessionRepository` owns one process-lifetime lease for the workspace database.
Each live actor owns a separate SQLite journal connection scoped by its
internal session key; repository operations use short-lived connections.

Welcome is the sole built-in Entrance session. `SessionRepository` creates a
private temporary file outside the workspace with the same schema and journal
path as persistent sessions, then removes it on destruction. All persistent
sessions share `workspace/workspace.sqlite3` and are addressed by stable forum
and session IDs.

## Persistence and identity

The transcript is the source of presentation-neutral chat history. The session
journal persists typed turns and entries transactionally in SQLite. Stable IDs
are stored and used in routes; display names and labels are presentation data.
Opening a session resolves `(forum_id, session_id)` to an internal
`session_key`, validates the workspace database identity, and restores only
rows belonging to that key.

One immutable `Workspace` is published process-wide, while one independent
`SessionRepository` owns session-storage operations. `POST
/api/v1/workspace/reload` first shuts down and joins every live session,
checkpoints the database, backs up the workspace, then loads, validates,
synchronizes, and atomically publishes one replacement. A failed candidate
leaves the current workspace published. Disk configuration edits become
visible only after a successful reload. Session listings are read from SQLite
per request, so newly created sessions appear without a reload.

## Build and test map

| Target | Purpose |
| --- | --- |
| `cha_core` | Domain, workspace model, session storage, and session opening. |
| `cha_web` | HTTP/SSE frontend. |
| `chaweb_app` (`chaweb`) | Production server executable. |
| `cha_tests` | Core, session, and application unit/component tests. |
| `cha_web_tests` | Web input-grammar, route, protocol, registry, runtime, and SSE tests. |
| `cha_web_stress_tests` | Concurrent live-session stress tests. |
| `cha_web_process_tests` | Real-process HTTP, SSE, restart, and shutdown tests. |
| `itest` | Live-provider integration tests for the retained core stack. |

The browser has Vitest checks and Playwright development/production flows under
`../webapp/`.

## Detailed contracts

- [Workspace layer](workspace/README.md)
- [Provider execution](providers/README.md)
- [Character definitions and model context](characters/README.md)
- [Sessions and persistence](session/README.md)
- [Shared chat model](chat/README.md)
- [Web frontend](web/README.md)
- [Utilities](util/README.md)
