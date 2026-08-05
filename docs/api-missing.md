# HTTP API gaps for the designed web UI

Status: simplified 2026-08-05 against the design in
[web-ui/](web-ui/README.md).

The existing HTTP API already supports stored-session listing, creation,
opening, live snapshots, SSE updates, input, default-character changes, and
stopping generation. The designed UI additionally needs one shell, built-in
startup state, richer discovery, character detail, Recent, and timestamps.

## Supported scenario

This contract covers one valid workspace and requests produced by the designed
browser UI. The server loads and validates workspace discovery data at startup.
Configuration, storage, I/O, and invariant failures propagate to the existing
exception boundary. Invalid state gets no additional API model or recovery work.

Rough edges are acceptable. When two designs both satisfy the happy path, the
one with less code wins.

## Boundary rules

HTTP routes and mutations address entities by stable ID. Server-produced
summaries carry the display data required to render them and may be reused in
multiple responses. Do not introduce smaller reference entities solely to avoid
repeating an existing summary.

Transcript entries keep their persisted `display_name` and `addressed_to_name`;
those values preserve historical attribution. Session labels also remain display
text.

## Coverage summary

| Designed feature | Current status | Required work |
| --- | --- | --- |
| One browser shell | Missing | Serve the same shell at `/` and session deep links; return session identity from `open` |
| Startup | Missing | Add `GET /api/v1/bootstrap` with built-in initial IDs and every discovery collection |
| Personas | Partial | Add configured short descriptions and Guest |
| Characters | Missing route | Add summaries, Assistant, and `CHARACTER.md` detail |
| Forums | Partial | Add member summaries, default character, and Entrance |
| Sessions | Partial | Add `updated_at` |
| Recent | Missing | Add a cross-forum mtime-ordered aggregate |
| Persona attribution | Complete | No work — see below |
| Live chat | Complete | Reuse the current live runtime and session actions |
| Settings | Undesigned | No API work |

## Existing foundations that remove work

- `WorkspaceSnapshot` already loads and validates personas, characters, and
  forums. The HTTP projection wraps it rather than re-scanning the workspace.
- `Persona`, `CharacterDefinitionMetadata`, and `Forum` already carry every
  discovery field the UI shows, including `Forum::default_agent_id` and member
  IDs.
- **`EffectivePersonas` already builds `Guest` + workspace personas as a
  `SharedPersonaRoster`**, and `SessionController::resolve_author()` already
  matches **by persona ID** — exactly what the browser submits. No persona
  refactor is needed. The only gap is that `Workspace::open_session()` hardcodes
  `load_personas()`, which omits Guest; pass the effective roster in instead.
- `SessionRegistry` already owns live web sessions and takes a session factory,
  so built-in dispatch is a branch, not a redesign.
- `Guest`, Assistant, Entrance, and Welcome already exist in the application
  layer.

HTTP handlers reuse these values and operations. They must not execute terminal
commands or parse `ApplicationResult` presentation strings.

## Required API work

### 1. Serve one shell

The server currently presents a lobby at `/` and a different chat document at
`/s/{forum}/{session}/`, and successful `open` returns a navigation path.

- `/` and `/s/{forum}/{session}/` serve the same document;
- successful `open` returns `{forum_id, session_id}`;
- the standalone session-not-open document is removed.

The shell itself stays the existing placeholder. This is a route-contract change,
not a UI.

### 2. Add bootstrap, and delete the routes it replaces

Construct one HTTP discovery projection at server startup, wrapping
`WorkspaceSnapshot` and adding Guest to the persona roster, Assistant to the
character roster, and Entrance to the forum roster with Assistant as its member
and default character. Lookup is by linear scan over a handful of entities; no
index is warranted.

Add `GET /api/v1/bootstrap`:

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

The browser starts in Welcome using those IDs. There are no configurable web
defaults in `app.toml`.

