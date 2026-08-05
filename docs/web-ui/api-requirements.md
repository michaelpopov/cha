# Web API implications

This document maps the agreed UI to the C++ web boundary. It describes the
successful browser flow for a valid workspace. Configuration, storage, and
invariant failures use the server's existing exception boundary; the UI does not
require recovery-specific API models.

## Existing capabilities

| UI need | Existing API |
| --- | --- |
| List personas | `GET /api/v1/personas` |
| List forums | `GET /api/v1/forums` |
| List one forum's sessions | `GET /api/v1/forums/{forum}/sessions` |
| Create a stored session | `POST /api/v1/forums/{forum}/sessions` |
| Open or reattach a session | `POST /api/v1/forums/{forum}/sessions/{session}/open` |
| Load live chat state | `GET /s/{forum}/{session}/api/v1/session` |
| Stream chat changes | `GET /s/{forum}/{session}/api/v1/events` |
| Submit input | `POST /s/{forum}/{session}/api/v1/input` |
| Change the default character | `POST /s/{forum}/{session}/api/v1/actions/default-agent` |
| Stop generation | `POST /s/{forum}/{session}/api/v1/actions/stop` |

The existing live-session API remains the base. The additions below supply the
designed navigation and startup data.

## Startup and discovery

Add a consolidated bootstrap response:

```json
{
  "initial_persona_id": "builtin-guest",
  "initial_forum_id": "builtin-entrance",
  "initial_session_id": "builtin-welcome",
  "personas": [],
  "characters": [],
  "forums": [],
  "recent_sessions": []
}
```

The server loads and validates discovery data at startup. Its HTTP projection
contains workspace entities plus Guest, Assistant, and Entrance. The browser
opens the shared Welcome session immediately. Configuration changes take effect
after a server restart.

Terminal and HTTP discovery are separate projections over the same workspace
entities. Terminal commands use public names; HTTP routes use stable IDs. The
projections reuse domain values and validation but not presentation policy.

Use the same summary entities wherever their fields fit. Do not introduce
ID-only reference variants solely to avoid repeating display data.

## Persona attribution

Personas are application-wide authors, not forum or session members. Every input
request carries the selected persona ID:

```json
{"persona":"reader","text":"Hello"}
```

The HTTP boundary resolves that ID to the server-owned ID and display name
before queueing the command. The session records the resolved identity and does
not hold or consult a persona roster. Guest can therefore author a message in
any forum.

Persona prompt context may be captured when a session opens. It is model context
only and never controls message attribution.

## Reusable summaries

A persona summary contains `id`, `display_name`, and its configured optional
`description`. It never contains `PERSONA.md` prompt text.

A character summary contains:

```json
{
  "id": "epictetus",
  "display_name": "Epictetus",
  "description": "A demanding Stoic teacher of freedom and judgment"
}
```

The Characters roster, forum membership, and live session snapshots reuse this
summary.

A forum summary contains:

```json
{
  "id": "stoics",
  "display_name": "The Stoics Forum",
  "default_character_id": "epictetus",
  "members": [
    {
      "id": "epictetus",
      "display_name": "Epictetus",
      "description": "A demanding Stoic teacher of freedom and judgment"
    }
  ]
}
```

Members are sorted by display name. Forums have no description.

## Character detail

Add:

```text
GET /api/v1/characters
GET /api/v1/characters/{character_id}
```

The detail response contains the character summary fields plus
`character_markdown`. For workspace characters this is the definition
`characters/<id>/CHARACTER.md`. Assistant uses the embedded application guide.

The short description comes from `character.toml`. There is no `DISPLAY.md`,
separate display template, or additional character entity. Character browsing
does not load provider configuration or forum-local overrides.

The browser renders a restricted Markdown subset. Headings, paragraphs,
emphasis, lists, inline code, and code blocks are supported. Links are not
interactive, images are not fetched, and raw HTML does not execute.

## Sessions and Recent

Add `updated_at` to each session listing. Stored-session rows contain ID, label,
live state, and time; they contain no description or transcript excerpt.

Bootstrap includes all sessions across forums, ordered by database mtime, with
Welcome first:

```json
{
  "forum_id": "stoics",
  "session_id": "2026-08-02-10-30-00-session",
  "session_label": "Design review",
  "updated_at": "2026-08-02T17:42:00Z",
  "live": true
}
```

The browser resolves the forum display from the forum roster. It trims the New
session name and submits only a non-empty label.

## Shell and opening

The server serves the same client-routed shell at `/` and
`/s/{forum}/{session}/`. Successful open returns:

```json
{"forum_id":"stoics","session_id":"2026-08-02-10-30-00-session"}
```

The browser changes its active conversation without a full page load.

## Chat status

No separate endpoint is needed.

| Field | Source |
| --- | --- |
| Forum | Active session's forum summary |
| From | Current client persona selection |
| To | Live `default_character_id`, resolved through the character summaries |

## Client-owned state

Sidebar visibility, Navigation state, selected persona, selected forum,
inspected character, and the highlighted Recent row require no mutation API.

## Non-requirements

- No create, edit, or delete endpoints for forums, personas, or characters.
- No forum description.
- No session description or transcript excerpt.
- No forum links from Character detail.
- No provider settings, secrets, or forum-local prompt overrides.
- Settings and attachments remain outside the API contract.

Invalid state uses the server's exception boundary and has no separate browser
API model.
