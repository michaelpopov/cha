# Plan: forum-scoped persona in agent system prompts

Implements `~/design.md`. Two changes: (A) load only the forum's default
persona into agent system prompts, (B) reload a forum's live sessions when
`/!Name` commits a new default persona.

## Step 1 — One-persona roster at definition load

File: `src/workspace/workspace_definition.cpp`

1. In `WorkspaceDefinition::load()` (forum loop near line 784): resolve the
   forum's default persona with `find_persona(forum.info.default_persona_id)`
   (validated earlier in load, so it cannot be absent) and pass a one-element
   `PersonaRoster` to `load_forum_definitions()` instead of `*model.personas_`.
2. In `copy_definitions_for()` (near line 894): resolve the disk-current
   persona with the existing `forum_default_persona(forum_id)`, look it up
   with `find_persona()`, and pass the one-element roster to the reload call.
   `forum_default_persona()` already validates and falls back to the
   startup-loaded value, so the lookup cannot fail.
3. Keep `load_forum_definitions()`'s signature (`const PersonaRoster&`)
   unchanged — the filtering happens at the two call sites. The `agents/`
   layer needs no changes.

No change to `builtin_assistant_definitions()` — the Entrance already gets
`{builtin_guest()}`.

## Step 2 — Reload fan-out after `/!Name` persist

Files: `src/web/lobby_routes.cpp`, `src/web/route_support.h`,
`src/web/route_support.cpp`, `src/web/session_routes.cpp`

1. Move `request_reload()` (currently a static in `lobby_routes.cpp:169`)
   into `route_support` so both route modules share it. Keep its exact
   behavior: iterate `LiveSessionManager::active_sessions()`, filter by
   forum ID, `request_shutdown(ShutdownReason::reloading)`.
2. In the `POST /api/v1/input` handler (`session_routes.cpp:144`): capture
   the `CommandSubmitResult` from `submit()`; if it holds a `CommandResult`
   whose `persist_default_persona_id` is set (present ⇒ the `config.toml`
   write committed on the owner thread; reset ⇒ "(not saved)"), call
   `request_reload(*live_sessions, {key->forum_id})`, then
   `set_command_result(...)` as before.
   - This covers the current session and all siblings on the forum,
     including still-Starting actors.
   - The fan-out runs on the route thread, preserving the manager→actor
     lock direction.
   - No new `ShutdownReason`; `reloading` already has the right wire
     priority and browser treatment.

## Step 3 — Webapp wording

File: `webapp/src/components/Screens.tsx` (line ~97)

Generalize the `reloading` message from `'Applying character settings…'` to
`'Applying settings…'` (persona switches now trigger the same path). Update
the matching assertion in `webapp/src/components/LiveChat.test.tsx`.

## Step 4 — Tests

1. `tests/application/unit_workspace_definition.cpp`: assert that a forum's
   loaded definitions contain the forum's default persona's `PERSONA.md`
   text under `## Participants` and **not** another workspace persona's.
   Cover two forums with different default personas if the fixture allows.
2. `tests/application/unit_session_open.cpp` (or the workspace test): after
   `persist_forum_default_persona()` to a different persona,
   `copy_definitions_for()` (via opening a session) yields prompts containing
   the new persona.
3. `tests/web/process_web_server.cpp`: mirror the existing character-PATCH
   reload test (line ~1066 asserts `shutdown_reason == "reloading"`): submit
   `/!Name` through the input endpoint and assert the session's stream
   reports `reloading`; assert a second live session on the same forum is
   also shut down, and a session on a *different* forum is not (compare
   line ~1125).
4. Existing tests that must keep passing unchanged:
   `tests/agents/unit_character_definition_loader.cpp` (roster agnostic),
   `tests/application/unit_builtins.cpp` (Entrance single-persona roster).

## Step 5 — Documentation

1. `src/agents/README.md`: the "four sections" paragraph and the sentence
   "The roster a forum's characters receive is the whole workspace roster,
   because `/!Name`…" — replace with: the roster is the forum's configured
   default persona only; `/!Name` persists the choice and reloads the
   forum's live sessions, so prompts always match the session's persona.
   The diagram's `roster` node already reads
   `personas/<forum default_persona>/` — verify it still matches.
2. `docs/tutorial.md` §8.3 ("the workspace personas and their `PERSONA.md`
   prompts") → the forum's default persona only.
3. `src/web/README.md`: note that `/!Name` saves the forum default and
   reloads the forum's live sessions (extend the existing "persona changes
   only through `/!Name`" passage).
4. `docs/web-ui/api-requirements.md`: persona-attribution section — a
   successful `/!Name` shuts the forum's live sessions down with
   `shutdown_reason: "reloading"`; the browser's stream recovery reopens
   them.
5. `docs/web-ui/behavior.md`: the `reloading` chat message is now
   "Applying settings…" and also appears after a persona switch.
6. `docs/web-ui/flows.md`: persona-attribution flow gains the reload edge.

## Step 6 — Build and verify

```sh
make test          # builds and runs the C++ suite
make web-check     # webapp typecheck/tests
```

Manual smoke: `make run`, open a forum session, `/!<other persona>` → chat
shows "Applying settings…", session returns with history intact, status line
shows the new persona, and agents answer with knowledge of only that
persona's description.

## Out of scope / accepted tradeoffs

- Sessions torn down mid-generation lose the in-flight answer (same as
  character-settings saves).
- Session-scoped `/provider` overrides revert on reload.
- Startup-fallback path (reload throws at session open) embeds the
  startup-time persona; covered by the existing fallback notice.
