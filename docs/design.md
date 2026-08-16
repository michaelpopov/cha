# Design: the Null agent (`-`) — record-only monologue

## Problem

Sometimes the user wants to think out loud across several messages *before*
asking a character anything — notes, framing, half-formed thoughts — and have
those messages become part of the conversation the next character sees, without
spending a model round-trip on each one and without a reply cluttering the
chat.

The goal: a pseudo-participant addressed as `-` that behaves like any other
character for the two addressing commands —

- `@- <text>` records one message, and
- `/@-` switches the session to it so plain (unprefixed) messages are recorded —

while it

- records each message to the transcript and shows it in the web UI like any
  other human message;
- does **not** call any model and produces **no** reply;
- later reaches a real character as ordinary **shared conversation**, not as a
  message addressed to that character.

## Current behavior (relevant facts)

- **Addressing grammar.** `parse_addressed_prompt()`
  (`src/web/text_mention.cpp:7`) reads a leading `@Handle`; a doubled `@@`
  escapes a literal leading at-sign (line 16). `@- text` parses as handle `-`,
  body `text`, and `/@-` is read by `parse_command` as set-default with handle
  `-` — neither disturbs the `@@` escape. `-` was chosen precisely to leave the
  escape alone.
- **Prompt dispatch.** `SessionController::submit_prompt()`
  (`src/session/session_controller.cpp:321`) resolves the handle (or, for an
  empty handle, the session default via `characters_.find(default_character_id_)`
  at line 335), builds a human `TranscriptEntry`, stages a `GenerationBatch`,
  and streams the reply. Human entry and response are persisted together as one
  *turn*.
- **Switching the default.** `set_default_character()`
  (`src/session/session_controller.cpp:747`) resolves the handle and, on
  success, the web layer persists the new id to the forum's `config.toml`
  (`text_input.cpp:106` sets `persist_default_character_id` when the update
  carries a snapshot). The default is validated when a session opens
  (`session_controller.cpp:222`). By contrast, `/provider` and `/style` are
  **session-scoped and never saved** (`README.md`) — the precedent the null
  agent's `/@-` follows.
- **The forum's characters are backend-derived.** `characters_` is built from
  `generation_executor_.runtime_info()` (`make_forum_characters`,
  `src/session/session_controller.cpp:184`) — one entry per model backend. A
  participant with no backend cannot be a `ForumCharacters` member, and
  `resolve_handle()` would never find it.
- **Human entries must name an addressee.** `validate_transcript_entry()`
  rejects a human entry whose `addressed_to` / `addressed_to_name` is empty
  (`src/chat/transcript.cpp:114`). A recorded message must be addressed to
  *something*.
- **Model-context projection is per-entry, and already emits shared history.**
  `project_model_context()` (`src/agents/model_context.cpp:38`) walks entries
  one at a time; a human entry projects on its own with no matching response
  required (line 61). An entry whose `addressed_to` is not the current
  character is bundled into the `Shared chat history (JSONL):` block
  (`src/agents/model_context.cpp:71`, heading at `src/agents/model_context.h:15`)
  rather than shown as a first-person message. **There is no user/assistant
  alternation requirement**, so a run of consecutive human messages is
  well-formed.
- **Persistence does not require a turn.** `entries.request_id` is nullable
  (`src/session/session_database.cpp:373`); the `one_prompt_per_turn` index
  only covers rows with a non-null `request_id` (line 394). `insert_entry()`
  (line 555) stores any storable entry and advances the persisted
  `next_entry_id`. A lone human entry with no turn is a legal, restorable row —
  there is just no journal method that writes one today.

## Proposed design

`-` is a **reserved null-target sentinel**, not a `ForumCharacters` member. It
is recognised in `submit_prompt()` and `set_default_character()`, and
everywhere short-circuits before any model machinery is touched. `/@-` is a
**session-local** switch: it does not save the default-character setting, while
the messages recorded through it remain durable. A real character may not use
`-` as either its stable id or display name; otherwise the sentinel could
silently intercept prompts intended for that character.

### Constants

Define alongside the existing well-known participant names in
`src/chat/transcript.h:17` (next to `notice_display_name`):

