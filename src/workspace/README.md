# Workspace layer

`workspace/` owns configuration import/export, the normal runtime
configuration store, and the immutable in-memory workspace view. It contains
no HTTP transport, session opening, or live-session switching policy.

| Component | Responsibility |
| --- | --- |
| `Workspace` | Load and validate providers, styles, personas, characters, forums, prompts, and built-ins. |
| `WorkspaceConfigStore` | Own the database lease, secured database, private materialization, configuration mutex, and atomic runtime edits. |
| `import_workspace_configuration()` / `export_workspace_configuration()` | Perform lease-protected offline transfer between a directory and database rows. |
| `WorkspaceConfigEditor` | Apply edits to candidate workspace text files. |
| `builtins` | Reserved built-in IDs and Welcome constants. |

`Workspace::load()` reads a physical directory tree once and builds a complete
candidate. It resolves character and forum prompts, provider and style
selections, forum membership and defaults, descriptions, labels, and the
Guest, Assistant, and Entrance data. Invalid referenced
configuration fails the load. `WorkspaceConfigStore` publishes only a
successfully built and committed candidate, so readers see either the previous
workspace or the complete new one.

Workspace values are the only process-wide copy of workspace configuration.
Callers hold the `shared_ptr` returned by `WorkspaceConfigStore::snapshot()`
while using references into it. Sessions retain only their IDs,
transcript/database key, generation state, and style overrides. At generation
start, a request receives an owned immutable copy of the exact character
prompt and provider settings it will execute; an in-flight request therefore
cannot change halfway through.

`WorkspaceConfigStore` is the top-level normal-runtime owner. It holds the
database lease and handle, one owner-private root containing `workspace/` and
`welcome/`, the configuration mutex, and cleanup. `SessionRepository` is
independent and process-owned but receives explicit paths from that store; it
owns neither the lease nor either private directory and uses short-lived
connections for storage operations.

Online configuration mutations go through `WorkspaceConfigStore` and the
corresponding `Workspace` write, create, or delete operation. Each store
operation edits the materialized files, loads a complete candidate, collects
the complete small file set, compares it with the committed rows, and inserts,
updates, or deletes only changed `config` rows in one SQLite transaction. It
publishes the candidate only after commit. A pre-commit failure rematerializes
the old rows; a post-commit publication failure requires restart and then loads
the committed rows. Offline import and export use the separate transfer
operations.

Character settings can override their provider's `reasoning_effort` and
`web_search`. The overrides remain optional in `WorkspaceCharacter`; generation
copies the provider config and applies them when it constructs the request's
`CharacterDefinition`.

## Source map

| Files | Responsibility |
| --- | --- |
| `workspace.*` | Load, validate, and query an immutable workspace configuration. |
| `workspace_config_editor.h` | Edit candidate workspace text files before validation and commit. |
| `workspace_config_store.*` | Materialize, publish, import, export, and transactionally update workspace configuration. |
| `builtins.h` | Declare reserved built-in IDs and Welcome constants. |

This directory may depend on `characters/`, `chat/`, `providers/`, `storage/`,
and `util/`. Session opening belongs to `session/`, and live-session behavior
belongs to `runtime/`.

Focused tests live in `tests/workspace/`.
