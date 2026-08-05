# HTTP API implementation plan

Status: simplified 2026-08-05 from [api-missing.md](api-missing.md) and the
design in [web-ui/](web-ui/README.md).

This plan implements only the successful browser workflow. It assumes the
workspace is valid at startup, session storage is readable, and requests come
from the designed UI. Errors propagate to the existing exception boundary. No
partial results, no recovery DTOs, no validation the browser can do itself.

Rough edges are acceptable. When two designs both satisfy the happy path, the one
with less code wins.

The browser application is outside this plan. The result is the API it needs.

## Two blocks

Block 1 makes the server able to serve built-in startup state. Block 2 is the
HTTP surface. Both are one Claude Code session each.

They split this way because Block 1 touches `session/` and `application/` — code
the terminal shares — while Block 2 is confined to `src/ui/web/`. Merging them
would put terminal-regression risk and route work in one session; splitting Block
2 further would make two sessions re-read the same `protocol.h` / `protocol.cpp`
/ `lobby_routes.cpp` cluster.

## Fixed decisions

| Decision | Choice |
| --- | --- |
| Shell | One client-routed shell. `/` and `/s/{forum}/{session}/` serve the same document. |
| Initial chat | The shared built-in Welcome session in Entrance, Guest selected, Assistant as target. |
| Built-ins | Guest, Assistant, and Entrance appear in discovery. Welcome is Entrance's only session. |
| Persona attribution | Already works. Pass the effective roster into opened sessions; change nothing else. |
| Workspace lifetime | Loaded and validated at startup. Disk changes need a restart. |
| Failure policy | Strict. Exceptions propagate. |
| Character detail | Verbatim `CHARACTER.md`. Include-only definitions render their directives; accepted. |
| Discovery lookup | Linear scan over a handful of entities. No index. |
| Timestamps | Session database mtime as epoch seconds. |
| Recent | All sessions, newest first. No cap, no pinning, no live state. |
| Route surface | Bootstrap replaces the persona, forum, and character list routes. |

---

## Block 1 — Built-in startup path

**Goal.** A running `cha_web` can open the Welcome session in Entrance and accept
a message authored by Guest.

### Background

- `builtin_entrance()` is a synthetic `Forum` with an empty `directory`
  ([builtins.cpp:14](../src/application/builtins.cpp:14)), so
  `Workspace::open_session()` cannot reach it — it resolves
  `<root>/forums/builtin-entrance` and throws.
- `WelcomeStorage` creates a per-process temp database and deletes it in its
  destructor ([welcome_storage.cpp](../src/application/welcome_storage.cpp)).
- `SessionRegistry` already takes a `RegistrySessionFactory`, and
  `from_workspace()` supplies a one-line lambda
  ([session_registry.cpp:92](../src/ui/web/session_registry.cpp:92)).
- `EffectivePersonas` already builds Guest + workspace personas as a
  `SharedPersonaRoster` ([effective_personas.cpp:9](../src/application/effective_personas.cpp:9)),
  and `SessionController::resolve_author()` already matches by persona ID
  ([session_controller.cpp:391](../src/session/session_controller.cpp:391)).

### Work

1. **Make Guest available in web sessions.** `Workspace::open_session()`
   hardcodes `PersonaRoster personas = load_personas()`
   ([workspace.cpp:651](../src/session/workspace.cpp:651)), which omits Guest.
   Add an optional `SharedPersonaRoster` parameter; when supplied, use it
   instead. The web passes `EffectivePersonas(snapshot).roster()`. Nothing in
   `SessionController` changes.
2. **Add the HTTP discovery projection.** A small type holding a
   `WorkspaceSnapshot` plus Guest, Assistant metadata, and `builtin_entrance()`
   with Assistant as its member and default character. Lookup by linear scan.
   Construct it once in `web_main.cpp`. Do not change `WorkspaceSnapshot` or
   terminal discovery policy.
3. **Own one `WelcomeStorage`** in the web composition root, in the same inner
   scope as the registry and workspace so its temp directory is removed before
   logging shuts down.
