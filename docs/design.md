# Editing a character's provider and style from the web UI

## What this is

A character's model provider and visual style live in
`workspace/characters/<id>/character.toml`. Both can be read and changed from
the browser, on a screen reached from the character's description page, and a
saved change takes effect in sessions that are already open.

## How provider and style are configured today

Both settings are *named references*, not inline settings. A character config
selects one of them by name:

```toml
display_name = "Epictetus"
description = "A Stoic teacher focused on practical judgment."

provider = "terra"
style = "serif-italic"
```

The names resolve against two workspace directories:

- `workspace/system/providers/<name>/config.toml` — a whole model backend
  (host, port, model, temperature, API key env, …).
- `workspace/system/styles/<name>/config.toml` — a `CharacterAppearance`
  (`font`, `style`, `weight`, `size`).

This is what makes the feature small: the editor is two pickers over short lists
of directory names, not a panel of individual knobs.

### Provider layering

Provider resolution has four layers, and the *highest* layer naming a provider
wins outright — configs are never merged field by field
(`src/agents/character_config.cpp`, `load_character_config`):

1. `workspace.toml` `[provider]` — the application default.
2. `workspace/characters/<id>/character.toml` — the character definition.
3. `workspace/forums/<f>/members/character_defaults.toml` — forum defaults.
4. `workspace/forums/<f>/members/<id>/character.toml` — a member override.

The Characters screen is workspace-wide, so it edits **layer 2**. A forum that
sets its own provider therefore shadows the value this screen shows. That is
live in this workspace today: `workspace/forums/circle/members/character_defaults.toml`
sets `provider = "sol-high"`, so changing Montaigne's provider here changes
nothing inside the Circle forum.

In this workspace that is not a corner case. Margaret, Montaigne and Stirlitz
each belong to exactly one forum — Circle — and Circle overrides the provider.
For three of the eight configured characters the character-layer provider is
therefore never read by anything.

The screen still edits the character layer: moving provider selection into
per-forum settings would split one feature across two screens and is a much
larger change. But it may not present a control that silently does nothing, so:

- **The screen names the forums that override.** `CharacterDetail` reports them
  and the form shows them under the provider picker.
- **A save does not restart sessions it cannot affect.** See "Close the affected
  live sessions after a save".

The picker stays enabled even when every forum overrides. The write is not
meaningless — it sets the character's own default, which applies to any forum
that doesn't override and to any forum it is added to later — it is only
currently inert, and the note says so.

### Style has no layering

`resolve_style()` reads the style name from the definition layer only. A
character that names no style gets the plain default appearance rather than
inheriting anything. So style is never shadowed and needs no caveat.

### Absent values

Both keys are optional and absence is meaningful:

- No `provider` — the workspace default applies (unless a forum layer sets one).
- No `style` — the plain default appearance (sans, normal, normal, normal).

So each picker needs an explicit entry for "not set", and choosing it **erases
the key** from the file rather than writing a value.

## The UI

### Entry point

The character description page gains a forward chevron at the top-right of the
top bar. It uses the same `›` affordance every roster row uses, so it reads as
"there is another screen this way" rather than introducing a second navigation
idiom.

It occupies the `cha-topbar-balance` slot, which is an empty `2.65rem` spacer
today — exactly the width of `.cha-icon-action` — so the centred title needs no
layout change.

The chevron is **absent** for the built-in Assistant, which is synthesized in
code and has no `character.toml` to write. This follows the existing precedent
of the Sessions screen, which simply omits the "New session" row in the
Entrance forum rather than showing a disabled one.

The current provider and style are not shown on the description page. That
matches the Forums screens, where a forum's settings are likewise only visible
once you push into them.

### Settings screen

One screen, one form, one write — the same shape as the existing New session
form:

```
‹ Montaigne

Provider   [ Terra          ▾ ]
           Circle of Life uses its own provider for this character,
           and it is the only forum Montaigne belongs to.

Style      [ Serif italic   ▾ ]

The chief task in life is this…        ← sample line in the chosen style

                    [ Cancel ]  [ Save ]
```

- Top bar title: `Settings`. Back row: `‹ <character display name>`.
- The provider picker's first entry is `Workspace default`; the style picker's
  first entry is `No style`. Both mean "erase the key".
- Below the provider picker, the forums that override it are named, so the
  picker never looks effective when it is not. The line is absent when no forum
  overrides, and says so plainly when every forum the character belongs to does.
  Style is never shadowed, so it carries no such line.
