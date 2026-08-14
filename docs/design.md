# A session-scoped style override (`/style`)

## What this is

`/provider` gave a session a runtime-only way to swap its current character's
model backend. This change adds the presentation counterpart: a chat command
that swaps the current default character's **appearance** — font, slant,
weight, size — **for the running session only**.

```text
/style sans-bold      "Montaigne now uses style 'sans-bold' for this session."
/style                "Montaigne's style override for this session is 'sans-bold'."
/style default        "Montaigne is back to its configured style for this session."
```

The requirements, restated:

1. **Runtime only.** Nothing is written to `character.toml`, forum configs, or
   the session database. The override lives and dies with the live session.
2. **Session-scoped and character-scoped.** The command targets whoever the
   session's current default character is at command time. Other characters,
   other sessions, and later runs of this session are unaffected.

The feature is deliberately much smaller than `/provider`: appearance is
presentation data, so there is no backend to rebuild, no worker thread to
avoid, and no browser change to make. The one way it is *larger* is that the
result is visible, so the update must carry a snapshot where `/provider`
needed only a notice.

## What exists today

- **Style configs** live in `system/styles/<id>/config.toml` and load through
  `load_named_style()` (`agents/character_config.h:154`) into a plain
  `CharacterAppearance` value (`chat/character.h:19`) — the same parse the
  Settings screen validates with. An appearance is inert data: it cannot fail
  to "construct" the way a backend can.
- **Appearance reaches the browser through the roster and the snapshot.**
  `SessionController::view()` borrows `characters_.all()` directly
  (`session_controller.cpp:282`); `to_snapshot()` copies each character's
  `appearance` (`session_projection.cpp:47`); the browser's voices map applies
  it to every message of that character at render time. Nothing in `session/`
  reads appearance, and no transcript entry or journal row carries it.
- **The browser already re-renders from the snapshot.** A `SnapshotRequired`
  delivers the entire visual change — past and future messages alike — with
  no webapp, protocol, or OpenAPI change. The client side of this feature
  already exists.
- **`/provider` built the command chassis.** `CommandKind` +
  argument-carrying `exact` descriptor + early dispatch branch
  (`text_input.cpp`), a typed controller method handling its own report /
  `default` / set forms, a workspace resolver injected through the
  constructor's trailing defaulted slot, and the sibling commands' "for this
  session" feedback convention are all established patterns this command
  reuses.
- **The executor retains open-time metadata.** `runtime_info_` is built from
  the retained definitions, and `/provider`'s `replace_backend()` rebuilds it
  from the same recipes — so it always holds the *configured* appearance.
  That makes it the reset source: the controller keeps no second copy, the
  same decision `/provider` made for backend configs.
- **The generating gate** (`text_input.cpp:43`) rejects every command except
  `/stop` while a generation runs, so the command's behavior mid-generation
  is already decided without any new rule.

## Decisions

### Scope: a per-character override map, session lifetime

The controller holds `std::unordered_map<CharacterId, std::string>
style_overrides_` (character ID → style name, kept for the report form). The
command applies to the current default character and is keyed to that
character, so switching default with `/@Name` and back keeps each character's
override intact.

The map is never cleared during the session: `/clear` advances the history
epoch and is orthogonal to presentation. The override ends exactly when the
live session does: `/exit`, reopen, a character-settings `reloading` restart
(which reverts to file truth, consistently), or process shutdown.

The override applies to **all** of the character's messages, including ones
already on screen. That is what a style change means; scoping it to future
messages would be a rule to explain and a harder render path.

### Resolution: complete, validated at command time

`WorkspaceDefinition` gains:

```cpp
[[nodiscard]] CharacterAppearance resolve_session_style(std::string_view name) const;
```

It validates the name (`require_path_component()`) and loads it with
`load_named_style()` — the same parse startup and the Settings screen use, so
command-time and startup-time semantics cannot drift. On failure it throws
`std::invalid_argument` whose message names the problem and lists the
available style IDs. Unlike a provider, a resolved style has no construction
phase: a config that parses is the whole value, so the swap itself cannot
fail.

### Presentation: one roster mutator, snapshot-carried

`ForumCharacters` gains:

```cpp
bool set_appearance(std::string_view id, const CharacterAppearance& appearance);
```

It mutates the stored `CharacterMetadata` in place and returns false for an
unknown ID. Construction invariants are untouched: the mutator cannot reach
the ID or display name, so uniqueness and syntax guarantees hold by
inspection. Because `view()` borrows the roster directly, the next snapshot
carries the new appearance with **no projection change** — the borrowed-view
model and its "nothing here allocates" invariant stay intact.

The rejected alternative — keeping the roster immutable and merging overrides
at view/projection time — would need a mutable materialized cache rebuilt on
every mutation, which is more machinery than the mutator it avoids.

**Reset comes from the executor's runtime info.** `/style default` restores
the open-time appearance found in `generation_executor_.runtime_info()` for
that character. The recipes live in the executor, so the configured truth
does too; the controller stores no appearance copies. `default` is a reserved
word, intercepted before the resolver runs; a style config actually named
`default` is unreachable through the command (see limitations).

### Controller: one typed method behind an injected resolver

