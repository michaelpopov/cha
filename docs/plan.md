# Implementation plan — editing a character's provider and style

Read `docs/design.md` first. It explains what is being built and why each choice
was made; this file is only the sequence.

The work is split into four blocks. Each block is sized to fit one session, ends
with a green build, and leaves the tree in a committable state. Run them in
order — B depends on A, C depends on B, D depends on all three.

Verification commands, from the repository root:

```bash
make test
```

```bash
make web-check
```

---

## Block A — server read path

**Goal:** the browser can read a character's current provider and style, and the
lists of available ones. Nothing is writable yet.

### Steps

1. **Retain the paths `WorkspaceDefinition` currently drops.** In
   `src/workspace/workspace_definition.cpp`, `load()` uses a local
   `definitions_directory` and per-forum directories that are discarded once
   loading finishes. Store both as members (`characters_directory_`, and a
   `forum_directories_` map or a widened use of the existing
   `forum_config_paths_`). Block B needs them too.

2. **Export a style-appearance loader.** `read_style_config()` and
   `load_named_style()` are both in the anonymous namespace of
   `src/agents/character_config.cpp`. Declare one of them in
   `src/agents/character_config.h` so the style list can resolve each entry's
   `CharacterAppearance`. The settings screen renders its sample line from that
   appearance, and it cannot be derived from the style's name — see the design
   note "Why the two lists have different shapes".

3. **Add a name-listing helper.** Enumerate the immediate subdirectories of
   `workspace/system/providers` and `workspace/system/styles` that contain a
   `config.toml`, sorted by label. Put the label derivation (`-`/`_` to spaces,
   capitalise the first letter) beside it. `src/workspace/workspace_definition.cpp`
   is the right home; the directories are already on `WorkspaceConfig`.

   **List only options that resolve.** Load each entry through
   `load_named_provider()` / `load_named_style()` and drop the ones that throw,
   logging a warning. Existence of a `config.toml` is not usability, and startup
   validates only the providers and styles something references — so an
   unreferenced malformed one reaches this list untouched. Dropping it also stops
   one bad file from failing the whole response, which is what parsing styles for
   their appearance would otherwise do.

   Loading a provider config here is safe and cheap: it reads no environment
   variable and opens no socket — `api_key_env` is only a name resolved at
   request time. Keep its *contents* out of the response all the same; a
   `ProviderOption` carries an id and a label and nothing else.

4. **Add the read accessors to `WorkspaceDefinition`** (`workspace_definition.h`):
   - `available_providers()` → a vector of `{id, label}`.
   - `available_styles()` → a vector of `{id, label, appearance}`.
   - `character_settings(id)` → the `provider` and `style` names read from
     `workspace/characters/<id>/character.toml` right now, both optional.
     Read on every call, not cached — see the design note on
     `forum_default_character`.
   - `character_config_path(id)` or an equivalent way to answer "does this
     character have a writable config file", which is false for the built-in
     Assistant.
   - `forums_overriding_provider(id)` → the forums that contain this character
     and name a provider in `members/character_defaults.toml` or
     `members/<id>/character.toml`. Block B reuses it to decide which sessions a
     save may restart, so return the forum ids and let the route map them to
     display names. In this workspace `montaigne`, `margaret` and `stirlitz`
     must each come back with Circle — they are the regression test for the
     whole disclosure.

5. **Extend the protocol.** In `src/web/protocol.h`, add a `ProviderOption`
   (`id`, `label`) and a `StyleOption` (`id`, `label`, `appearance`), and give
   `CharacterDetail` the fields `provider`, `style`, `available_providers`,
   `available_styles`, `provider_overridden_by`, `writable`. Add their `to_json`
   and extend
   `CharacterDetail`'s in `src/web/protocol.cpp`; `CharacterAppearance` already
   has a serialisation to reuse. Optional names serialise as `null`, not as an
   omitted key. Do not collapse the two option types into one — the design note
   explains why they must stay apart.

6. **Populate them in the route.** `src/web/lobby_routes.cpp`, the existing
   `GET /api/v1/characters/([^/]+)` handler.

7. **Update the API spec.** `resources/cha.yaml` — the `CharacterDetail` schema
   and the two new option schemas. This file is the source of truth for the
   browser's generated types, so it must be right before Block C.

8. **Tests.** `tests/application/unit_workspace_definition.cpp` for the
   accessors and label derivation (including a character with neither key set,
   and the built-in Assistant reporting not-writable);
   `tests/web/unit_protocol.cpp` for the JSON shape;
   `tests/web/unit_lobby_routes.cpp` for the route. Assert that a style option's
   appearance matches its `config.toml` — `mono-large` must come back as
   `font: mono, size: large` — and that no provider option carries anything but
   an id and a label. Add a fixture workspace holding an unreferenced malformed
   provider and an unreferenced malformed style, and assert neither appears in
   its list and the response is still `200`.

