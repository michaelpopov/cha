# Workspace layer

`workspace/` owns the immutable in-memory view of the workspace directory and
the operation that opens a session from it. It contains no HTTP transport,
SQLite ownership, or live-session switching policy.

| Component | Responsibility |
| --- | --- |
| `Workspace` | Load and validate workspace settings, providers, styles, personas, characters, forums, prompts, and built-ins. |
| `getws()` / `loadws()` | Read or atomically replace the current published `Workspace`. |
| `open_session()` | Combine the current `Workspace` with prepared session storage and construct a `SessionController`. |
| `builtins` | Reserved built-in IDs and Welcome constants. |

`Workspace::load()` reads the directory tree once and builds a complete
candidate. It resolves character and forum prompts, provider and style
selections, forum membership and defaults, descriptions, labels, and the
Guest, Assistant, and Entrance data. Invalid referenced
configuration fails the load. `loadws()` publishes only a successfully built
candidate, so readers see either the previous workspace or the complete new
one.

Workspace values are the only process-wide copy of workspace configuration.
Callers hold the `shared_ptr` returned by `getws()` while using references into
it. Sessions retain only their IDs, transcript/database key, generation
state, and style overrides. At generation start, a request receives an owned
immutable copy of the exact character prompt and provider settings it will
execute; an in-flight request therefore cannot change halfway through.

`SessionRepository` is independent and process-owned. It owns the lease on
`workspace/workspace.sqlite3`, synchronizes configured forum IDs into that
database, and uses short-lived connections for storage operations. SQLite
connections, the lease, and Welcome's private temporary database are live
state and are not part of `Workspace`.

Configuration writes use `Workspace::write_character_settings()`,
`write_forum_default_character()`, and `write_forum_default_persona()`. Each
write validates its IDs, writes a uniquely named temporary file, and renames it
over the destination. A successful default write is followed by `loadws()` so
the published values match disk.

`POST /api/v1/workspace/reload` first stops live sessions, checkpoints the WAL,
creates the backup, loads and validates one candidate, synchronizes forum rows,
and publishes that candidate. Failure leaves the previous `Workspace`
published. Reload does not reload `.env`.

This directory may depend on `session/`, `characters/`, `chat/`, and `util/`.
It must not depend on `web/`, executable wiring, or HTTP types.
