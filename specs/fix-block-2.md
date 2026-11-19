# Fix block 2: Chat and providers

**Fixes:** F10, F15, F16, F17 · **Written:** November 19, 2026

## Before you start

- This file is complete. Do not read `docs/fix-plan.md`, the architecture
  reviews in `docs/`, or `specs/p0-d1-design.md`.
- **Start condition:** none. Start from the latest `main`.
- Other work (P0-D1, a D1 sync rework) is in progress. Do not change D1 sync
  code (`src/app/d1_sync.cpp`, `src/storage/sync_*.cpp`).
- File paths and line numbers come from commit `11ca9039`. Lines can move.
  Find the code by the function name.

## Approvals

- **ChaWeb permission:** all four fixes change `cha_lib` code that cha-daemon
  also runs. Before you change code, ask the user for explicit permission. If
  the user does not give it, stop. Run ChaWeb tests only with this permission.
- **F15 mapping table:** show it to the user and get approval before you write
  F15 code.
- **UI approval:** none. No visible text or control changes.

## Rules

- Make the smallest fix. Do not add machinery or new settings.
- Add only the tests that this file names. Run the tests of the changed area.
- Do not change `docs/` or `specs/`. The document updates are a separate task.
- Do not commit unless the user tells you to.

## Suggested order

F17, F16, F10, then F15 (F15 waits for the mapping approval).

## F10. Jev: start the first reply at once

**Decision (user, November 19):** start the reply when Jev picks a character.
Do not wait for the session name.

**Problem:** For the first message of an unnamed chat, Jev classification
(5 s limit) and the session name request (10 s limit) start together. The
reply starts only after both finish. The reply can wait up to 10 s, also when
Jev picks the character in 100 ms. Chats without Jev already name the session
in parallel with the reply.

**Fix:**

1. In `SessionController` (`src/session/session_controller.cpp:753, 789`),
   start the reply when classification gives a result.
2. Apply the name when it arrives: in `finish_classification` if it is
   ready, else in `poll_session_name`.
3. Make the cancel, expiry, and rejected-submission paths call
   `cancel_session_name()`. A late name must not rename a chat whose first
   message was cancelled.
4. Keep the current rule for the first name of a shared session.

**Test:** Change the existing behavior test (`tests/runtime/unit_jev.cpp:717`)
to the new order. Add one test: a cancelled first message gets no late name.

## F15. Thinking settings for Google and Anthropic

**Decision (user, November 19):** send thinking settings in the Google and
Anthropic provider requests.

**Problem:** The Chat Effort picker shows for Google and Anthropic
characters. Neither encoder sends thinking settings, so the value has no
effect. `workspace.cpp` logs that effort is ignored.

**Fix:**

1. Before code, write a short table that maps each effort value of the picker
   (find the values in the code) to the Gemini `thinkingConfig` and the
   Anthropic `thinking` request fields. Use the current API documentation of
   each provider. Show the table to the user and get approval.
2. Send the mapped values in `src/providers/google_api.cpp` and
   `src/providers/anthropic_api.cpp`.
3. Remove the "effort is ignored" warnings
   (`src/workspace/workspace.cpp:288-296`).
4. Keep `provider_supports_reasoning_effort` as it is. If it returns false
   for Google, Gemini Summarize fails for every effort value other than None.

**Test:** One encoder test for each provider: one effort value gives the
expected request field. Run the Gemini summary tests.

## F16. `max_completion_tokens` for OpenAI Chat Completions

**Problem:** Chat Completions sends `max_tokens` when `config.max_tokens` is
set (`src/providers/chat_completions_api.cpp:179-181`). OpenAI reasoning
models reject it. This affects chats and the Flex test
(`src/notes/notes_operations.cpp:453`). It occurs only when `max_tokens` is
set by hand. Settings has no such field.

**Fix:** In the encoder, for direct OpenAI hosts, send
`max_completion_tokens` in place of `max_tokens`. Keep `max_tokens` for other
Chat Completions backends.

**Test:** One encoder test.

## F17. Drop a stale provider override at load

**Problem:** When the saved `provider_id` of a session names a deleted
provider, `initialize()` keeps the override and warns again on each turn.

**Fix:** Reset the missing `provider_id` at load
(`src/session/session_controller.cpp:382-387`). Do not change the
`model_locked` behavior.

**Test:** No new test.

## Not in this block

- Hardcoded Gemini Library model: no change. Change the constant only when
  Google retires the model. Do not add a setting.
- Provider override on a `model_locked` forum: no change. The forum provider
  already wins.
- SSE and response buffer limits, the multicast queue: no change.
- `body["include"]` overwrite, the tool-limit message, prompt rows per
  target: no change.
- Gemini store metadata calls and OpenAI store delete: no change.
- Do not change the Welcome web tools (`web_search`, `web_read`). They are
  required.

## Done when

- F10, F15, F16, and F17 are in the code.
- The F15 mapping table was approved before F15 code.
- The named tests pass, and the tests of the changed areas pass.
- Your final report says, for each fix, what changed and which tests ran.
