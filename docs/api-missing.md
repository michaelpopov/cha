# HTTP API gaps for the designed web UI

Status: API review completed 2026-08-03.

This document compares the current HTTP API with the agreed browser design in
[web-ui/](web-ui/README.md). The live-session API is mostly sufficient. The
blocking gaps are workspace bootstrap and discovery metadata: defaults,
Characters, forum membership, Recent sessions, and timestamps.

## Coverage summary

| Designed feature | Status | Missing |
| --- | --- | --- |
| Persona selection and message attribution | Partial | Persona IDs/names and per-message attribution work; short descriptions and the default persona do not exist |
| Characters list | Missing | No workspace Characters route and no short-description field |
| Character detail | Missing | No route or defined browser-safe `CHARACTER.md` representation |
| Forums list | Partial | Forum IDs/names work; members and the default character are omitted |
| Sessions list | Partial | IDs, labels, and live state work; timestamps are missing |
| Session descriptions | Complete | The API correctly has no description field |
| Session creation and opening | Mostly complete | The server does not enforce the designed trimmed, non-empty name rule |
| Recent sessions | Missing | No cross-forum aggregate or recency metadata |
| Live chat and status line | Mostly complete | Active-session Forum/To data exists; pre-session defaults are missing |
| Sidebar and Navigation state | Complete client-side | No API is needed |
| Settings | Undetermined | Settings content has not been designed |

The current lobby routes expose only forums, personas, per-forum sessions,
session creation, and session opening. See
[`lobby_routes.cpp`](../src/ui/web/lobby_routes.cpp). Their DTOs are
correspondingly narrow: forum and persona summaries contain only ID and display
name, while session listings contain only ID, label, and live state. See
[`protocol.h`](../src/ui/web/protocol.h).

## Required API work

### 1. Add workspace bootstrap and defaults

The UI needs configured `default_persona_id` and `default_forum_id` values.
`ApplicationConfig` currently contains only host, port, and logging settings.
See [`workspace.h`](../src/session/workspace.h) and
[`workspace.cpp`](../src/session/workspace.cpp).

A bootstrap response should provide:

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

This could be `GET /api/v1/bootstrap` or equivalent composed requests. A single
response avoids dependent startup requests and per-forum fan-out. The exact
route name remains an implementation decision.

Both configured defaults must be resolved and validated against the loaded
workspace before they are returned.

### 2. Expose forum members and the default character

The underlying `Forum` already contains ordered member IDs and
`default_agent_id`, but the HTTP `ForumSummary` discards both. The HTTP boundary
must resolve member display names and expose approximately:

```json
{
  "id": "stoics",
  "display_name": "The Stoics Forum",
  "default_character_id": "epictetus",
  "members": [
    {"id": "epictetus", "display_name": "Epictetus"},
    {"id": "markus_aurelius", "display_name": "Marcus Aurelius"},
    {"id": "seneca", "display_name": "Seneca"}
  ]
}
```

The required source data is visible in the `Forum` type in
[`workspace.h`](../src/session/workspace.h). No forum description should be
added. Member names are plain display text in this UI and do not need links or
member-detail routes scoped through a forum.

### 3. Add workspace Characters routes

Recommended minimum routes:

```text
GET /api/v1/characters
GET /api/v1/characters/{character_id}
```

The list needs stable ID, display name, and a short description. Character
detail needs the Markdown used by the Character detail screen.

Provisional list response:

```json
[
  {
    "id": "epictetus",
    "display_name": "Epictetus",
    "description": "A demanding Stoic teacher of freedom and judgment"
  }
]
```

Provisional detail response:

```json
{
  "id": "epictetus",
  "display_name": "Epictetus",
  "description": "A demanding Stoic teacher of freedom and judgment",
  "character_markdown": "# Epictetus\n\n..."
}
```

The typed character metadata currently contains only ID, display name, and
tags. See [`config.h`](../src/agents/config.h). The assumed `description` field
therefore requires a model/config-loader change as well as a new HTTP DTO. It is
workspace metadata and should not be forum-overridable. Exact validation and
fallback behavior remain to be defined.

The detail route must validate the character ID as a safe route component and
must not expose provider configuration, credentials, expanded forum prompts, or
forum-local overrides.

### 4. Define `CHARACTER.md` browsing semantics

This is not only an endpoint gap; the current content model is ambiguous for a
forum-independent browser screen.

Returning the raw root file will not necessarily produce the designed full
description. For example,
[`workspace/characters/epictetus/CHARACTER.md`](../workspace/characters/epictetus/CHARACTER.md)
mainly contains template includes. Existing prompt expansion also injects forum
variables and can select forum-local overrides. See
[`agent.cpp`](../src/agents/agent.cpp).

The API contract must choose one of these representations:

1. Raw workspace-level `CHARACTER.md` source.
2. Workspace-definition Markdown with includes expanded under a
   forum-independent browsing scope.
3. A separate display document distinct from the prompt template.

Option 2 is closest to the approved visual design, but it must define the
behavior of forum variables and must never apply a forum member override.

Whatever representation is selected, the browser must render a restricted
Markdown subset. Headings, paragraphs, emphasis, lists, inline code, and code
blocks are supported. Links must not become interactive, images must not be
rendered or fetched, and raw HTML must not execute. Returning Markdown rather
than pre-rendered HTML keeps this sanitization policy in one browser renderer.

