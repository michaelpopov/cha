# Design: forum-scoped persona in agent system prompts

## Problem

Today every agent's system prompt embeds the **entire workspace persona
roster**: `append_participant_roster()` (`src/agents/character.cpp:190`)
appends a `## Participants` section with every persona's display name and full
`PERSONA.md` text to every character definition, for every forum.

Each forum already has a configured `default_persona` in its `config.toml`.
The goal: a session's agents should carry **only that one persona's**
description in their system prompts, and a `/!Name` persona switch must take
effect **immediately**, not just for the next session.

## Current behavior (relevant facts)

- Prompt assembly: `load_character_definitions()` builds
  `CHARACTER.md` + `FORUM.md`, then `append_standard_prompt_context()` appends
  the participant roster and the generated forum-context section.
- Definitions are assembled **twice**: at workspace load
  (`WorkspaceDefinition::load` → `load_forum_definitions`,
  `src/workspace/workspace_definition.cpp:472`) and again at **every session
  open** via `copy_definitions_for()` (same file, line 894), which re-reads
  the forum's files from disk and falls back to the startup copy (with a user
  notice) if the reload throws.
- The session's starting persona is resolved at open time by
  `forum_default_persona()`, which re-reads `default_persona` from the forum's
  `config.toml` for currency, validates it against the workspace roster, and
  falls back to the startup-loaded value with a log warning.
- `/!Name` (`SessionController::set_default_persona`,
  `src/session/session_controller.cpp:775`) resolves the handle against the
  controller's **full** roster, switches the session's current persona, and —
  on success — the new persona ID is persisted to the forum's `config.toml`
  (`persist_default_persona`, wired in `src/workspace/session_open.cpp:58`,
  executed in `src/web/live_session.cpp:481` before the command reply
  completes). So `/!Name` is already a durable forum-default change, not a
  session-local one.
- The built-in Entrance forum already loads with a single-persona roster
  (`PersonaRoster{builtin_guest()}`) — the proposed behavior is precedent, not
  novelty.
- There is an existing **reload fan-out** for character-settings saves:
  `request_reload()` (`src/web/lobby_routes.cpp:169`) shuts down every live
  session of the affected forums with `ShutdownReason::reloading` via
  `LiveSessionManager::active_sessions()` (which deliberately includes
  still-Starting actors). The server never reopens anything; the browser's
  stream-recovery ladder reopens sessions automatically, showing "Applying
  character settings…". On reopen the transcript is restored from the session
  journal — no history loss.

## Proposed design

### 1. Prompt assembly: filter the roster to the forum's default persona

At both definition-load points, pass a **one-element `PersonaRoster`**
containing the forum's default persona instead of the full workspace roster:

- **Startup** (`WorkspaceDefinition::load`): resolve
  `forum.info.default_persona_id` (already validated at load) via
  `find_persona()` and pass `{*persona}` to `load_forum_definitions()`.
- **Session open** (`copy_definitions_for()`): resolve
  `forum_default_persona(forum_id)` — the disk-current value, the same
  resolution `open_session()` uses for the session's starting persona — and
  pass that single persona. Prompt persona and session persona therefore
  always agree.
- **Fallback path**: if the session-open reload throws, the startup copy is
  used, embedding the startup-time default persona; the existing
  "settings from startup" notice already covers this degraded mode.

The `agents/` layer (`load_character_definitions`,
`append_participant_roster`) is untouched — it already accepts any roster.
The `## Participants` section simply contains one entry. This matches the
Entrance forum's existing shape.

### 2. `/!Name`: persist, then reload the forum's live sessions

Reuse the character-settings reload path so the switch takes effect
immediately:

1. The owner thread persists the new default persona to `config.toml`
   (existing behavior). On failure the `persist_default_persona_id` flag is
   reset and the notice gains `"(not saved)"` — **no reload happens** in that
   case.
2. In the input route (`POST /api/v1/input`, `src/web/session_routes.cpp`),
   when the completed `CommandResult` still carries
   `persist_default_persona_id` (i.e. the write committed), fan out
   `request_shutdown(ShutdownReason::reloading)` to all live sessions of that
   forum — including the current one and any still Starting.
3. The browser's existing stream recovery reopens each session. Reopen runs
   `open_session()` → `copy_definitions_for()` → prompts rebuilt with the new
   persona, and the session starts on that persona. Transcript restores from
   the journal.

The fan-out runs on the HTTP route thread, preserving the one-way lock
relationship (manager → actor; actors never call into the manager).

### Resulting semantics

- `/!Name` = switch speaker, persist forum default, restart the forum's live
  sessions. The user sees a brief "Applying settings…" and the conversation
  continues with agents that carry the new persona's description.
- Prompt persona, session starting persona, and persisted forum default can
  never diverge through normal use.

### Edge cases (all follow existing patterns)

- **Busy session**: `set_default_persona` already refuses while generating;
  no persist, no reload. *Sibling* sessions on the forum may be torn down
  mid-generation — the same accepted tradeoff as character-save reloads.
- **Re-selecting the current persona**: `set_default_persona` skips the
  snapshot when `/!Name` names the persona already in effect, so
  `persist_default_persona_id` stays empty and no persist or reload happens —
  the confirming notice is still shown. This mirrors the character-settings
  path, which reloads only when the saved values actually change.
- **Persist failure**: no reload; session continues with old prompts and the
  `"(not saved)"` notice.
- **Sessions opened after the write** read the new config naturally; sessions
  still Starting are included in the fan-out because they may already have
  read their definitions (this is why `active_sessions()` exists).
- **Runtime-only state is lost on reload**: session-scoped `/provider`
  overrides revert to configuration; in-flight background multicast output is
  volatile (already documented).
- **`reloading` reason** outranks `browser_disconnected`, so it reaches the
  wire and suppresses Retry buttons.

### What does not change

- The controller's runtime persona roster stays the full workspace roster:
  handle resolution, ambiguity errors, message attribution, and the
  `/api/v1/personas` endpoints are unaffected.
- `validate_persona_character_collisions()` still runs against the full
  roster at startup.
- `append_participant_roster()` and the `## Participants` section format.
- `/@Name` (default character): no prompt content depends on it, so no
  reload — unchanged.

### Alternatives considered

- **Defer roster appending to session open** (assemble prompts without the
  roster at load, append in `open_session`): removes the fallback-path
  staleness but splits prompt assembly across `agents/` and `workspace/`,
  breaking the "session/ decides *which* directories, agents/ decides *how*"
  boundary. Rejected as more machinery for a marginal gain.
- **Names for all personas, full text only for the default**: keeps agents
  aware of other speakers, but other speakers' names already appear via
  `from <Name>:` prefixes and shared-history JSONL. Rejected as two policies
  where one suffices.
- **Rebuild system prompts mid-session on persona switch**: busts provider
  prompt caches and adds real machinery. Rejected; the reload fan-out
  achieves the same effect through an existing, tested path.