```cpp
inline constexpr std::string_view null_agent_handle = "-";
inline constexpr std::string_view null_agent_name   = "-";
```

They are visible to both the controller and the C++ web layer (which includes
the transcript header transitively), so those layers do not hard-code the
literal `"-"`. The React client uses the literal once when interpreting the
wire value because it cannot share the C++ constant.

Although the current character validators accept `-`, the sentinel must be
unambiguous at every construction path. Update `validate_character_id()` and
`validate_character_display_name_syntax()` (`src/agents/character.cpp`) to
reject the exact value `-` using these constants. Checking the syntax-level
display-name validator matters because trusted/built-in construction paths may
bypass the workspace-only reserved-name check. Existing workspace characters
named or identified as `-` become invalid with a clear “reserved for the null
agent” error rather than changing behaviour silently.

### 1. Recording a message: `record_monologue`

A new private helper does the whole job:

```cpp
void SessionController::record_monologue(
    std::string_view author_id, std::string text, ControllerUpdate& update) {
    if (text.empty()) { update.notice = "Message to @- is empty"; return; }
    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return;
    TranscriptEntry entry = make_human_entry({
        .id = next_entry_id_++,
        .author = std::move(*author),
        .addressed_to = {std::string(null_agent_handle),
                         std::string(null_agent_name)},
        .text = std::move(text),
    });                                   // no request_id
    journal_.record_entry(entry);         // persist first (see §4)
    transcript_.add_entry(entry);
    update.input_consumed = true;
    update.notice = "";                    // clear any stale frontend notice
    require_snapshot(update);
}
```

No `GenerationBatch`, no `ActiveResponse`, no worker thread, no reply. It reuses
the same human-entry construction the normal path uses (`make_human_entry`,
`src/chat/transcript.cpp:17`), and the persist-before-transcript order of
`activate_current_run` (`session_controller.cpp:437`). `addressed_to = -`
satisfies the addressee-required invariant. The explicit empty-string notice
clears a previous success or error notice just as normal prompt activation
does. In recording mode this also clears the mode-switch notice after the first
recorded message; the composer remains the persistent indication of the mode.

### 2. Two entry points in `submit_prompt`

- **Inline `@- text`.** In the non-empty-handle branch, before
  `resolve_handle()` (`session_controller.cpp:337`):
  `if (handle == null_agent_handle) { record_monologue(...); return update; }`.
- **Default is `-`.** In the empty-handle branch (line 334), before
  `characters_.find(default_character_id_)`:
  `if (default_character_id_ == null_agent_handle) { record_monologue(...);
  return update; }` — so plain messages record while in recording mode instead
  of hitting the "default character not found" `logic_error` (line 345).

The existing `busy()` guard and the existing early return for an entirely empty
plain submission stay in force. Consequently `@-` with no body reaches
`record_monologue` and reports an empty-message notice, while an empty plain
submission in recording mode remains the same silent no-op it is in normal
mode (and the web input layer already discards an empty input before dispatch).

### 3. `/@-`: session-local recording mode

- `set_default_character()` (`session_controller.cpp:747`): before
  `resolve_handle()`, `if (handle == null_agent_handle)` set
  `default_character_id_ = std::string(null_agent_handle)`, produce a snapshot,
  and set a notice such as *"Recording to @- — messages are saved to the
  transcript but not sent to a model. Use /@<name> to resume."*
- The web layer must **not** persist this. In `text_input.cpp`'s `set_default`
  case (line 106), guard the persist with
  `controller.view().default_character_id != null_agent_handle`, so `-` is never
  written to `config.toml`.
- Because `-` is never persisted, a reopened session's default comes from
  `config.toml` as before — a real character. `initialize`'s open-time
  validation (`session_controller.cpp:222`) never sees `-` and needs no change.

### 4. New journal method: `record_entry`

Add to `SessionJournal` (`src/session/session_database.h:100`):

```cpp
void SessionJournal::record_entry(const TranscriptEntry& entry) {
    Transaction transaction(impl_->database);
    insert_entry(impl_->database, current_epoch(impl_->database), entry);
    transaction.commit();
}
```

