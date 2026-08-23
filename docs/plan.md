# Workspace migration plan

Status: completed.

## Goal

`Workspace` is the only process-wide source of data loaded from the
`workspace/` directory tree. `Workspace::load()` builds a complete validated
candidate, `loadws()` atomically publishes it, and `getws()` returns a shared
pointer to the current immutable value.

Sessions keep only their identity, database/lease, transcript, and mutable
runtime state. `SessionRepository`, SQLite data, and provider workers remain
independent process/runtime objects.

## Rules preserved

- Workspace configuration is loaded only by `Workspace::load()`; write methods
  parse only the target file they atomically replace.
- Callers retain the `shared_ptr` returned by `getws()` while using workspace
  references.
- No session or repository caches a second workspace catalog.
- An in-flight provider request owns an immutable copy of the exact prompt and
  backend configuration it is already executing.
- Workspace writes use a unique temporary file followed by rename.
- A failed load never replaces the currently published workspace.

## Completed migration map

| Place | Result |
| --- | --- |
| `src/web/lobby_routes.*` | Bootstrap, detail, settings, session routes, and reload read `Workspace`; routes receive `SessionRepository` directly. |
| `src/session/session_repository.*` | One process-owned repository obtains the current forum session directory from `Workspace` per operation. |
| `src/workspace/session_open.*` | Opens from `getws()` plus the independent repository; no disk re-read or workspace lifetime capture. |
| `src/session/session_controller.*` | Production controllers retain stable IDs and session state; character, persona, provider, and style operations use the current `Workspace`. |
| Forum character lookup | `Workspace` owns forum membership and handle resolution; the old `ForumCharacters` projection was removed. |
| `src/web/session_projection.cpp` | Builds character/persona presentation from the current `Workspace` and overlays session-local style overrides. |
| `src/providers/*` and `src/characters/*` | Provider execution stays workspace-independent and receives one request-owned input copied from `Workspace`. Obsolete filesystem loaders were removed. |
| `src/session/opened_session.h`, `src/web/live_session.*` | No workspace-generation or generic dependency lifetime is retained. Test-backed controllers own their injected provider executor directly. |
| `src/web_main.cpp` | Publishes one `Workspace`, owns one `SessionRepository`, and calls the free `open_session()` function. Logging uses `WorkspaceSettings`. |
| Reload path | Stops sessions, backs up, loads one candidate, synchronizes session SQL, then publishes that same candidate. |
| `src/workspace/workspace.*` | Exposes forum-member and session-directory lookup, resolved provider/style labels, built-ins, atomic config writes, and publication. |

## Removed implementation

- `WorkspaceRuntime` and `WorkspaceGeneration`;
- `WorkspaceDefinition`;
- `ForumCharacters` and its copied roster;
- the duplicate workspace character/provider filesystem loaders;
- the old built-in assembly source;
- obsolete CMake entries and tests tied only to those implementations.

## Request boundary

`Workspace` stores the common character, persona, appearance, and provider
value types directly. `Workspace::character_definition()` returns an owned
copy for one provider request, so no adapter or duplicate value hierarchy is
needed.

Controller tests publish small real workspaces and use the same lookup path as
production. The only test-specific controller seam is provider behavior,
inactive leases, and deterministic activation failure.

## Verification scope

- Workspace load, validation, built-ins, lookup, write, and publication tests;
- session opening, repository, controller, and concurrent-controller tests;
- lobby, live-session, reload, process, and stress tests;
- local/mock integration tests;
- repository search confirming no production reference to the removed
  workspace hierarchy and no workspace-tree reads outside `Workspace::load()`.
