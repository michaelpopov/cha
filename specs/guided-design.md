# Model-guided sessions: technical design

Date: November 1, 2026

Status: Technical design for review. This document does not implement the feature.

Requirements: [docs/guided-prd.md](../docs/guided-prd.md).

## 1. Summary

A model-guided session is an ordinary stored session with one more durable value: `guided`. The `/guided` command sets the value. When the value is true, every character request in the session gets a fixed instruction block at the end of the current user message. Nothing else changes.

The design uses existing paths:

| Need | Existing path that the design extends |
| --- | --- |
| Start the mode | The command parser in `text_input.cpp`, like `/clear`. One more `CommandKind`. |
| Carry the mode to the model request | `start_generation()`, which builds every `GenerationRequest` of a character turn. |
| Add text to the model request | The `<research_requirements>` block in `project_model_context()`. One more block after it. |
| Keep the mode across reopen and restart | The `sessions` row, read by `read_state()` into `SessionRestore`, like `discardable`. |
| Upgrade old databases | `ensure_session_lifecycle_columns()`, which already adds `recent_pending` and `discardable`. |

The design adds no Jev change, bridge method, wire field, frontend code, setting, log channel, background work, or new table. ChaWeb gets the behavior through the shared `handle_text_input()` and `SessionController` with no ChaWeb change.

## 2. Mode state and lifecycle

### 2.1 Rule

- `guided` is false for a new session and for the sessions from before this change.
- `/guided` sets it to true, except in Welcome.
- `/clear` sets it to false.
- Nothing else changes the value. Prompts, Jev, removal of turns, cover, and reopen do not change it.

### 2.2 State locations

| Location | Change |
| --- | --- |
| `sessions` table | New column `guided INTEGER NOT NULL DEFAULT 0 CHECK (guided IN (0, 1))`. |
| `SessionRestore` | New `bool guided{}`, read by `read_state()` / `build_restore()`. |
| `SessionController` | New member `bool guided_{}`, set in `initialize()` from `restored.guided`, next to `discardable_`. |

| Event | Database | Memory |
| --- | --- | --- |
| `/guided` | `SessionJournal::set_guided()` sets `guided = 1`. | `start_guided()` sets `guided_ = true` after the write. |
| `/clear` | `SessionJournal::clear_history()` sets `guided = 0` in its existing transaction. | `clear_conversation()` sets `guided_ = false` after the write. |
| Open a session | `read_state()` reads the column. | `initialize()` copies it. |

If a write fails, `persist()` throws before the member changes, so memory and the database stay the same.

`clear_conversation()` calls `clear_history()` also when the transcript is empty, and then returns early. Set `guided_ = false` directly after the `persist()` call, before that early return, so `/clear` also ends the mode in a session with no messages.

## 3. Command

### 3.1 Parser

Add `guided` to `CommandKind` in `text_input.h`. In `text_input.cpp`, add `CommandDescriptor{"/guided", CommandKind::guided}` after `/detach`. `command_names()` then adds `/guided` to the existing "Unknown command" notice.

In `handle_text_input()`, next to `/clear`:

```cpp
if (command.kind == CommandKind::guided) {
    if (!command.argument.empty()) {
        result.clear_input = true;
        result.session.notice = "Usage: /guided";
        return result;
    }
    result.session = controller.start_guided();
    result.clear_input = result.session.input_consumed;
    return result;
}
```

The existing code before this point already calls `controller.retain()` and rejects input during generation. The path check applies only to `text` and `unknown` commands, so it does not see `/guided`.

### 3.2 Controller

Add `ControllerUpdate start_guided();` to `SessionController`, next to `clear_conversation()`:

```cpp
ControllerUpdate SessionController::start_guided() {
    if (is_generating()) {
        return busy_notice();
    }
    if (identity_.forum_id == workspace_entrance_id && identity_.session_id == welcome_id) {
        return {.input_consumed = true, .notice = "Guided mode is not available in Welcome"};
    }
    persist("start guided mode", [this] { journal_.set_guided(); });
    guided_ = true;
    return {.input_consumed = true, .notice = ""};
}
```

