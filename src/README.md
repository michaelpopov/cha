# Native architecture

CHA has one production composition root: `web_main.cpp`. It builds the
`chaweb` server, which serves the browser client and coordinates live sessions
through HTTP and server-sent events.

## Dependency shape

```text
chaweb_app -> cha_web
cha_web    -> cha_core

cha_core -> application / agents / chat / session / util
cha_web  -> cpp-httplib / nlohmann-json
cha_core -> curl / sqlite / libuv / threads / toml++ / spdlog
```

`cha_core` contains no `web/` or executable sources. The browser chat grammar is
part of `cha_web`, including its editor-clearing and `/exit` lifecycle policy.
HTTP workers never access a `SessionController` directly; they submit bounded
commands to a registry-owned session thread.

## Composition root

`web_main.cpp` owns only process wiring and top-level error handling. It loads
configuration, assembles the workspace model, session repository, live-session
manager, routes, assets, HTTP listener, and shutdown coordinator, then starts
the server. Reusable application policy remains in `application/`, and HTTP/SSE
policy remains in `web/`.

Declaration order in the composition root also defines shutdown order: the HTTP
server and live-session owners are released before the repository and workspace
model, and diagnostic logging remains available until their teardown finishes.

## Directories

| Directory | Responsibility |
| --- | --- |
| `web/` | HTTP/SSE transport, chat-input grammar, API DTOs, live-session registry, mailboxes, and lifecycle policy. |
| `application/` | The immutable loaded workspace model, built-ins, and the one controller-opening operation. |
| `session/` | Session storage, databases and leases, controller state, persistence, and character resolution. |
| `agents/` | Character configuration, provider clients, completion context, execution, cancellation, and event delivery. |
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

Each `SessionController` owns a fixed-width worker pool and a `CompletionExecutor`.
Provider work runs on those workers and publishes deltas plus exactly one final
event. The owner thread applies events and persists turn transitions. A stored
session lease remains owned until the controller and journal are destroyed.

Welcome is the sole built-in Entrance session. `SessionRepository` creates and
owns its process-local database directory and removes it on destruction, while
ordinary sessions are addressed by stable forum and session IDs in workspace
storage.

## Persistence and identity

The transcript is the source of presentation-neutral chat history. The session
journal persists typed turns and entries transactionally in SQLite. Stable IDs
are stored and used in routes; display names and labels are presentation data.
Opening a session validates that its database metadata matches its forum,
filename, and schema before restoring it.

One `WorkspaceModel` holds every static workspace value for the server lifetime,
and both discovery and newly opened sessions read from it, so the browser and a
controller cannot disagree. Session listings are read from storage per request,
so newly created sessions appear without a restart. Changes to personas,
characters, forums, prompts, or provider configuration require restarting
`chaweb`.

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

- [Application layer](application/README.md)
- [Completion execution](agents/README.md)
- [Sessions and persistence](session/README.md)
- [Shared chat model](chat/README.md)
- [Web frontend](web/README.md)
- [Utilities](util/README.md)