**Delete `GET /api/v1/personas`, `GET /api/v1/forums`, and do not add
`GET /api/v1/characters`.** Bootstrap returns all three rosters and the browser
navigates from client-held state, so separate list routes would duplicate the
same summary work and its tests.

The surviving lobby surface is bootstrap, `GET /api/v1/characters/{id}`,
`GET /api/v1/forums/{forum}/sessions`, create, and open.

### 3. Persona, character, and forum summaries

- `PersonaSummary` gains the configured optional `Persona::description`. Guest
  has none; an absent description is fine. `PERSONA.md` is never reused as a
  description.
- One reusable `CharacterSummary` carries `id`, `display_name`, and optional
  `description` from `character.toml`.
- `ForumSummary` gains `default_character_id` and `members`, reusing
  `CharacterSummary` and sorted by display name. Forums have no description and
  member names are not links.

### 4. Character detail

Add `GET /api/v1/characters/{character_id}`, returning the summary fields plus
`character_markdown`.

For a workspace character this is the verbatim contents of
`characters/<id>/CHARACTER.md`. It does not expand includes, load a forum
override, or initialize a provider. Definitions that are include-only will render
their directives as text; that is an accepted rough edge. Assistant returns the
embedded application guide because it has no workspace file.

Do not add `DISPLAY.md`, another browsing file, or another character entity.

### 5. Expose Entrance and Welcome through the web runtime

The browser's initial session uses application built-ins the web composition root
does not construct.

- Own one `WelcomeStorage` per server process, shared by all browsers.
- Extract the existing Entrance session construction after ID resolution into a
  shared primitive and call it with IDs from the web.
- Teach the `SessionRegistry` factory to dispatch Entrance and Welcome IDs.
- Include Entrance in the bootstrap forum roster.
- **Entrance's session list is exactly `[Welcome]`.** The web does not read the
  stored Entrance catalog under `var/system/entrance/sessions`, and creating a
  session in Entrance is not supported — that request already fails through the
  existing not-found path, which is an acceptable rough edge.

### 6. Timestamps and Recent

Add `updated_at` to `SessionListing`, taken from the session database's
`last_write_time` and serialized as **epoch seconds**. An integer avoids the
`file_clock` conversion and formatting that ISO-8601 would require. Session rows
stay limited to ID, label, live state, and time.

Populate bootstrap's `recent_sessions` by merging the per-forum session listings
and sorting newest first. There is no cap, no pagination, and no pinning —
Welcome's database is created at process start, so it already sorts first until
another session is used. A Recent entry is:

```json
{
  "forum_id": "stoics",
  "session_id": "2026-08-02-10-30-00-session",
  "session_label": "Design review",
  "updated_at": 1754153000
}
```

There is no `live` field: nothing in the design renders live state, and it is the
only reason Recent would need to consult `SessionRegistry`. The browser resolves
the forum display name through the bootstrap forum roster.

The browser trims the required New session name and submits a non-empty label.
The server keeps its existing ID-based create/open workflow and adds no
validation.

## Chat status derivation

No new status endpoint is required.

| Field | Source |
| --- | --- |
| Forum | Active session's `ForumSummary` |
| From | Current browser persona selection |
| To | `SessionSnapshot.default_character_id`, resolved through its character summaries |

An accepted default-character mutation is reflected by the next snapshot or SSE
event.

## Existing capabilities to retain

- One owner thread per live session and owning commands across the HTTP seam.
- One SSE connection per live session.
- Reattachment through `SessionRegistry`.
- Server-owned transcript attribution.
- No session descriptions or transcript excerpts in listings.
- No create, edit, or delete endpoints for personas, characters, or forums.

## Client-owned state

No server mutation is needed for sidebar visibility, Navigation state, selected
persona, selected forum, inspected character, or highlighted Recent row. A page
load starts from the bootstrap initial IDs.

## Out of scope

Only the success-path work above is part of the contract. Invalid state gets no
API model or recovery work. Settings, attachments, and additional character
display entities are not part of the designed UI.

The executable development plan is [api_plan.md](api_plan.md).