The function does not look at the transcript. `/guided` works before the first prompt and after earlier turns. A second `/guided` writes the same value again. The mode has no visible state, so the function returns no snapshot.

## 4. Request flow

### 4.1 `start_generation()`

Set `.guided = guided_` in each `GenerationRequest` that the function builds. All multicast children get the same value, as the PRD requires for several characters.

`dispatch_target()`, `start_resolved_multicast()`, `start_generation()`, and the Jev classification path get no new parameters.

### 4.2 Requests that do not change

Session naming, conversation summaries, note compression, Assistant maintenance in Welcome, and Jev build their own requests. They keep the default `guided = false`.

## 5. Model instruction

Add `bool guided{}` to `GenerationRequest`, after `research_needs`.

In `project_model_context(const GenerationRequest&, std::string_view)`, after the optional `<research_requirements>` block, append this block to the content of the last user message when `input.guided` is true:

```text


<guided_session>
The user asked you to lead this conversation. Follow the topic and the
guidelines that the user gave. Give the content in parts. Make each answer
one part: usually 2 to 4 paragraphs, and never more than 6 paragraphs. A part
can be shorter, for example one question. End each part at a natural stop,
then stop and wait for the user. Do not write the user's words, answers, or
decisions. If the user's message has no new content, for example "continue"
or "go on", continue from where you stopped. If the message has new content,
use it and continue. The user's own guidelines, at the start or later, take
precedence over these rules, including the length of a part.
</guided_session>
```

Store the text as one string constant in `model_context.cpp`, written as one paragraph without the line breaks shown above. The block starts with two newlines, like the research block.

Reasons for this location:

- It follows the research block pattern. The system prompt and the projected history stay the same for normal and guided sessions, so the cached prompt prefix stays valid.
- Older user messages in the history do not contain the block, because the projection adds it only to the current message. The model sees the instruction once per request.
- Both protocol encoders and every provider already use `project_model_context()`. No encoder changes.

The block is not stored in the transcript, the journal, or the Markdown mirror. The transcript keeps the user's text unchanged.

## 6. Storage

### 6.1 Schema

- `create_workspace_session_schema()`: add the `guided` column to `CREATE TABLE sessions` after `discardable`.
- `ensure_session_lifecycle_columns()`: read `PRAGMA table_info(sessions)` as now. If `guided` is missing, run `ALTER TABLE sessions ADD COLUMN guided INTEGER NOT NULL DEFAULT 0 CHECK (guided IN (0, 1))`. Old rows become normal sessions, as the PRD requires.
- The Welcome database is created through the same schema. It gets the column, but its value stays 0.
- `INSERT INTO sessions` statements in `session_repository.cpp` and `session_database.cpp` need no change, because of the default.

### 6.2 Journal

| Function | Change |
| --- | --- |
| `read_state()` | Select `guided` and return it in `DurableState`. |
| `build_restore()` | Copy it into `SessionRestore::guided`. |
| `set_guided()` | New. Run `UPDATE sessions SET guided = 1 WHERE session_key = ?1`, like `publish_recent()`. |
| `clear_history()` | Run `UPDATE sessions SET guided = 0 WHERE session_key = ?1` before the commit. |

`start_turn()` and `delete_turn()` do not change.

Vault export does not contain sessions. R2 upload and download copy the database file, so they keep the column with no change.

## 7. Behavior checks against the PRD

