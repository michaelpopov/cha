# Plan: the Null agent (`-`) — record-only monologue

Implements `docs/design.md`. Two ways to record: `@- <text>` for one message,
and `/@-` to enter a **session-local** recording mode where plain messages are
recorded. Recorded messages show in the UI, stay in the transcript, send nothing
to a model, and reach the next character as shared history. The change is
confined to character validation, the controller, one new journal method, one
web-layer persist guard, and a small webapp indicator — no grammar, executor, or
schema changes.

## Step 1 — Constants and reserve the sentinel

Files: `src/chat/transcript.h`, `src/agents/character.cpp`

Add next to `notice_display_name` (line 17), visible to controller and web
layer:

```cpp
inline constexpr std::string_view null_agent_handle = "-";
inline constexpr std::string_view null_agent_name   = "-";
```

Then include `chat/transcript.h` from `src/agents/character.cpp` and make `-`
unavailable to real characters at every runtime construction boundary:

1. `validate_character_id()` rejects an id exactly equal to
   `null_agent_handle`.
2. `validate_character_display_name_syntax()` rejects a display name exactly
   equal to `null_agent_name`. Use the syntax-level validator so trusted and
   built-in paths cannot bypass the reservation.

Both errors should say that `-` is reserved for the null agent. This is an
intentional compatibility rule: an existing character whose id or display name
is `-` must be renamed instead of having its prompts silently intercepted.

## Step 2 — Journal: persist a lone entry

Files: `src/session/session_database.h`, `src/session/session_database.cpp`

1. Declare `void record_entry(const TranscriptEntry& entry);` on
   `SessionJournal` (`session_database.h:100`, next to `start_turn`).
2. Implement it near `complete_turn` (`session_database.cpp:915`):

   ```cpp
   void SessionJournal::record_entry(const TranscriptEntry& entry) {
       Transaction transaction(impl_->database);
       insert_entry(impl_->database, current_epoch(impl_->database), entry);
       transaction.commit();
   }
   ```

   `insert_entry` (`session_database.cpp:555`) already validates storability and
   advances `next_entry_id`; a null `request_id` is exempt from
   `one_prompt_per_turn`. No new SQL, no schema change.

## Step 3 — Controller: record, and recognise the sentinel

Files: `src/session/session_controller.cpp`, `src/session/session_controller.h`

1. Declare the private helper in `session_controller.h` (near `resolve_author`,
   line ~177):

   ```cpp
   void record_monologue(std::string_view author_id, std::string text,
                         ControllerUpdate& update);
   ```

   Implement it as in `docs/design.md` §1: resolve the author, build a human
   entry addressed to `{null_agent_handle, null_agent_name}` with no
   `request_id`, `journal_.record_entry(entry)` then `transcript_.add_entry`,
   set `input_consumed`, set `notice` to the empty string, and
   `require_snapshot`. Empty text → "Message to @- is empty" notice. Clearing
   the notice on success prevents an earlier error from remaining visible and
   matches normal prompt activation.

2. In `submit_prompt()` (`session_controller.cpp:321`), add two short-circuits:
   - Empty-handle branch (line 334), before
     `characters_.find(default_character_id_)`:
     `if (default_character_id_ == null_agent_handle) { record_monologue(author_id, std::move(text), update); return update; }`
   - Non-empty-handle branch, before `resolve_handle()` (line 337):
     `if (handle == null_agent_handle) { record_monologue(author_id, std::move(text), update); return update; }`

   Keep the existing `busy()` guard and the early return for `text.empty() &&
   handle.empty()` at the top of `submit_prompt`. Thus an empty inline `@-`
   reports the helper's notice, but an empty plain submission in recording mode
   remains a silent no-op.

3. In `set_default_character()` (`session_controller.cpp:747`), before
   `resolve_handle()`: `if (handle == null_agent_handle)` set
   `default_character_id_ = std::string(null_agent_handle)`, `require_snapshot`,
   and set the recording-mode notice. Return without touching the roster.

## Step 4 — Web layer: don't persist the sentinel default

File: `src/web/text_input.cpp`

In the `CommandKind::set_default` case (line 106), guard the persist so `-` is
never written to `config.toml`:

```cpp
if (requires_snapshot(result.session)
    && controller.view().default_character_id != null_agent_handle) {
    result.persist_default_character_id =
        std::string(controller.view().default_character_id);
}
```

