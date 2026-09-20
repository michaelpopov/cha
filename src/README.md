# Native architecture

CHA's production composition root is the platform host plus `Application`.
macOS and Windows hosts load packaged React assets in a WebView and talk to C++
through the common bridge. There is no application HTTP listener.

## Dependency shape

```text
cha_macos / cha_windows -> cha_lib

cha_lib -> app / bridge / web / services / workspace / providers / characters / chat / session / util
cha_lib -> curl / sqlite / libuv / threads / toml++ / spdlog / nlohmann-json
```

One static library holds every production source. The layering below is a
directory convention, not a link-time boundary: read the table under
`Directories` for where a file belongs. `cha_lib` contains no WebView or
inbound HTTP types. Outbound provider HTTP/SSE and R2 transfer live in it
alongside everything else.

## Composition root

The native host opens `Application`, which owns the process-owned
`WorkspaceConfigStore`, session repository, live-session manager, providers,
credentials, audio, and vault maintenance. The store owns the selected database
lease and handle, one private root with materialized `workspace/` and temporary
`welcome/` children, the configuration mutex, publication, and cleanup.

Declaration order in the composition root also defines shutdown order: session
owners are released before the repository and provider supervisor, and
diagnostic logging remains available until teardown finishes.

## Directories

| Directory | Responsibility |
| --- | --- |
| `app/` | Application composition root and domain operations. |
| `bridge/` | Native request envelopes and the operation dispatcher. |
| `web/` | Chat-input grammar, API DTOs, live-session registry, and projection. |
| `services/` | Direct integrations with external services such as Fish Audio and R2. |
| `workspace/` | The loaded workspace model, built-ins, and the one controller-opening operation. |
| `session/` | Session storage, databases and leases, controller state, persistence, and character resolution. |
| `providers/` | Provider transport, request execution, cancellation, protocol decoding, and event delivery. |
| `characters/` | Request-owned character/provider values, identity validation, and model context. |
| `chat/` | Stable domain IDs plus presentation-neutral transcript records, validation, and live mutation. |
| `util/` | Domain-neutral text, path, environment, logging, queue, and thread helpers. |
| `../webapp/` | React browser application and its browser tests. |

Dependencies point downward through those responsibilities. Core layers never
import bridge or browser presentation types. Protocol serialization owns the
browser contract without introducing a second persistence model.

## Runtime ownership

One process-wide `SessionRuntime` thread owns every live session. That thread
exclusively owns the live controllers, their transcripts, journal mutation, and
provider event draining. Bridge threads submit commands to it and wait on a
`CommandReply`; runtime-produced state is copied into protocol snapshots or
append events and delivered through `SessionOutput`.

`Application` owns one process-wide `Providers` instance. Each
`SessionController` retains only request handles while it applies streamed
events and persists turn transitions; every provider request owns its own
worker, client, curl handle, cancellation state, and event queue. The runtime
thread never waits for provider cleanup during a Stop action or controller teardown.
`WorkspaceConfigStore` owns the one process-lifetime database lease.
`SessionRepository` receives explicit database, materialized-workspace, and
Welcome paths; it owns none of those outer resources. Each live controller owns
a separate SQLite journal connection scoped by its internal session key;
repository operations use short-lived connections.

Welcome is the sole built-in Entrance session. Its database lives under the
store's private `welcome/` child and is removed with that root. All persistent
sessions share the SQLite file selected by the external application config and
are addressed by stable forum and session IDs.

## Persistence and identity

The transcript is the source of presentation-neutral chat history. The session
journal persists typed turns and entries transactionally in SQLite. Stable IDs
are stored and used in requests; display names and labels are presentation data.
Opening a session resolves `(forum_id, session_id)` to an internal
`session_key`, validates the workspace database identity, and restores only
rows belonging to that key.

One immutable `Workspace` is published process-wide, while one independent
`SessionRepository` owns session-storage operations. `Workspace::load()` parses
the store's materialized physical root. Published values eagerly own their
parsed data; normal reads do not reopen materialized files. Process settings,
including diagnostic logging, come from the external application config.

The three narrow runtime mutations are serialized by the store. Each edits the
materialized candidate, validates it, replaces the complete small `config`
table in one SQLite transaction, and publishes only after commit. Other
configuration changes use offline export/edit/import and a process restart.
Session listings are read from SQLite per request, so newly created sessions
appear immediately.

## Build and test map

| Target | Purpose |
| --- | --- |
| `cha_lib` | All production sources: domain, storage, application operations, live sessions, DTOs, and the native request dispatcher. |
| `cha_macos_runtime` / `cha_windows_app` | Production desktop hosts. |
| `cha_tests` | Core, session, and workspace unit/component tests. |
| `cha_app_tests` | Application, live-session, protocol, and audio tests. |
| `cha_bridge_tests` | Native envelope and dispatcher tests. |
| `itest` | Live-provider integration tests for the retained core stack. |

The React UI has Vitest checks under `../webapp/`. Native host automation is
under `../tests/native/`.

## Detailed contracts

- [Workspace layer](workspace/README.md)
- [Provider execution](providers/README.md)
- [Character definitions and model context](characters/README.md)
- [Sessions and persistence](session/README.md)
- [Shared chat model](chat/README.md)
- [Native protocol and live sessions](web/README.md)
- [Utilities](util/README.md)