| PRD rule | Design result |
| --- | --- |
| Only `/guided` starts the mode | `start_guided()` is the only code that writes `guided = 1`. |
| Jev takes no part in the decision | No Jev change. |
| Welcome is always normal | `start_guided()` refuses in Welcome. |
| `/guided` at any time | `start_guided()` does not look at the transcript. Each request built after the command reads `guided_`. |
| `/guided` with an argument | The parser shows "Usage: /guided" and does not call the controller. |
| Mode stays after reopen and restart | The column is restored into `guided_`. |
| `/clear` ends the mode | `clear_history()` resets the column, and `clear_conversation()` resets the member, also for an empty transcript. |
| Removal of turns keeps the mode | `delete_turn()` does not touch the column. |
| Old sessions are normal | The migration default is 0. |
| Every answering character gets the instruction | `start_generation()` uses `guided_` for every target. |
| No truncation | No change to provider code, `max_tokens`, or error handling. |
| Hands-free use | No change to frontend voice code. |
| No visible UI change | No bridge, wire, or frontend change. The new text is only in notices. |

## 8. Tests

Extend the existing test files. Do not add new test targets.

`tests/runtime/unit_text_command.cpp`:

- `/guided` and `/guided\t` parse as `CommandKind::guided`. `/guideds` parses as `unknown`. `@Ismael /guided` and ` /guided` parse as `text`, like `/clear`.
- `command_names()` returns `/mcast, /clear, /detach, /guided`.

`tests/runtime/unit_text_input.cpp`:

- `/guided later` shows "Usage: /guided" and does not change the mode.
- `/guided` consumes the input and clears the notice.

`tests/agents/unit_model_context.cpp`:

- `guided = true` appends the block to the last user message only. The history messages and the system message do not change.
- With research needs and `guided` both set, the research block comes first.
- `guided = false` gives the same messages as now.

`tests/storage/unit_workspace_session_database.cpp`:

- A database without the column gets it from `ensure_session_lifecycle_columns()`, and existing rows read as `guided = 0`.
- `set_guided()` stores 1, and a restore returns it.
- `clear_history()` stores 0.
- `delete_turn()` of the last turn keeps 1.

`tests/session/unit_session_controller.cpp`:

1. Without `/guided`, generation requests send `guided = false`.
2. After `/guided`, the next prompt and later prompts send `guided = true`.
3. `/guided` after earlier turns makes the next request send `guided = true`.
4. `/guided` in Welcome returns the notice, and later requests send `guided = false`.
5. With `/mcast` or a Jev `all_characters` decision after `/guided`, every multicast child gets `guided = true`.
6. After `/guided`, the removal of the only turn keeps `guided = true`.
7. After `/guided`, `/clear` makes the next prompt send `guided = false`. This is also true when `/clear` comes before the first prompt, and after the session is reopened.
8. Reopening a guided session restores the mode.

## 9. Manual validation

Run with a real provider:

- In new sessions, send `/guided` and then each PRD example. The first answer is one part, and "continue", "go on", and "let's move" give the next parts.
- Without `/guided`, the same prompts give normal answers.
- `/guided later` and `/guided` in Welcome show their notices.
- Do one hands-free round with automatic audio and the send phrase: "go on, over to you".
- Restart CHA, reopen a guided session, and continue it.
- Use `/clear` in a guided session. The next prompt gives a normal answer.
- Open an existing vault from before this change. Old sessions open, work, and stay normal. `/guided` works in them.

## 10. Files to change

| File | Change |
| --- | --- |
| `src/runtime/text_input.h`, `src/runtime/text_input.cpp` | `CommandKind::guided`, the descriptor, and the handler. |
| `src/characters/model_context.h`, `src/characters/model_context.cpp` | `GenerationRequest::guided` and the instruction block. |
| `src/session/session_controller.h`, `src/session/session_controller.cpp` | `guided_`, `start_guided()`, the reset in `clear_conversation()`, and `.guided` in `start_generation()`. |
| `src/storage/session_database.h`, `src/storage/session_database.cpp` | `SessionRestore::guided`, `read_state()`, `set_guided()`, `clear_history()`. |
| `src/storage/workspace_session_database.cpp` | Schema column and migration. |
| `resources/application-guide.md` | One line for `/guided` in **Commands**. |
| Test files in section 8 | New cases. |

No changes in `src/providers/`, `src/bridge/`, `src/daemon/`, `webapp/`, `packaging/`, or provider encoders.