No change to the text branch: `@- …` and plain messages in recording mode both
flow through `submit_prompt` and clear the input on `input_consumed`
(`text_input.cpp:52`).

## Step 5 — Webapp: show recording mode

File: `webapp/src/components/Screens.tsx`

When `state.currentDefaultCharacterId === '-'` (the lookup at line 128 yields no
member), the composer already falls back to "Message character" (line 363) and
the character `<select>` (line 350) has no matching option. Make it legible:

1. Composer placeholder → a recording label (e.g. `Recording — saved, not
   sent`) when the current default is `-`.
2. Only when the current default is `-`, prepend a selected
   `<option value="-">Recording</option>` so the controlled `<select>` has a
   matching value. Do not render that option for a real default. It is a
   display-only representation of the current mode, not another way to enter
   it; selecting a real character while recording continues through the
   existing `set_default_character_by_id` path and persists that real id.

Keep it small; the fallback already prevents a broken render, this just names
the state. Do not change `set_default_character_by_id` or the typed command path
in `live_session.cpp`: only `/@-` enters recording mode, so those paths never
receive the sentinel.

## Step 6 — Help text and docs

1. `README.md`: after the command table / mention paragraph, document `@-
   <text>` (records without sending) and `/@-` (session-local recording mode;
   `/@<name>` resumes). Note `/@-` does not save `-` as the forum default,
   unlike `/@Name`; messages recorded while it is active are still durable.
2. `src/session/README.md`, `src/chat/README.md`: one line each — the
   null-target `-` records a human entry with no turn via `record_entry`, and
   projects as shared history; `/@-` is session-scoped like `/provider`.
3. `docs/web-ui/behavior.md`: a `@-` / recording-mode message renders as a
   normal human entry addressed to `-`, with no reply; the composer shows the
   recording state.

## Step 7 — Tests

1. Character validation: reject `-` as both a real character id and display
   name, including the trusted/syntax-only display-name path.
2. `tests/chat/unit_transcript.cpp`: a human entry addressed to `-` validates
   and carries no `request_id`.
3. Journal round-trip (e.g. `tests/session/unit_session_repository.cpp`):
   `record_entry` stores the entry, advances `next_entry_id`, creates no `turns`
   row, and `load_session_state` restores it with `request_id == nullopt`.
4. `tests/session/unit_session_controller.cpp`:
   - `submit_prompt(author, "hi", "-")` adds exactly one human entry addressed
     to `-`, sets `input_consumed`, clears the notice, starts **no** generation,
     and snapshots.
   - `set_default_character("-")` switches to recording mode (snapshot +
     notice); a subsequent plain `submit_prompt(author, "hi", "")` records via
     the same path and starts no generation; then
     `set_default_character("<real>")` resumes normal dispatch.
   - Empty `@-` yields the "empty" notice and no entry; an empty plain submission
     in recording mode is a no-op; `@-` while generating is refused by `busy()`.
5. `tests/web/unit_text_input.cpp`: after
   `set_default_character("-")`, no `persist_default_character_id` is produced;
   after `set_default_character("<real>")`, it is.
6. `tests/agents/unit_model_context.cpp`: a `-`-addressed entry projects into
   the `Shared chat history (JSONL):` block for a different character (not as a
   first-person message); several project as consecutive shared entries.
7. Webapp: extend a `LiveChat`/`Screens` test to assert that when
   `default_character_id === '-'` the recording placeholder and selected
   `Recording` option appear, choosing a real character calls the existing typed
   setter, and the sentinel option is absent when a real character is current.

## Step 8 — Build and verify

```sh
make test
make web-check
```

Manual smoke (`make run`): open a forum session. `@- first thought` records with
no reply and clears the input. `/@-` → composer shows recording mode; send two
plain thoughts, both recorded with no reply. `/@<name>` → resume; ask something
and confirm from the reply the character is aware of the thoughts as background
context. Reopen the session: the recorded messages are still present and the
default is the real character again (recording mode was session-local).

## Out of scope / accepted tradeoffs

- **`/@-` does not persist.** Recording mode is session-local; a persisted
  "journaling forum" default is deferred (see `docs/design.md` Alternatives).
- **No hidden reply.** No `"Ok."` entry; request/response pairing is
  unnecessary at every layer.
- **Stored addressee remains `-`.** The web UI still renders an ordinary human
  message; the optional `encode_shared_entry` tweak to omit `addressed_to` from
  shared-history JSONL for the null agent is a later cosmetic change.
