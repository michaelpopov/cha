# Web API implications

This document maps the agreed UI to the current C++ web boundary. Route behavior is implemented in [lobby_routes.cpp](../../src/ui/web/lobby_routes.cpp), [session_routes.cpp](../../src/ui/web/session_routes.cpp), and [protocol.h](../../src/ui/web/protocol.h).

## Existing capabilities

| UI need | Existing API |
| --- | --- |
| List personas | `GET /api/v1/personas` returns stable IDs and display names |
| List forums | `GET /api/v1/forums` returns stable IDs and display names |
| List one forum's sessions | `GET /api/v1/forums/{forum}/sessions` returns ID, label, and live state |
| Create a stored session | `POST /api/v1/forums/{forum}/sessions` with `{"label":"..."}` |
| Open or reattach a session | `POST /api/v1/forums/{forum}/sessions/{session}/open` |
| Load live chat state | `GET /s/{forum}/{session}/api/v1/session` |
| Stream chat changes | `GET /s/{forum}/{session}/api/v1/events` |
| Submit as the current persona | `POST /s/{forum}/{session}/api/v1/input` with persona ID and text |
| Stop generation | `POST /s/{forum}/{session}/api/v1/actions/stop` |

The existing per-input persona ID matches the design: changing Personas affects the next message without reopening the session.

## Required additions or changes

### Defaults

The application needs configured default persona and forum IDs. The browser must receive their resolved IDs and display names during startup. Both values must be validated against the loaded workspace.

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

The server should aggregate and order this list. Requiring the browser to fetch every forum's sessions would create an avoidable fan-out and still would not provide reliable recency data.

### Session-name validation

The agreed UI requires a non-empty trimmed session name. The server should enforce the same rule rather than relying only on the browser.

### Startup payload

A consolidated startup response is recommended so the browser can render the sidebar without sequencing several dependent requests. A provisional shape is:

```json
{
  "default_persona_id": "reader",
  "default_forum_id": "lobby",
  "personas": [],
  "forums": [],
  "recent_sessions": []
}
```

This can be a new bootstrap endpoint or an extension of the current lobby boundary. The exact route name is an implementation decision.

## Client-owned state

The following state does not require a server mutation endpoint:

- Sidebar open or closed.
- Current Navigation screen.
- Current persona selection, provided its ID is included with each input.
- Current forum selection while browsing.
- Which Recent row is highlighted, derived from the active session route.

Browser persistence for persona and forum selections is optional; server-provided defaults remain the fallback.

## Still unresolved

- Whether the initial default-forum Chat is a draft, an immediately persisted session, or a reusable default session.
- The Settings data model and mutation endpoints.
- The maximum Recent count and pagination or overflow strategy.
- Whether duplicate session display names are allowed within a forum.

