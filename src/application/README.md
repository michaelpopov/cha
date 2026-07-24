# Application services

`application/` is the reusable use-case layer. It coordinates conversation
state, agent execution, and persistence while presenting structured operations
that do not depend on terminal command syntax or UI widgets.

## Contents

| Source | Responsibility |
| --- | --- |
| `chat_coordinator.*` | Coordinate one live session, including prompts, agent events, persistence, and shutdown. |
| `workspace_service.*` | Expose room/session use cases and construct the selected coordinator. |
| `session_summary.h` | Present selectable session metadata without exposing the storage representation. |
| `generation_status.h` | Describe active generation state and provide the shared in-progress notice. |

## Chat coordination

`ChatCoordinator` owns the live `Conversation`, `ConversationJournal`,
`AgentRegistry`, default agent, identifier counters, and optional active turn.
Its public methods are structured operations such as `submit_prompt()`,
`clear_conversation()`, `set_default_agent()`, `request_stop()`, and
`receive()`.

The coordinator is the only conversation and journal writer. A prompt is made
durable before it is added to memory and submitted for execution. Agent events
are correlated with the active request, persisted at terminal transitions, and
then reflected in the live transcript. Only one turn may be active.

The coordinator deliberately does not parse `/commands`, leading mentions, HTTP
routes, or JSON request bodies. Interface adapters translate those protocols
into its structured methods.

## Workspace use cases

`WorkspaceService` lists rooms and prepares a selected room. The move-only
`PreparedRoom` owns one validated `Room`, its fully loaded ordered agent
definitions, and its `SessionRepository`.

Definitions are loaded once per preparation and reused while the user retries
session selection and when a session is opened or created. `PreparedRoom`
restores durable state, maps storage `Session` values to `SessionSummary`, and
constructs the selected `ChatCoordinator`.

This boundary prevents terminal and future HTTP interfaces from reaching into
workspace loaders or repositories.

## Dependencies

Application services may depend on:

- `conversation/` for live and restored transcript values;
- `agents/` for rosters, execution, and agent definitions;
- `storage/` for workspace loading, repositories, and journaling.

They must not depend on `interfaces/` or `apps/`. This rule keeps the same use
cases available to the terminal interface, a future HTTP interface, tests, and
other composition roots.
