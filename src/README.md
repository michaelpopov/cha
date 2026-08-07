# Native architecture

CHA has one production composition root: `apps/web_main.cpp`. It builds the
`chaweb` server, which serves the browser client and coordinates live sessions
through HTTP and server-sent events.

## Dependency shape

```text
chaweb_app -> cha_web
cha_web    -> cha_ui_text -> cha_core
cha_web    -> cha_core

cha_core -> application / agents / session / transcript / util
cha_web  -> cpp-httplib / nlohmann-json
cha_core -> curl / sqlite / libuv / threads / toml++ / spdlog
```

`cha_core` contains no `ui/` or `apps/` sources. `cha_ui_text` contains only the
controller-level input grammar used by the web runtime. HTTP workers never
access a `SessionController` directly; they submit bounded commands to a
registry-owned session thread.

## Directories

| Directory | Responsibility |
| --- | --- |
| `apps/` | Executable wiring, process signals, startup, and top-level failures. |
| `ui/web/` | HTTP/SSE transport, API DTOs, live-session registry, mailboxes, and lifecycle policy. |
| `ui/text/` | Slash commands, mentions, multicast parsing, and controller input dispatch. |
| `application/` | Immutable discovery, built-ins, effective personas, workspace inventory, and Welcome storage. |
| `session/` | Workspace loading, session databases and leases, controller state, persistence, and character resolution. |
| `agents/` | Character configuration, provider clients, completion context, execution, cancellation, and event delivery. |
| `transcript/` | Presentation-neutral transcript records, validation, and live mutation. |
| `util/` | Domain-neutral text, path, environment, logging, queue, and thread helpers. |
| `resources/webapp/` | React browser application and its browser tests. |

Dependencies point downward through those responsibilities. Core layers never
import HTTP or browser presentation types. Protocol DTOs are explicit
projections from core state, not a second persistence model.

## Runtime ownership

`SessionRegistry` owns one thread per live session. That thread exclusively
owns its controller, live transcript, journal mutation, and provider event
draining. HTTP threads communicate through `CommandQueue`; owner-produced state
is copied into protocol snapshots or append events and delivered through an
`SseMailbox`.

Each `SessionController` owns a fixed-width worker pool and an `AgentRegistry`.
Provider work runs on those workers and publishes deltas plus exactly one final
event. The owner thread applies events and persists turn transitions. A stored
session lease remains owned until the controller and journal are destroyed.

Welcome is the sole built-in Entrance session. `WelcomeStorage` owns its
process-local database directory, while ordinary sessions are addressed by
stable forum and session IDs in workspace storage.

## Persistence and identity

The transcript is the source of presentation-neutral chat history. The session
journal persists typed turns and entries transactionally in SQLite. Stable IDs
are stored and used in routes; display names and labels are presentation data.
Opening a session validates that its database metadata matches its forum,
filename, and schema before restoring it.

Workspace discovery is immutable for the server lifetime. Session listings are
read from storage per request, so newly created sessions appear without a
restart. Changes to personas, characters, forums, or provider configuration
require restarting `chaweb`.

## Build and test map

| Target | Purpose |
| --- | --- |
| `cha_core` | Domain and application-discovery implementation. |
| `cha_ui_text` | Text input grammar used by the web runtime. |
| `cha_web` | HTTP/SSE frontend. |
| `chaweb_app` (`chaweb`) | Production server executable. |
| `cha_tests` | Core, session, application, and text-grammar unit/component tests. |
| `cha_web_tests` | Web route, protocol, registry, runtime, and SSE tests. |
| `cha_web_stress_tests` | Concurrent live-session stress tests. |
| `cha_web_process_tests` | Real-process HTTP, SSE, restart, and shutdown tests. |
| `itest` | Live-provider integration tests for the retained core stack. |

The browser has Vitest checks and Playwright development/production flows under
`resources/webapp/`.

## Detailed contracts

- [Application discovery](application/README.md)
- [Agent execution](agents/README.md)
- [Sessions and persistence](session/README.md)
- [Transcript model](transcript/README.md)
- [Web frontend](ui/web/README.md)
- [Text input grammar](ui/text/README.md)
- [Entry point](apps/README.md)
- [Utilities](util/README.md)