4. **Extract the Entrance open primitive.** Everything after session-ID
   resolution is identical on both surfaces — `open_database_path()`,
   `SessionLease::acquire`, `builtin_assistant_definitions()`,
   `SessionController::from_shared_definitions()`, descriptor construction. Put
   that in `application/`, add the Welcome variant that substitutes
   `WelcomeStorage::prepare()`, and rewrite `StoredEntranceSessionSource::open()`
   as name-resolution plus a call to it. The terminal ends up with less code.
5. **Dispatch in the registry factory.** Branch on
   `identity.forum_id == entrance_id` in `SessionRegistry::from_workspace()`;
   everything else falls through to `Workspace::open_session()`. Keep the factory
   signature so the `PortBackedSession` test seam still works.

### Tests

- Open Welcome through the registry; the snapshot names Entrance, Welcome, and
  Assistant.
- Submit input as `builtin-guest` and observe the attributed transcript entry.
- Guest also authors in an ordinary workspace forum.
- Existing terminal suites stay green.

### Done when

`cha_web` completes the initial Welcome chat flow end to end.

---

## Block 2 — HTTP surface

**Goal.** Every screen in the design has the data it needs, from one shell.

### Work

1. **Summaries.** Add optional `description` to `PersonaSummary`. Define one
   reusable `CharacterSummary` (`id`, `display_name`, optional `description`).
   Add `default_character_id` and `members` to `ForumSummary`, reusing
   `CharacterSummary` sorted by display name. Reuse these same types in live
   snapshots; do not add ID-only variants.
2. **`GET /api/v1/bootstrap`** returning the three initial built-in IDs plus
   `personas`, `characters`, `forums`, and `recent_sessions`.
3. **Delete `GET /api/v1/personas` and `GET /api/v1/forums`**, and do not add a
   characters list route. Bootstrap replaces all three. Delete their tests.
4. **`GET /api/v1/characters/{character_id}`** returning the summary fields plus
   `character_markdown`: verbatim `characters/<id>/CHARACTER.md` for a workspace
   character, the embedded application guide for Assistant. No include expansion,
   no forum override, no provider initialization.
5. **Entrance in the lobby routes.** Entrance appears in the bootstrap forum
   roster, and `GET /api/v1/forums/builtin-entrance/sessions` returns exactly
   `[Welcome]`. Do not read the stored Entrance catalog. Creating a session in
   Entrance is unsupported and already fails through the existing not-found path.
6. **`updated_at`** on `SessionListing`, from the session database
   `last_write_time` as epoch seconds.
7. **Recent.** Merge the per-forum session listings, sort newest first, and put
   the result in bootstrap's `recent_sessions`. Each entry is `forum_id`,
   `session_id`, `session_label`, `updated_at`. No cap, no pinning, no `live`
   field, no `SessionRegistry` lookup.
8. **`open` returns `{forum_id, session_id}`** instead of a navigation path.
9. **One shell.** Serve the same document at `/` and `/s/{forum}/{session}/`.
   Delete the separate lobby markup and the session-not-open document, and the
   `page` branch in `resolve()`
   ([session_routes.cpp:60](../src/ui/web/session_routes.cpp:60)). The shell stays
   the existing placeholder.

### Tests

- Bootstrap contains the three initial IDs and all four collections.
- A forum summary carries members and `default_character_id`; Entrance carries
  Assistant.
- Character detail returns file contents for a workspace character and the guide
  for Assistant.
- Entrance's session list is `[Welcome]`.
- A session listing carries `updated_at`; Recent spans forums newest first.
- Successful open returns the two IDs.
- `/` and a session deep link serve the same document.

### Done when

Startup, browsing, session creation, session opening, Recent, and chat all have
complete API support.

### If this session runs long

Stop after step 5. Steps 1–5 are the discovery surface and are independently
useful; steps 6–9 are sessions, Recent, and the shell.

---

## Out of scope

Invalid state gets no API model or recovery work. Settings, attachments, server
side session-name validation, and mutation APIs for personas, characters, and
forums are all outside this plan.
