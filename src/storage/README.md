# Storage and workspace infrastructure

`storage/` owns workspace discovery and SQLite session persistence. It
resolves rooms and persona directories, manages session files, and implements
the durable conversation journal. Loading agent configuration from those
paths belongs to `agents/`.

## Structure

### Workspace layout

| Source | Responsibility |
| --- | --- |
| `workspace.*` | Resolve the workspace layout, enumerate rooms, and load an ordered room/persona definition. |

### Session persistence

| Source | Responsibility |
| --- | --- |
| `session_repository.*` | List, create, identify, and safely resolve SQLite session files for one room. |
| `session_database.*` | Initialize databases, restore transcripts, and journal turn transitions. |

## Functionality

`Workspace` understands the on-disk workspace shape and validates configured
names before resolving paths. Persona order is preserved because it determines
roster and backend order.

`SessionRepository` treats each session as a self-contained SQLite file. It
validates embedded session identity when listing or opening a file. Creation
initializes a hidden temporary sibling and publishes it without replacing an
existing destination.

`ConversationJournal` persists transcript changes and request lifecycle changes
transactionally. Restore code validates durable entries and reports interrupted
turns for application-level repair. Streaming entries and reasoning text are
not durable session content.

## Boundary types

This directory owns storage representations such as `Room`, `Session`,
`SessionDatabaseMetadata`, and `ConversationRestore`. These types are not
interface contracts. `application/` converts repository `Session` records into
`SessionSummary` values before returning them to a selector or future HTTP
adapter.

`Config` and `AgentDefinition` remain agent-owned, including the loaders that
materialize them from persona and room files.

## Dependencies

Storage may depend on:

- `conversation/` for durable entries, identifiers, and restore state;
- `util/` for safe path components and shared text parsing;
- SQLite for concrete session persistence.

It must not depend on `agents/`, `application/`, or `interfaces/`. Storage
exposes workspace paths and journal mechanisms; application services decide
when and why those mechanisms are used and call agents to load definitions.
