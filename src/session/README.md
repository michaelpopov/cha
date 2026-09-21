# Session layer

`session/` owns controller behavior, character selection, prompt construction,
the presentation order of generation output, Markdown formatting, and the
best-effort filesystem mirror. Durable storage lives in `storage/`. This layer
has no HTTP or browser dependency.

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

The Stop action cancels every request, drops non-foreground handles immediately, and
continues to drain only the durable foreground queue until its terminal event
is persisted. It then clears session-visible busy state without waiting for
the cancelled workers to unregister. Controller destruction similarly cancels
and releases handles without waiting; it synchronously closes the current
durable turn using existing partial-response rules.

## Source map

| Source | Responsibility |
| --- | --- |
| `session_controller.*` | Controller commands, durable turn transitions, request-handle presentation, and shutdown. |
| `controller_update.*` / `controller_view.h` | Presentation-neutral controller updates and borrowed views. |
| `generation_status.h` | Presentation-neutral generation phase and progress values. |
| `opened_session.h` | Owning result returned after controller construction. |
| `session_open.*` | Combines workspace state, prepared storage, providers, and notifier into a controller. |
| `session_markdown.*` | Markdown rendering and export filenames. |
| `session_mirror.*` | Best-effort projection of stored sessions into Markdown files. |

The controller view is borrowed and owner-thread-only. Workers never receive a
`TranscriptView`; provider input always owns its `ModelHistory` snapshot.

This directory may depend on `characters/`, `chat/`, `providers/`, `storage/`,
`workspace/`, and `util/`. It does not depend on `app/`, `bridge/`, `media/`,
or `runtime/`.

Tests live in `tests/session/`.
