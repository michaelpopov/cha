# Workspace layer

`workspace/` owns configuration import/export, the normal runtime
configuration store, the immutable in-memory workspace view, and the operation
that opens a session. It contains no HTTP transport or live-session switching
policy.

| Component | Responsibility |
| --- | --- |
| `Workspace` | Load and validate workspace settings, providers, styles, personas, characters, forums, prompts, and built-ins. |
| `getws()` / `loadws()` | Read or atomically replace the current published `Workspace`. |
| `WorkspaceConfigStore` | Own the database lease, secured database, private materialization, configuration mutex, and atomic runtime edits. |
| `import_workspace_configuration()` / `export_workspace_configuration()` | Perform lease-protected offline transfer between a directory and database rows. |
| `open_session()` | Combine the current `Workspace` with prepared session storage and construct a `SessionController`. |
| `builtins` | Reserved built-in IDs and Welcome constants. |

`Workspace::load()` reads a physical directory tree once and builds a complete
candidate. Its separate durable path base lets relative log paths resolve from
the database parent rather than the temporary materialization. It resolves
character and forum prompts, provider and style
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

`WorkspaceConfigStore` is the top-level normal-runtime owner. It holds the
database lease and handle, one owner-private root containing `workspace/` and
`welcome/`, the configuration mutex, and cleanup. `SessionRepository` is
independent and process-owned but receives explicit paths from that store; it
owns neither the lease nor either private directory and uses short-lived
connections for storage operations.

The only online configuration writes use
`Workspace::write_character_settings()`,
`write_forum_default_character()`, and `write_forum_default_persona()`. Each
store operation edits the materialized file, loads a complete candidate,
collects the complete small file set, replaces all `config` rows in one SQLite
transaction, and publishes the candidate only after commit. A pre-commit
failure rematerializes the old rows; a post-commit publication failure requires
restart and then loads the committed rows. All other edits use offline
export/edit/import.

This directory may depend on `session/`, `characters/`, `chat/`, and `util/`.
It must not depend on `web/`, executable wiring, or HTTP types.