### 5. Add persona short descriptions

The Personas panel shows a short descriptor below each display name, but the
current `Persona` contains only ID, display name, and private prompt text. See
[`persona.h`](../src/agents/persona.h). The HTTP summary likewise exposes only
ID and display name.

`persona.toml` currently rejects every field except `display_name`, so adding a
safe `description` requires an explicit loader and model change. See
[`workspace.cpp`](../src/session/workspace.cpp). `PERSONA.md` must not be reused
as the short description because it is prompt content.

### 6. Add Recent sessions and timestamp metadata

There is no cross-forum Recent endpoint. The browser could fetch every forum's
session list, but that would create startup fan-out and still would not provide
authoritative recency ordering.

A Recent entry needs at least:

```json
{
  "forum_id": "lobby",
  "forum_display_name": "The Lobby",
  "session_id": "2026-08-03-12-00-00-session",
  "session_label": "Design review",
  "updated_at": "2026-08-03T12:00:00-07:00",
  "live": true
}
```

The stored session metadata currently contains only ID, forum, and label. See
[`session_database.h`](../src/session/session_database.h). An authoritative
`updated_at` must therefore be persisted or derived under a clearly defined
filesystem policy.

The same time value can support the compact `Now`/`Tue` presentation in the
per-forum Sessions screen. `SessionListing` should gain an ISO timestamp or
equivalent sortable value. It must not gain a description or transcript
excerpt.

Recent entries also contain no description or transcript excerpt.

### 7. Enforce session-name validation on the server

The request parser accepts any string unchanged. See
[`json.cpp`](../src/ui/web/json.cpp). An empty label is then silently replaced
with the generated session ID, while a whitespace-only label remains
whitespace. See [`session_catalog.cpp`](../src/session/session_catalog.cpp).

The server must trim the submitted label and return `400 bad_request` when the
result is empty. Browser-side disabled-button behavior is not sufficient
validation.

## Chat status derivation

No separate status endpoint or mutation is needed.

| Status field | Before a live session | In a live session |
| --- | --- | --- |
| Forum | Selected/default forum summary | `SessionSnapshot.forum.display_name` |
| From | Current client persona selection | Current client persona selection |
| To | Forum summary `default_character_id`, resolved through `members` | `SessionSnapshot.default_character_id`, resolved through `characters` |

The current live snapshot already contains forum identity, the forum character
roster, and `default_character_id`, together with transcript and generation
state. See [`protocol.h`](../src/ui/web/protocol.h) and
[`session_projection.cpp`](../src/ui/web/session_projection.cpp).

Persona attribution is already correctly submitted with each prompt. A
successful default-character action marks session state as changed, and the web
runtime publishes authoritative updated state through SSE. See
[`session_controller.cpp`](../src/session/session_controller.cpp) and
[`web_session_runtime.cpp`](../src/ui/web/web_session_runtime.cpp).

## Existing API capabilities that should be retained

| Capability | Current boundary |
| --- | --- |
| List persona IDs and display names | `GET /api/v1/personas` |
| List forum IDs and display names | `GET /api/v1/forums` |
| List a forum's session IDs, labels, and live state | `GET /api/v1/forums/{forum}/sessions` |
| Create a stored session | `POST /api/v1/forums/{forum}/sessions` |
| Open or reattach a session | `POST /api/v1/forums/{forum}/sessions/{session}/open` |
| Load a live snapshot | `GET /s/{forum}/{session}/api/v1/session` |
| Stream snapshots and append events | `GET /s/{forum}/{session}/api/v1/events` |
| Submit text as the current persona | `POST /s/{forum}/{session}/api/v1/input` |
| Change a live session's default character | `POST /s/{forum}/{session}/api/v1/actions/default-agent` |
| Stop generation | `POST /s/{forum}/{session}/api/v1/actions/stop` |

The live chat transport does not need redesign to support the current visual
specification.

## Client-owned state and explicit non-requirements

No server mutation endpoint is needed for:

- Sidebar open or closed.
- Current Navigation screen.
- Inspected Character detail ID.
- Current persona selection, provided its ID accompanies each input.
- Current forum selection while browsing.
- The highlighted Recent row, derived from the active session route.

Do not add:

- Create, update, or delete endpoints for forums, personas, or characters.
- Forum descriptions.
- Session descriptions or transcript excerpts in listings.
- Forum membership links from Character detail.
- A separate chat-status endpoint.

Settings content remains undesigned, so no Settings API can be specified yet.

## Attachment-button inconsistency

The approved mockup currently contains a `+` button in the prompt composer, but
neither its behavior nor an attachment/upload API has been defined. The current
application has no corresponding HTTP or session-domain capability.

The button must be removed or disabled unless attachments become an explicit
feature with a separate design and API contract.

## Recommended implementation order

1. Define and validate default persona/default forum configuration.
2. Add public workspace summary access for personas, characters, forums,
   members, and forum defaults.
3. Add the bootstrap response and Character list route.
4. Resolve Character detail Markdown semantics and add its detail route.
5. Add persisted or authoritative session time metadata and Recent aggregation.
6. Enforce trimmed, non-empty session labels server-side.
7. Decide whether the composer attachment button remains in the design.