- The sample line is a fixed sentence rendered with the chosen style's classes,
  so a style choice can be seen before it is saved. Style is purely visual, so
  picking one blind is the one thing worth avoiding. This is why each style
  option carries its resolved appearance — see "Why the two lists have different
  shapes".
- Save is disabled while nothing has changed and while a save is in flight.
  Failures are reported in place, like every other screen in this app.
- A static line above Save says that saving restarts the sessions using this
  character and that an answer being generated is lost. Saving is the one action
  in this app that can destroy work in a conversation the reader is not looking
  at, so it says so before the click rather than after. It is fixed text, not a
  count of affected sessions: the count needs a live query, would be stale by the
  time it was read, and does not change the decision.

### Labels

Provider and style config files have no `display_name` field, and both readers
reject unknown fields, so adding one would mean widening two allowlists. Instead
the label is derived from the directory name: `-` and `_` become spaces and the
first letter is capitalised.

| directory        | label            |
| ---------------- | ---------------- |
| `terra`          | Terra            |
| `sol-high`       | Sol high         |
| `mistral-large`  | Mistral large    |
| `serif-italic`   | Serif italic     |
| `mono-large`     | Mono large       |

Derivation never fails, so there is no raw-id fallback path. It produces
sentence case, so a future provider named `gpt-4o` would read "Gpt 4o".

## The API

`GET /api/v1/characters/{id}` gains four fields alongside the existing summary
and `character_markdown`:

```json
{
  "id": "epictetus",
  "display_name": "Epictetus",
  "provider": "terra",
  "style": "serif-italic",
  "available_providers": [
    { "id": "mistral-large", "label": "Mistral large" },
    { "id": "qwen",          "label": "Qwen" },
    { "id": "sol-high",      "label": "Sol high" },
    { "id": "terra",         "label": "Terra" }
  ],
  "available_styles": [
    { "id": "mono", "label": "Mono",
      "appearance": { "font": "mono", "style": "normal",
                      "weight": "normal", "size": "normal" } },
    { "id": "serif-italic", "label": "Serif italic",
      "appearance": { "font": "serif", "style": "italic",
                      "weight": "normal", "size": "normal" } }
  ],
  "provider_overridden_by": ["Circle of Life"],
  "writable": true,
  "character_markdown": "…"
}
```

- `provider` and `style` are `null` when the key is absent from the file.
- `provider_overridden_by` lists the display names of the forums that contain
  this character and set their own provider in `members/character_defaults.toml`
  or in `members/<id>/character.toml`. Empty when none do. The browser renders it
  under the provider picker and the server uses the same fact to decide which
  sessions a save may restart.
- `available_*` are sorted by label and are the same for every character; they
  ride along on this response so the settings screen needs exactly one request.
- Both lists contain only options that **resolve**. Each entry's `config.toml`
  is parsed through the same loader the session runtime uses, and one that fails
  is logged and dropped rather than offered. A directory containing an
  unparseable `config.toml` is not a usable option, and startup does not catch
  it: it only validates providers and styles some character actually references,
  so an unreferenced broken one survives to be offered here. Dropping it also
  keeps one bad file from failing the whole response.
- `writable` is false for the built-in Assistant. The browser uses it to decide
  whether to show the chevron.

### Why the two lists have different shapes

A style option carries its **resolved appearance** — the same four fields
`CharacterAppearance` already publishes. The settings screen has to render its
sample line in a style the user has selected but not yet saved, and nothing else
in the response can answer that: `summary.appearance` describes the style
currently saved, not the one being considered. The appearance cannot be derived
from the name either. Today's names happen to be descriptive, but a style is an
arbitrary directory name pointing at a config, so a style called `dramatic` is
perfectly legal.

A provider option deliberately carries **nothing but its name**. A provider
config is a whole model backend, including `host`, `port`, `model` and
`api_key_env`. `CharacterMetadata` is documented as public, discovery-safe data
containing "no endpoint, credential, model, or other model-provider detail", and
this response must keep that promise. There is nothing to preview about a
provider, so there is no reason to send any of it.

This is why there is no single generic option type covering both. Keep them
separate; a shared type that carries "the config" invites exactly the leak the
protocol has so far avoided.

`PATCH /api/v1/characters/{id}` takes both values and rewrites the file:

```json
{ "provider": "qwen", "style": null }
```

`null` erases the key. A character with no config file answers `404`.

The server **loads** each non-null selection before touching `character.toml` —
`load_named_provider()` / `load_named_style()`, the same calls the session
runtime makes — and answers `400 bad_request` if either fails. Checking that the
directory merely exists is not enough: a name that resolves to a malformed
config would be written happily, and the failure would only appear later, when
the session re-resolves it. Loading it here means a config that cannot run is
never recorded as the character's setting, and the file is left untouched.

