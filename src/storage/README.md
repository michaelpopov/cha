# Storage and workspace infrastructure

`storage/` owns all filesystem and SQLite access. It discovers rooms and
personas, loads persisted agent definitions, manages session files, and
implements the durable conversation journal.

## Structure

### Workspace and configuration loading

| Source | Responsibility |
| --- | --- |
| `workspace.*` | Resolve the workspace layout, enumerate rooms, and load an ordered room/persona definition. |
| `config_loader.*` | Parse one persona's TOML configuration into an agent-owned `Config`. |
| `agent_definition_loader.*` | Build `AgentDefinition` values from persona and room files. |

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

`Config` and `AgentDefinition` intentionally remain agent-owned value types.
Storage constructs them because it reads their persisted representation; it
does not own their runtime meaning.

## Dependencies

Storage may depend on:

- `agents/` for configuration and agent-definition value types;
- `conversation/` for durable entries, identifiers, and restore state;
- `util/` for safe path components and shared text parsing;
- SQLite and toml++ for concrete persistence formats.

It must not depend on `application/` or `interfaces/`. Storage exposes
mechanisms and storage-shaped data; application services decide when and why
those mechanisms are used.
