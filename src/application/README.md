# Application services

`application/` is the reusable use-case layer. It owns workspace discovery,
session persistence, and live chat coordination while presenting structured
operations that do not depend on terminal command syntax or UI widgets.

## Contents

| Source | Responsibility |
| --- | --- |
| `workspace.*` | Resolve workspace layout, list rooms/sessions, load room agent definitions, and construct a `ChatCoordinator`. |
| `sessions_repository.*` | List, create, identify, and safely resolve SQLite session files for one room. |
| `session_database.*` | Initialize databases, restore transcripts, and journal turn transitions (`ConversationJournal`). |
| `chat_coordinator.*` | Own one live session: prompts, agent events, default agent, notices, persistence, and shutdown. |
| `response_controller.*` | Own the single in-flight response: start a turn, apply agent events, update conversation and journal. |
| `generation_status.h` | Describe active generation state and provide the shared in-progress notice. |

## Workspace and sessions

`Workspace` resolves the on-disk workspace root (`personas/`, `rooms/`), lists
rooms, loads a room’s ordered persona roster, and exposes session use cases.
Creating or opening a session loads `AgentDefinition` values through `agents/`,
opens or creates a SQLite file through `SessionsRepository`, and returns a
`ChatCoordinator`.

`SessionsRepository` treats each session as a self-contained SQLite file and
validates embedded session identity when listing or opening. Creation
initializes a hidden temporary sibling and publishes it without replacing an
existing destination.

`ConversationJournal` persists transcript and request-lifecycle changes
transactionally. Restore validates durable entries and reports interrupted
turns for application-level repair. Streaming entries and reasoning text are
not durable session content.

Application-owned persistence types include `Room`, `Session`,
`SessionSummary`, `SessionDatabaseMetadata`, `InterruptedTurn`, and
`ConversationRestore`. Selectors and adapters see room names and
`SessionSummary` values, not repository paths.

## Chat coordination

`ChatCoordinator` is the UI-facing owner of one live session. It composes
`Conversation`, `ConversationJournal`, `AgentRegistry`, and
`ResponseController`. Public methods are structured operations such as
`submit_prompt()`, `clear_conversation()`, `set_default_agent()`,
`request_stop()`, `receive()`, and `shutdown()`, returning `CoordinatorUpdate`
side effects for the interface.

Only one turn may be active. A prompt is made durable before it is added to
memory and submitted for execution. Agent events are correlated with the
active request, persisted at terminal transitions, and then reflected in the
live transcript. Session notices (unknown/ambiguous handles, roster and
`/info` text) are formatted inside the coordinator.

The coordinator does not parse `/commands`, leading mentions, HTTP routes, or
JSON request bodies. Interface adapters translate those protocols into its
structured methods.

## Dependencies

Application services may depend on:

- `conversation/` for live and restored transcript values;
- `agents/` for definitions, rosters, execution, and agent events;
- `util/` for path and text helpers;
- SQLite for concrete session persistence.

They must not depend on `interfaces/` or `apps/`. This keeps the same use
cases available to the terminal interface, a future HTTP interface, tests, and
other composition roots.
