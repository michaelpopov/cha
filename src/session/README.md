# Session layer

`session/` owns transcript behavior and schema, durable journals, character
selection, prompt construction, and the presentation order of generation
output. It has no HTTP or browser dependency.

All persistent sessions share the SQLite file selected by top-level `--data`.
Public `(forum_id, session_id)` identities resolve to internal `session_key` values;
every restore and journal statement is scoped by that key. The top-level
configuration store owns the database lease and private root. The repository
receives explicit database, materialized-workspace, and Welcome paths and owns
none of them; it uses short-lived connections. Each live controller owns its
own long-lived journal connection, and write transactions begin with `BEGIN
IMMEDIATE`.

## Controller ownership

Production `SessionController` instances retain only workspace IDs and mutable
session state. Roster, persona, provider, and style lookups come from the
current `Workspace`, which also resolves forum character handles. Session style
overrides live in the controller and are applied only when character metadata
is copied for presentation or a request. Controller tests publish small
filesystem workspaces and exercise this same data path.

The controller borrows the process-owned `Providers` and holds a shared wake
notifier. During a generation it owns only ordered `ProviderRequest` handles,
a foreground index, and presentation/cancellation state. It never owns a
provider thread, client, curl handle, or provider event queue.

For a normal prompt, the controller snapshots model history, commits the
durable foreground turn, then calls `Providers::make_request()`. For multicast,
it creates one shared history snapshot and one request per target before
committing the first turn; all requests start independently, while completed
later targets remain buffered until their ordered foreground turn is active.
Operational request failures are terminal events, so a committed turn is never
stranded.

`/stop` cancels every request, drops non-foreground handles immediately, and
continues to drain only the durable foreground queue until its terminal event
is persisted. It then clears session-visible busy state without waiting for
the cancelled workers to unregister. Controller destruction similarly cancels
and releases handles without waiting; it synchronously closes the current
durable turn using existing partial-response rules.

## Source map

| Source | Responsibility |
| --- | --- |
| `session_controller.*` | Controller commands, durable turn transitions, request-handle presentation, and shutdown. |
| `workspace_session_database.*` | Workspace schema, validation, WAL initialization, and checkpointing. |
| `session_database.*` | Session-key-scoped restore and journal operations. |
| `session_repository.*` | SQL-backed listing, creation, rename, archival, history, and preparation. |
| `session_storage_layout.*` | Import-only, path-based detection of legacy per-session databases. |
| `session_lease.*` | Portable companion-file lease used by the top-level store and offline transfers. |

The controller view is borrowed and owner-thread-only. Workers never receive a
`TranscriptView`; provider input always owns its `ModelHistory` snapshot.