```cpp
using StyleResolver = std::function<CharacterAppearance(std::string_view)>;

[[nodiscard]] ControllerUpdate set_session_style(std::string_view name);
```

The resolver is a constructor argument on the production constructor,
`from_shared_definitions()`, and the definitions-based test constructor,
defaulted to empty and appended after the `provider_resolver` slot added by
`/provider`. An absent resolver yields a fixed notice ("Style override is not
available in this session.") and keeps every existing call site compiling.

The method mirrors `set_session_provider()`'s forms:

- **no busy guard.** `/provider` rejects while generating because swapping a
  backend under a live batch is a real hazard; appearance touches no
  generation machinery, so the typed action is safe at any time. The web
  grammar's generating gate still rejects the command mid-generation, so the
  user-visible behavior matches `/provider` without a rule here.
- **empty name** → a report notice: whether the default character has an
  override, and which. It reports the *override*, not the baseline — the
  configured style's name is not retained in `CharacterMetadata`, so "the
  configured style" has no name to print.
- **`default`** → restore the open-time appearance from `runtime_info()`,
  erase the override, snapshot, notice. The restore cannot fail: the default
  character is always in the roster and in runtime info.
- **otherwise** → run the resolver; an `invalid_argument` becomes the error
  notice unchanged. On success, mutate the roster, record the override,
  snapshot, notice.

The four notice strings are fixed here so the tests, the command table and
`application-guide.md` cannot drift apart:

```text
set       "<Name> now uses style '<id>' for this session."
report    "<Name>'s style override for this session is '<id>'."
report    "<Name> is using its configured style for this session."   (no override)
reset     "<Name> is back to its configured style for this session."
```

Every form sets `input_consumed`. The mutating forms (`default`, set) call
`require_snapshot()` — unlike `/provider`, the change is browser-visible, so
the update is notice **and** snapshot, the same shape
`set_default_character()` returns. The report form is notice-only.

Unlike `/@Name` and `/!Name`, nothing here persists. The notice says "for
this session" precisely because its sibling commands silently save; the
asymmetry must be audible in the feedback.

### Wiring: the workspace hands the controller a function, not a path

`session_open.cpp` builds the resolver as a closure over
`WorkspaceDefinition` (which owns `styles_directory`), passed as the new last
positional argument to `from_shared_definitions()`:

```cpp
[&model](std::string_view name) {
    return model.resolve_session_style(name);
}
```

`OpenedSession` gains nothing. The closure borrows the process-lived model
for the session's life — the same borrow the provider resolver and the
persist callbacks already take — and `resolve_session_style()` only reads a
file under `styles_directory`, so it never touches the document lock the
character-settings PATCH holds.

### Web grammar: one argument-carrying exact command

- `text_command.*`: `CommandKind::session_style`, descriptor
  `{"/style", CommandKind::session_style}` in the `exact` form — the argument
  arrives in `command.argument` like `/provider`'s.
- `text_input.cpp`: dispatch before the generic argument rejection, mirroring
  the provider branch:

  ```cpp
  if (command.kind == CommandKind::session_style) {
      result.clear_input = true;
      result.session = controller.set_session_style(command.argument);
      return result;
  }
  ```

  The `switch` on `CommandKind` is exhaustive under `-Wswitch`; the new
  enumerator needs the same dummy case `/mcast` and `/provider` have.
- No persist callback, no `CommandResult` field, no route, no
  `resources/cha.yaml`, no generated browser types, no webapp change.
  `command_names()` picks the new spelling up automatically.

### What the command never touches

- **Generation.** No backend, executor, batch, or worker interaction; the
  provider override machinery is entirely uninvolved. A `/provider` override
  and a `/style` override on the same character are independent.
- **Configuration files.** `character.toml` and forum configs are not read
  for selection and never written. The only file read at command time is the
  style config being resolved.
- **Persistence schema.** No journal write, no history-epoch interaction; a
  restored session starts from configured truth.
- **Protocol shape.** `appearance` is already a snapshot field; no DTO,
  OpenAPI, or browser change.
- **Model context.** Appearance never reaches a provider request.
- **`/info` and `/characters` output.** Those notices report model and API,
  not appearance; they are unchanged.

## What we are deliberately not doing

- **No save variant.** Persisting a style already exists as the Settings
  screen with its documented restart semantics.
- **No mid-generation exemption at the web gate.** The command could safely
  run during a generation, but exempting it from the gate is new grammar
  machinery for a marginal convenience; wait-for-finish matches `/provider`.
- **No style name in `/info` or the composer line.** The visible change is
  its own confirmation; the report form covers the rest.
- **No per-target override syntax for multicast.** Targets keep their own
  per-character overrides; that composition needs no grammar.

## Known limitations

- A style config whose ID is literally `default` cannot be selected through
  the command (the word is the reset spelling). The Settings screen is
  unaffected.
- The bare report form can name an override but not the baseline style,
  because `CharacterMetadata` retains an appearance, not the style ID it came
  from. It can only say "its configured style".
- As with `/provider`, the override is process-local: a second `chaweb`
  process holding a session of the same forum never sees it. That is the
  lease model's existing shape, not new state to manage.