9. **Module docs.** `src/workspace/README.md`, `src/web/README.md` and
   `src/agents/README.md` for the newly exported style loader.

### Done when

`make test` passes and `GET /api/v1/characters/epictetus` returns
`"provider": "terra"`, `"style": "serif-italic"`, `"writable": true`, and an
`available_styles` whose entries each carry a resolved appearance.

---

## Block B — server write path and session reload

**Goal:** saving works, and a saved change reaches sessions that are open.

### Steps

1. **Generalise the config writer.** `write_forum_config_setting()` in
   `src/workspace/workspace_definition.cpp` already does parse →
   `insert_or_assign` → write to `.new` → rename. Extend it (or add a sibling)
   so a setting can also be **erased**, which is what "not set" means for both
   pickers.

2. **Add `WorkspaceDefinition::write_character_settings(id, provider, style)`.**
   Rejects a character with no config file. **Loads** each non-null selection
   through `load_named_provider()` / `load_named_style()` before writing
   anything, so a name that resolves to a malformed config is rejected rather
   than recorded — checking that the directory exists is not enough, and the
   failure would otherwise surface only at the next session open. A rejected save
   leaves the file untouched. Takes the same write mutex as the forum writes.

3. **Re-resolve definitions when a session opens.** Change
   `copy_definitions_for()` to re-run `load_forum_definitions()` from disk for
   that forum instead of returning the startup copy. On exception, log a warning
   and return the cached copy — a broken hand edit must degrade to stale
   settings, not to an unopenable session.

   Re-run the **whole** loader. It also re-reads `CHARACTER.md`, the member
   layers and the forum context, so more than provider and style goes live at the
   next open; that is accepted and documented in the design. Do not narrow it to
   the two fields — that means re-implementing the four-layer provider precedence
   outside `load_character_config()` and gives resolution order a second
   implementation to drift from.

   **Report the fallback, do not just log it.** Add an optional message to
   `OpenedSession` (`src/session/opened_session.h`) that `open_session()` sets
   when it falls back, and have `LiveSession` publish it as the session notice.
   `SessionSnapshot.notice` already exists and already renders as
   `cha-session-notice`. A session quietly running startup settings while the
   settings screen reports the file is the exact divergence Block A's validation
   exists to prevent, and a hand edit reaches it without the UI.

   Cover both paths in `tests/application/unit_session_open.cpp`, and the notice
   in `tests/web/unit_live_session.cpp`.

4. **Add `ShutdownReason::reloading`** in `src/web/protocol.h`, its string in
   `src/web/protocol.cpp`, and the enum value in `resources/cha.yaml`. **Also
   rank it in `shutdown_reason_priority()`** in `src/web/live_session.cpp` —
   above `browser_disconnected`, below `session_failed`. Skipping this is silent:
   `shutdown_reason_` is initialised to `browser_disconnected` and
   `keep_higher_priority_reason()` only replaces on a strict increase, so an
   unranked `reloading` ties at zero, loses, and never reaches the browser.
   Cover it in `tests/web/unit_live_session.cpp`.

5. **Add the PATCH route** in `src/web/lobby_routes.cpp`, modelled on the
   existing `PATCH /api/v1/forums/…/sessions/…` handler: `is_valid_route_component`,
   `validate_json_mutation`, `parse_route_json_body`, then the write. Map a bad
   name to `400 bad_request` and a missing or non-writable character to `404`.
   Respond with the same body `GET` returns.

6. **Close the affected live sessions**, after the write succeeds and before the
   response: take `live_sessions->snapshot().running_sessions`, keep the
   identities whose `forum_id` names a forum listing this character in
   `member_ids`, and call `request_shutdown(ShutdownReason::reloading)` on each
   handle from `lookup()`. The server does not reopen anything.

   **Restart only what the save can actually change.** Compare the submitted
   values against the file before writing, then:
   - style changed → every forum containing the character;
   - provider changed and style unchanged → only forums *not* in
     `forums_overriding_provider(id)`;
   - neither changed → restart nothing, and skip the write.

   Without this, saving Montaigne's provider kills Circle's sessions — losing
   any answer being generated — to rebuild them with the identical effective
   provider, because Circle overrides it. Test that case by name.

7. **Tests.** `tests/web/unit_lobby_routes.cpp` for the route including
   validation failures and the untouched-file guarantee — among them a PATCH
   naming a provider whose `config.toml` is malformed, which must answer `400`
   and leave `character.toml` byte-identical;
   `tests/application/unit_workspace_definition.cpp` for the write and erase;
   `tests/web/unit_live_session_manager.cpp` or `unit_lobby_routes.cpp` for the
   shutdown fan-out.

