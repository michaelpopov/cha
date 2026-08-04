# Web API implications

This document maps the agreed UI to the current C++ web boundary. Route behavior is implemented in [lobby_routes.cpp](../../src/ui/web/lobby_routes.cpp), [session_routes.cpp](../../src/ui/web/session_routes.cpp), and [protocol.h](../../src/ui/web/protocol.h).

## Existing capabilities

| UI need | Existing API |
| --- | --- |
| List personas | `GET /api/v1/personas` returns stable IDs and display names |
| List forums | `GET /api/v1/forums` returns stable IDs and display names |
| List one forum's sessions | `GET /api/v1/forums/{forum}/sessions` returns ID, label, and live state; it correctly has no description field |
| Create a stored session | `POST /api/v1/forums/{forum}/sessions` with `{"label":"..."}` |
| Open or reattach a session | `POST /api/v1/forums/{forum}/sessions/{session}/open` |
| Load live chat state | `GET /s/{forum}/{session}/api/v1/session` returns forum identity, character roster, and `default_character_id` with the transcript |
| Stream chat changes | `GET /s/{forum}/{session}/api/v1/events` |
| Submit as the current persona | `POST /s/{forum}/{session}/api/v1/input` with persona ID and text |
| Change a live session's default character | `POST /s/{forum}/{session}/api/v1/actions/default-agent` with `character_id` |
| Stop generation | `POST /s/{forum}/{session}/api/v1/actions/stop` |

The existing per-input persona ID matches the design: changing Personas affects the next message without reopening the session. The live snapshot already contains enough information to render the Chat status line's Forum and To values after a session is open.

## Required additions or changes

### Defaults

The application needs configured default persona and forum IDs. The browser must receive their resolved IDs and display names during startup. Both values must be validated against the loaded workspace.

Before a session is open, the default forum metadata must also provide its resolved default character. Once a session is live, `SessionSnapshot.default_character_id` is authoritative because the live default can change.

### Forum membership metadata

The Forums screen needs each forum's ordered member roster and configured default character without opening a session. Extend the forum summary or provide equivalent bootstrap data:

```json
{
  "id": "stoics",
  "display_name": "The Stoics Forum",
  "members": [
    {"id": "epictetus", "display_name": "Epictetus"},
    {"id": "markus_aurelius", "display_name": "Marcus Aurelius"},
    {"id": "seneca", "display_name": "Seneca"}
  ],
  "default_character_id": "epictetus"
}
```

Forum summaries have no description field. Member names are display-only text in this version.

### Workspace characters

The Characters screens need a workspace-level character API. A summary list should return stable ID, display name, and a short description; a detail request should return the raw `CHARACTER.md` text without opening a forum or session.

Recommended boundary:

```text
GET /api/v1/characters
GET /api/v1/characters/{character}
```

Provisional responses:

```json
[
  {
    "id": "epictetus",
    "display_name": "Epictetus",
    "description": "A demanding Stoic teacher of freedom and judgment"
  }
]
```

```json
{
  "id": "epictetus",
  "display_name": "Epictetus",
  "description": "A demanding Stoic teacher of freedom and judgment",
  "character_markdown": "# Epictetus\n\n..."
}
```

The assumed `description` key is not yet defined in `characters/<id>/character.toml`. It is workspace metadata and should not be forum-overridable. Exact validation and fallback behavior remain an implementation decision.

The browser renders a restricted Markdown subset. Headings, paragraphs, emphasis, lists, inline code, and code blocks are supported. Links must not become interactive, images must not be rendered or fetched, and raw HTML must not execute. Returning raw Markdown keeps sanitization policy in one client renderer; returning pre-rendered HTML would require an equally strict server-side sanitizer.

### Persona descriptions

The approved Personas screen shows a short description below each display name, but the current persona protocol exposes only ID and display name. The lobby payload therefore needs a safe short-description field or the design needs an explicit empty-description fallback. Persona prompt text must remain private and must not be reused as the summary.

### Recent sessions

The current API lists sessions only within one forum and provides no cross-forum recency ordering. The sidebar needs an aggregate endpoint or equivalent bootstrap data containing at least:

```json
[
  {
    "forum_id": "lobby",
    "forum_display_name": "The Lobby",
    "session_id": "2026-08-02-10-30-00-session",
    "session_label": "Design review",
    "updated_at": "2026-08-02T10:42:00-07:00",
    "live": true
  }
]
```

The server should aggregate and order this list. Requiring the browser to fetch every forum's sessions would create an avoidable fan-out and still would not provide reliable recency data. Recent entries contain no session description.

### Forum-session time metadata

The approved Sessions rows show compact relative time at the trailing edge. The existing `SessionListing` has only ID, label, and live state, so it needs an `updated_at` value or equivalent sortable time metadata. It must not gain a description or transcript-excerpt field.

### Session-name validation

The agreed UI requires a non-empty trimmed session name. The server should enforce the same rule rather than relying only on the browser.

### Startup payload

A consolidated startup response is recommended so the browser can render the sidebar and workspace navigation without sequencing several dependent requests. A provisional shape is:

```json
{
  "default_persona_id": "reader",
  "default_forum_id": "lobby",
  "personas": [],
  "characters": [],
  "forums": [],
  "recent_sessions": []
}
```

Character detail Markdown can remain a separate request so startup does not transfer every `CHARACTER.md`. This can be a new bootstrap endpoint or an extension of the current lobby boundary. The exact route name is an implementation decision.

## Chat status derivation

No separate mutation endpoint is required for the status line.

| Status field | Source before a live session | Source in a live session |
| --- | --- | --- |
| Forum | Selected/default forum summary | `SessionSnapshot.forum.display_name` |
| From | Current client persona selection | Current client persona selection |
| To | Forum summary `default_character_id`, resolved through `members` | `SessionSnapshot.default_character_id`, resolved through `characters` |

An accepted default-character mutation is reflected by the next authoritative snapshot or event and updates To.

## Client-owned state

The following state does not require a server mutation endpoint:

- Sidebar open or closed.
- Current Navigation screen.
- Inspected Character detail ID.
- Current persona selection, provided its ID is included with each input.
- Current forum selection while browsing.
- Which Recent row is highlighted, derived from the active session route.

Browser persistence for persona and forum selections is optional; server-provided defaults remain the fallback.

## Explicit non-requirements

- No create, edit, or delete endpoints for forums, personas, or characters.
- No forum description.
- No session description or transcript excerpt in session listings.
- No forum membership links from Character detail.
- No need to expose character provider settings, secrets, expanded prompts, or forum-local overrides.

## Still unresolved

- Where default persona and default forum are configured.
- Whether the initial default-forum Chat is a draft, an immediately persisted session, or a reusable default session.
- The Settings data model and mutation endpoints.
- The maximum Recent count and pagination or overflow strategy.
- Whether duplicate session display names are allowed within a forum.
- Validation and fallback rules for persona and character short descriptions.