The response is the same body as `GET`, so the screen re-renders from the
server's own reading of the file it just wrote.

Both the current values and the available lists are read from disk on every
request, rather than cached. This follows the precedent already set by
`forum_default_character`, which is re-read from `config.toml` on every session
open so a saved change reaches the next session without a restart.

## Making a change take effect

`WorkspaceDefinition` loads every character definition once at startup and
`open_session()` copies those cached definitions, so neither a restart-free edit
nor a session reopen would pick up a new value on its own. Two changes fix that.

### 1. Re-resolve definitions when a session opens

`copy_definitions_for()` re-resolves the forum's character definitions from
disk instead of returning the startup copy. If re-resolution throws — a
hand-edited file that no longer parses — it falls back to the startup copy, so a
broken edit degrades to stale settings rather than an unopenable session. This
mirrors how `forum_default_character` already handles an unusable config file.

**The fallback must not be silent.** A session running the startup settings
while the screen reports what is on disk is the same divergence the PATCH
validation above exists to prevent, and it is reachable without the UI at all:
edit a provider config by hand after a good save, then reopen. So the fallback
logs *and* reports. `OpenedSession` carries an optional message, which
`LiveSession` publishes as the session notice — the field already exists on
`SessionSnapshot` and already renders in the chat as `cha-session-notice` — so
the conversation says plainly that its character settings could not be reloaded
and that it is running the ones from startup.

This requires `WorkspaceDefinition` to retain two paths it currently drops after
loading: the characters directory, and each forum's own directory.

#### This makes more than provider and style live

`load_character_definitions()` does not reload two fields. It re-reads the
definition `character.toml`, `members/character_defaults.toml`, the member
`character.toml`, the character's `CHARACTER.md` (or the member's override of
it), and the forum context that goes into the system prompt. Re-running it means
**all** of that takes effect at the next session open, not only the two settings
this feature edits.

That is accepted deliberately. The alternative — re-resolving only `provider`
and `style` — means re-implementing the four-layer provider precedence and the
definition-only style rule outside `load_character_config()`, which would create
a second source of truth for resolution order. A settings screen that computes
precedence differently from the runtime is the exact class of defect this design
has already had to correct twice. One loader, one answer.

The cost is that a hand edit to an unrelated file becomes live at the next open
while discovery data does not, because the roster, the character-detail Markdown
and the forum Markdown are all still the copies read at startup. Making those
live too would mean invalidating the whole workspace model, including state that
HTTP threads read without a lock, which is far beyond this feature. See "Known
limitations".

### 2. Close the affected live sessions after a save

After the file is written, the PATCH handler asks the affected live sessions to
shut down, with a new shutdown reason meaning "reopening". The browser reopens,
and the reopened session resolves the new settings through change 1.

"Live" here has to include sessions that are still *opening*, not only those
already running. A session reads its definitions on the way up, so one that is
mid-open when the save commits is already holding the old values, and would
otherwise arrive running settings the file no longer has, with nothing left to
correct it. Because the write commits before the fan-out, a session that has not
appeared by then has not read the file yet and will read the new values.

Which sessions are affected depends on what actually changed, because a restart
costs the reader any answer being generated and must never be spent for nothing:

| changed          | sessions restarted                                          |
| ---------------- | ----------------------------------------------------------- |
| style (any case) | every forum containing the character — style never layers    |
| provider only    | only forums that do **not** override the provider            |
| nothing          | none                                                         |

"Nothing changed" is reachable: Save is disabled when the form is untouched, but
a stale browser can still PATCH the values already on disk. Comparing against
the file before writing costs nothing and makes the no-op harmless.

Whether a forum overrides is the same layer-3 / layer-4 question the settings
screen displays: does `members/character_defaults.toml` or
`members/<id>/character.toml` name a provider. Two small TOML reads per forum,
using the forum directories retained for change 1.

The server does **not** reopen anything itself. Reopening is the browser's job,
which avoids waiting for the old actor to finish and the `busy` window that
would follow. A session nobody is watching simply stays closed and picks the
change up whenever it is next opened — the same outcome.

Reopening is done by the **existing stream recovery ladder**, with no new client
logic. When the actor tears down, the SSE stream drops, and a stream error is
already the one thing that starts `beginRecovery()`. Each rung probes with a
snapshot request, sees `session_not_live`, calls `openSession()`, and reattaches
— `sessionProbe()` calls that outcome `recovered`, and it is exactly this case.