8. **End-to-end test of the server half**, in
   `tests/web/process_web_server.cpp` — it already runs a real server process
   against a fixture workspace with an httplib client, so this is the natural
   home. Drive the whole sequence rather than its parts: open a session in a
   forum containing the character, attach the SSE stream, PATCH the character's
   style, and assert that

   - the stream delivers a final snapshot whose `shutdown_reason` is
     `reloading` — this is the assertion that catches an unranked priority,
     which is otherwise silent;
   - the actor then finishes, and a reopen succeeds. Allow bounded retries: an
     open during teardown answers `session_stopping`, so a one-shot assertion
     here is a flaky test, not a passing one;
   - the reopened snapshot carries the newly saved appearance, which is the only
     proof that re-resolution actually reaches a running session.

   Add the negative case beside it: PATCH a provider for a character every one of
   whose forums overrides it, and assert no session was shut down at all.

9. **Module docs.** `src/workspace/README.md`, `src/web/README.md`, and
   `docs/web-ui/api-requirements.md`.

### Done when

`make test` passes; a PATCH rewrites `character.toml` correctly for all four
combinations (set/clear × provider/style); and the end-to-end test above proves
a live session is torn down with `reloading` and comes back running the new
settings.

---

## Block C — web UI

**Goal:** the chevron, the settings screen, and the silent reopen.

### Steps

1. **Regenerate the API types.** `cd webapp && npm run api-types`. Do this
   first; everything below is typed against it.

2. **Client method.** `webapp/src/api/client.ts` — add
   `updateCharacter(characterId, { provider, style })` returning
   `CharacterDetail`, alongside the existing `getCharacter`.

3. **State.** `webapp/src/state/view.ts` — add the `character-settings`
   `MainView`, a `show-character-settings` action returning to
   `character-detail` on back, a `characterSettingsAvailable: boolean` field,
   and a `character-detail-loaded` action carrying `writable` that sets it.
   Add the `Settings` case to `navigationTitle()`. Reset the flag whenever a
   different character is inspected — that reset is what keeps it from going
   stale, so do not drop it.

   The flag is deliberate, and it exists for one reason: the chevron lives in
   the top bar, which `App` renders, while `writable` is fetched by the screen.
   A control rendered inside the screen body from its own detail would need no
   global state — that trade was considered and the top-bar placement was chosen
   over it. Three lines of state is the agreed price; do not "simplify" it by
   moving the control into the screen.

4. **Report writability.** `CharacterDetailScreen` in
   `webapp/src/components/Screens.tsx` already maps the fetched detail to its
   markdown; have that same callback dispatch `character-detail-loaded` with
   `detail.writable`. The chevron therefore appears once the detail has loaded,
   which is also when we first know it should.

5. **The chevron.** `webapp/src/components/App.tsx` — render an
   `.cha-icon-action` button with `ChevronRightIcon` in place of the
   `cha-topbar-balance` spacer when `mainView === 'character-detail'` and
   `characterSettingsAvailable`. It needs an `aria-label` and a `title` of
   "Character settings"; the spacer must stay when the button is absent so the
   title remains centred.

6. **The settings screen.** A new `CharacterSettingsScreen` in `Screens.tsx`:
   fetches the detail on mount, renders the back row, two `<select>`s over
   `available_providers` / `available_styles` each with a leading
   "Workspace default" / "No style" option for `null`, the override note under
   the provider picker built from `provider_overridden_by` (absent when empty,
   and worded to say the picker is currently inert when it names every forum the
   character belongs to), the sample line rendered
   by passing the **selected** style option's `appearance` to `voiceClasses()`
   (and `undefined` for "No style", which is already how that helper spells the
   plain default), a fixed line above Save warning that saving restarts the
   sessions using this character and loses any answer being generated, and
   Cancel / Save. Reuse
   `.cha-new-session`, `.cha-form-control` and the existing button classes;
   the sample line is the only new CSS in `webapp/src/styles/app.css`.
   Report failures in place with `publicErrorMessage`, like the other screens.

7. **Silent reopen — display only.** The reopening itself needs no new code: the
   stream drops, `beginRecovery()` runs, and the ladder's probe reopens and
   reattaches. Two presentation changes in `Screens.tsx` are the whole task:
   `endedMessage()` returns "Applying character settings…" for `reloading`, and
   `showRecoveryActions` excludes it so no manual Retry / Browse / Return
   buttons appear.

   Do **not** add an `openConversation()` call for this reason. It would no-op —
   the function returns early with a bare `show-chat` dispatch while
   `connection.current.key` still matches, which it does when the final snapshot
   arrives — and if it did run it would dispatch `conversation-opened`, forcing
   `mainView` to `chat` and throwing the user off the settings screen. The
   ladder dispatches `session-snapshot` instead, which leaves `mainView` alone.
   See the design section "Close the affected live sessions after a save".

