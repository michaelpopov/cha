# Conversation model

`conversation/` owns the interface-neutral transcript model. It describes what
a conversation entry means and how live transcript state changes, without
knowing how entries are rendered, persisted, or sent to a completion provider.

## Contents

| Source | Responsibility |
| --- | --- |
| `request_id.h` | Request identifier shared by prompts, agent events, and durable turn state. |
| `response_content.h` | Reasoning-versus-answer classification for completion deltas. |
| `conversation.*` | Entry types, validation, entry factories, snapshots, read views, and the thread-safe `Conversation` container. |

## Functionality

`ConversationEntry` is the common semantic record used by the application,
storage, rendering, and model-context projection. It records participant
identity separately from display labels and distinguishes human, agent, notice,
and error entries. Completion status distinguishes live streaming output from
complete, cancelled, and failed records.

`Conversation` has a single-writer design with synchronized readers. It
supports atomic snapshots for interfaces and a short-lived
`ConversationReadView` for agent request preparation. At most one streaming
entry may be open, and mutations advance a revision used by incremental
rendering.

The validation helpers enforce the model at its boundaries:

- `validate_conversation_entry()` checks semantic field combinations;
- `require_terminal_conversation_entry()` rejects live streaming state;
- `require_storable_conversation_entry()` also rejects ephemeral reasoning.

## Dependencies

This is a domain leaf:

- it depends only on the C++ standard library;
- it does not depend on agents, storage, application services, or interfaces;
- `agents/`, `storage/`, `application/`, and terminal rendering may depend on
  its public values.

Persistence rules and presentation choices must stay outside this directory.
The model may expose the information those consumers need, but it must not
import SQLite, provider-protocol, or terminal concepts.