Mirrors `finish_turn`'s entry write without a turn. `insert_entry` validates
storability and advances `next_entry_id`; the null `request_id` is exempt from
`one_prompt_per_turn`. No schema change.

### 5. Webapp: show recording mode

While `default_character_id === '-'` (`Screens.tsx:128`), the composer already
degrades to "Message character" and the character `<select>` (line 350) has no
matching option — functional but opaque. Small touch: render the composer
placeholder as the recording state (e.g. *"Recording — saved, not sent"*) and,
**only while the current default is `-`**, render a selected option such as
`<option value="-">Recording</option>`. It exists only to represent the current
controlled-select value. It is absent for a real default, so the dropdown cannot
enter recording mode; `/@-` remains the only mode switch. From recording mode,
choosing any real character follows the existing typed `set_default_character_by_id`
path, leaves recording mode, and persists that real character normally. No
sentinel support or persistence guard is added to the typed path.

## Resulting semantics

- `@- <text>` records one message and returns immediately, no reply.
- `/@-` enters recording mode for the session; every plain message is recorded
  the same way until `/@<name>` or the character selector switches back. The
  real character selected on exit is persisted exactly as it is today. The
  temporary `-` default is not saved; the recorded transcript entries are.
- To a real character addressed later, each recorded message appears inside the
  `Shared chat history (JSONL):` block as
  `{"kind":"human","speaker":"<persona>","addressed_to":"-","text":"…"}` —
  shared context, never a first-person prompt. This is the "part of the shared
  conversation, not addressed to a specific character" behaviour, straight out
  of the existing projection.
- Recorded entries persist and restore like any other; a reopened session shows
  them and still feeds them to the next character.

## Edge cases

- **Generating.** `record_monologue` keeps the `busy()` guard; no recording
  while a reply streams.
- **Empty message.** `@-` alone is a notice, not an entry. An empty plain input
  in recording mode is a silent no-op, matching normal mode and the web input
  layer's existing empty-input handling.
- **`/info` / `/characters` while recording.** `format_characters_notice`
  (`session_controller.cpp:733`) marks the default with `*` by matching a
  backend id; `-` matches none, so nothing is marked. Harmless; an optional
  "(recording)" note could be added.
- **Stored addressee.** A recorded message renders as a normal human message;
  its snapshot and model-context data retain `addressed_to = -`. Optional later
  polish: `encode_shared_entry` (`src/agents/model_context.cpp:15`) could omit
  `addressed_to` for the null agent so the JSONL reads as pure ambient talk.
- **`/mcast`, multicast.** Unchanged; `-` is not a resolvable character, so a
  stray `-` there falls through to the existing "unknown character" notice.

## What does not change

- The `@@` literal-at escape (`src/web/text_mention.cpp:16`) — the reason `-`
  was chosen.
- Command grammar (`src/web/text_command.cpp` already parses `/@-`), the
  generation executor, `ForumCharacters`, model backends, `runtime_info`. `-`
  never becomes a member and never acquires a backend.
- `config.toml` semantics and open-time default validation
  (`session_controller.cpp:222`): only real characters are ever persisted, so
  a stored default is always resolvable.
- `to_snapshot` / the SSE broadcast: a recorded entry is an ordinary human
  entry and flows through the existing snapshot path.

## Alternatives considered

- **`/@-` persisted like `/@Name`.** Would make `-` a forum's saved default so
  every new session starts in recording mode — a legitimate "journaling forum"
  use — but requires open-time validation and the UI to represent a
  null-agent default, and risks a forum left silently in recording mode.
  Deferred; **session-local chosen**, matching the session-scoped `/provider`
  and `/style` precedent with far less plumbing.
- **`-` as a real `ForumCharacters` member with a dummy backend.** Would force
  a fake entry through `runtime_info`, `format_characters_notice`, and the
  executor — machinery for a participant that never generates. Rejected;
  contradicts the smallest-change rule.
- **A hidden `"Ok."` reply to keep request/response pairs.** The original
  instinct, based on the belief that models require alternation. They do not
  (projection is per-entry; shared history already emits consecutive same-role
  messages), the DB does not (null-`request_id` entries are legal), and a real
  `"Ok."` would pollute the next character's shared history with junk. Rejected
  — no reply is created at all.
