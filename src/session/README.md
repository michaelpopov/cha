# Session layer

`session/` owns transcript behavior, durable journals and leases, character
selection, prompt construction, and the presentation order of generation
output. It has no HTTP or browser dependency.

## Controller ownership

`SessionController` retains ordered immutable character definitions and derives
one `CharacterRuntimeInfo` per character for `/characters`, `/info`, and style
reset. `ForumCharacters` is the mutable presentation copy: a session style
override changes only that copy, and reset restores the immutable configured
appearance.

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
| `forum_characters.*` | Character lookup, public descriptions, and style presentation. |
| `session_database.*` | SQLite schema, restore, and journal operations. |
| `session_repository.*`, `session_catalog.*` | Stored-session discovery and lifecycle. |
| `session_lease.*` | Cross-process companion-file lease. |

The controller view is borrowed and owner-thread-only. Workers never receive a
`TranscriptView`; provider input always owns its `ModelHistory` snapshot.