That the ladder retries is what makes it the right tool rather than a
convenience. The old actor stays `stopping` until its final drain and teardown
finish, and `LiveSessionManager::open()` rejects an open during that window with
`session_stopping`. A single explicit open would hit that 409 and give up:
`openWithCapacityRetry()` retries only `session_limit_reached`. The ladder's
rungs cover the window with backoff and get there on a later attempt.

The ladder also preserves the screen you are on. It reports each probe with a
`session-snapshot` dispatch, which updates the snapshot and nothing else. The
explicit open path would instead dispatch `conversation-opened`, which sets
`mainView` to `chat` and would drag you out of the settings screen you are
standing on. Combined with the fact that nothing in `App.tsx` keys a stream
cleanup on `mainView`, this is what makes the reload genuinely a background
event: the stream is still attached while you are on the settings screen, the
ladder runs there, and the chat is reattached by the time you go back to it.

So `App` must **not** call `openConversation()` for a reloaded session. Beyond
being unnecessary, it would not work: `openConversation()` returns early with a
bare `show-chat` dispatch when `connection.current.key` already matches the
target, and at the moment the final snapshot arrives the connection is still
open.

`ShutdownReason` gains `reloading`, but only as a **display** reason. Its whole
job is to stop the browser from misreporting what happened: for every other
reason the chat shows an ended-session notice with manual Retry / Browse /
Return buttons, and without a reason of its own a reload would surface as
"This session was released because the browser disconnected". With it, the chat
reads "Applying character settings…" and shows no buttons, because recovery is
already under way on its own.

That reason must also be ranked in `shutdown_reason_priority()` in
`src/web/live_session.cpp`. `LiveSession::shutdown_reason_` is initialised to
`browser_disconnected`, and `keep_higher_priority_reason()` replaces the stored
reason only on a strict priority increase — so an unranked `reloading` falls
through to priority zero, ties, loses, and never reaches the browser at all.
`-Wall` warns about the unhandled enumerator, but the build does not use
`-Werror`, so the warning is easy to walk past.

### What this costs

A session that is generating an answer when you press Save loses that partial
answer: a shutdown cancels the in-flight generation. This is accepted
deliberately. CHA is a personal application, and the alternative — deferring the
reload, or swapping a running character's model backend in place — is more
machinery than the feature is worth. The settings screen says so above Save, so
the loss is chosen rather than discovered.

## What we are deliberately not doing

- **Not swapping settings inside a running session.** `ForumCharacters` and
  `GenerationExecutor` are both immutable after construction by design, and
  `stage_batch` hands raw `ModelBackend*` pointers to the worker pool, so an
  in-place swap has a real use-after-free ordering problem. Close-and-reopen
  avoids all of it and reuses paths that already work.
- **Not editing per-forum provider overrides.** See "Provider layering".
- **Not adding `display_name` to provider or style configs.** See "Labels".
- **Not editing any other character setting.** `display_name`, `description`,
  tags and prompt variables stay hand-edited.

## Known limitations

- A forum-level provider override still shadows what this screen saves; the
  screen now names the forums responsible and skips restarting their sessions,
  but it cannot change them. Editing a forum's own provider override remains a
  hand edit. See "Provider layering".
- Saving reserialises `character.toml` through toml++, so comments in that file
  are lost. This is already true of forum `config.toml`, which the existing
  default-character and default-persona writes go through.
- **Sessions and discovery can disagree after a hand edit.** Because a session
  open now re-resolves the forum's definitions, every file that goes into them
  becomes live at the next open: the definition and member `character.toml`,
  `character_defaults.toml`, `CHARACTER.md`, and the forum context. Discovery
  does not follow. So after editing `CHARACTER.md` by hand, a session opened
  afterwards runs the new prompt while the Characters screen still shows the old
  Markdown; after editing `display_name`, a session answers to the new name —
  `@handles` and speaker labels included — while the roster shows the old one.
  Restarting CHA reconciles the two. This is a widening of what a restart-free
  edit means, accepted so that resolution has exactly one implementation; see
  "This makes more than provider and style live".
- `CharacterSummary.appearance` keeps the appearance resolved at startup, both
  in `/api/v1/bootstrap` and in the summary embedded in `CharacterDetail`. So
  after a save it is stale until restart. Nothing renders from it: the transcript
  takes each voice from the session snapshot, and the settings screen renders its
  sample from the selected entry in `available_styles`. It is left alone rather
  than made live because making it live means invalidating the startup roster,
  which is the cache the whole workspace model is built around.
- Two browser windows saving the same character at once is last-write-wins.