8. **Fix the 404 message these screens now show.** This one step is server-side
   despite living in the UI block, because the string only becomes visible here.
   `set_route_not_found()` in `src/web/route_support.cpp` answers every 404 with
   "That forum or session was not found." That has always been wrong for
   `GET /api/v1/characters/{id}` and `GET /api/v1/personas/{id}`, and the new
   PATCH now shows it in the two cases a user is most likely to hit: the
   built-in Assistant, and a character whose `character.toml` cannot be read.
   The settings screen would report a character problem as a missing forum.

   Give `set_route_not_found()` an optional message parameter defaulting to the
   current text, and pass a specific one from the character and persona
   handlers in `src/web/lobby_routes.cpp`. Leave the forum and session call
   sites alone — the existing wording is correct there, and there are 26 call
   sites in total, so do not rewrite them all. Cover the character 404s in
   `tests/web/unit_lobby_routes.cpp`; `expect_error()` already takes the
   expected message.

9. **Tests.** `webapp/src/components/App.test.tsx` for the chevron's presence
   and absence, a new screen test for the form and its save, and
   `webapp/src/state/view.test.ts` for the reducer additions. Extend
   `webapp/src/test/fixtures.ts` with the new detail fields. Add a
   `LiveChat.test.tsx` case asserting that a snapshot with `reloading` shows the
   applying message and **no** recovery buttons.

10. **End-to-end test of the client half**, in
   `webapp/src/components/App.test.tsx`. `App` already takes `client`,
   `connectSessionEvents` and `retryDelays` props, so the whole sequence runs
   deterministically with near-zero delays and a fake stream. Starting on the
   settings screen with a live conversation, drive: save resolves → the stream
   delivers a final snapshot with `reloading` → the stream then errors →
   `getSessionSnapshot` rejects with `session_not_live` → `openSession` resolves
   → a snapshot carrying the **new** appearance arrives. Assert that the stream
   reattached, that the transcript renders the new voice classes, and that
   `mainView` is still the settings screen throughout.

   That last assertion is the point of the test. It is what fails if anyone
   reintroduces an explicit `openConversation()` call, whose
   `conversation-opened` dispatch would force `mainView` to `chat`.

   Add a second case where the first `openSession` rejects with
   `session_stopping` and the next attempt succeeds, so the ladder's retries stay
   load-bearing and a later "simplification" to a single open fails here rather
   than in someone's browser.

### Done when

`make web-check` passes, including the two end-to-end cases above. The manual
pass in a running CHA (`make run`) — chevron opens the screen, a save persists,
an open chat reattaches on its own — is a sanity check on top of them, not the
evidence that the flow works.

Playwright is deliberately not used for this. The two tests above cover the
assumptions that were wrong, and a browser-driving test of a teardown-and-recover
race would buy little for its cost in flakiness.

---

## Block D — user-facing documentation and final pass

**Goal:** the repository describes the feature the way it describes the rest.

### Steps

1. `README.md` — the provider and style settings are no longer hand-edit-only.
2. `docs/tutorial.md` — a short walkthrough of changing a character's provider
   and style from the browser.
3. `resources/application-guide.md` — this is what the built-in Assistant
   answers from, so it should know about the screen.
4. `docs/web-ui/behavior.md` and `docs/web-ui/flows.md` — the new screen and the
   navigation into it.
5. `README.md` and `src/workspace/README.md` both describe workspace definitions
   as read once at startup. That is no longer true of a forum's character
   definitions: they are re-resolved on every session open, so hand edits to
   `CHARACTER.md`, member configs and the forum context now reach the next
   session without a restart, while discovery data does not. Say so where the
   old claim is made.

6. Re-read `docs/design.md` against what was actually built and correct any
   drift, particularly in "Known limitations".

7. Full verification: `make test`, `make web-check`, and `make itest`.

### Done when

All three verification commands pass and no document still says provider or
style can only be changed by hand.

---

## Notes for whoever picks up a block

- `CLAUDE.md` governs: prefer the smallest change, no machinery for hypothetical
  needs. If a step below seems to call for an abstraction, it probably does not.
- Do not attempt to update a running session's settings in place. `docs/design.md`
  explains why close-and-reopen was chosen; reintroducing an in-place swap
  reintroduces a use-after-free.
- `resources/cha.yaml` is the source of truth for the browser's types. Change it
  in the same block as the C++ that serves it, never in Block C.
- The reload flow has been got wrong before, and always in the seams rather than
  inside a component: a shutdown reason that never reached the wire, a reopen
  call that silently no-opped, a teardown window that refused the open. Blocks B
  and C each end with a test that drives the whole sequence for exactly that
  reason. If one starts failing, suspect the change before the test.
