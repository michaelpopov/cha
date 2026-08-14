# A session-scoped provider override (`/provider`)

## What this is

A character's provider can be changed from the browser today (Characters →
character → Settings), but that is a deliberately heavy operation: it writes
`character.toml`, then shuts down and restarts every live session the change
can affect. This change adds the light counterpart: a chat command that swaps
the provider of the session's current default character **for the running
session only**.

```text
/provider terra       "Montaigne now uses provider 'terra' for this session."
/provider             "Montaigne's provider override for this session is 'terra'."
/provider default     "Montaigne is back to its configured provider for this session."
```

The requirements, restated:

1. **Runtime only.** Nothing is written to `character.toml`, forum configs, or
   the session database. The override lives and dies with the live session.
2. **Session-scoped and character-scoped.** The command targets whoever the
   session's current default character is at command time. Other characters,
   other sessions, and later runs of this session are unaffected.

## What exists today

- **Provider configs** live only in `system/providers/<id>/config.toml`.
  Runtime resolution by name is already a supported operation: the
  character-settings PATCH validates a save with `require_path_component()` +
  `load_named_provider()` (`workspace_definition.cpp:1060`), and
  `make_backend_config()` (`agents/character_config.h:89`) materializes a
  complete `ModelBackendConfig` from one provider file, filling absent fields
  with defaults — never merging with another config.
- **Backends are baked at session open.** `open_session()` re-resolves the
  forum's `CharacterDefinition`s from disk and hands them to
  `SessionController::from_shared_definitions()`; `GenerationExecutor` then
  owns one `ProviderClient` per character for the session's life
  (`generation_executor.cpp:49`). Batch executions borrow those backends;
  nothing mutates a backend after construction.
- **State change and persistence are already separate steps** in the command
  pipeline. `set_default_character()` mutates controller state and sets a
  notice; the config-file write is a callback owned by `LiveSession`, wired
  from `OpenedSession`, invoked only when the session state changed
  (`live_session.cpp:466`). A runtime-only command needs no such callback —
  the plumbing has no persistence step to suppress.
- **Busy discipline.** `SessionController::busy()` covers an active response
  or batch; commands that cannot run mid-generation (`/clear`, `/@Name`)
  answer with `busy_notice()`. The web grammar additionally gates every
  command except `/stop` behind `is_generating()` (`text_input.cpp:43`).
- **Command grammar.** `web/text_command.cpp` maps slash spellings to typed
  `CommandKind`s; `handle_text_input()` dispatches them to typed controller
  methods. `/mcast` is the precedent for a command that takes an argument;
  `set_default_character()` is the precedent for a controller method that
  handles its own empty-handle usage notice.
- **Notices are presentation state.** `ControllerUpdate::notice` reaches the
  browser as a transient banner and is never durable. That is the whole
  feedback channel this feature needs: the override appears in no snapshot
  field, so no snapshot-vs-append question arises.
- **Layering.** `session/` must not learn the workspace file layout; today the
  controller receives already-resolved definitions. Resolving a provider name
  is workspace knowledge and must be injected, not learned.

## Decisions

### Scope: a per-character override map, session lifetime

The controller holds `std::unordered_map<CharacterId, std::string>
provider_overrides_` (character ID → provider name, kept for the report form).
The command applies to the current default character and is keyed to that
character, so switching default with `/@Name` and back keeps each character's
override intact, and `/mcast` targets each use their own (possibly overridden)
provider with no special syntax.

The map is never cleared during the session: `/clear` advances the history
epoch and is orthogonal to backend choice, and tying the two would be a rule
to explain. The override ends exactly when the live session does: `/exit`,
reopen, a character-settings `reloading` restart (which reverts to file
truth, consistently), or process shutdown.

### Resolution: complete replacement, validated at command time

`WorkspaceDefinition` gains:

```cpp
[[nodiscard]] ModelBackendConfig resolve_session_provider(std::string_view name) const;
```

It validates the name (`require_path_component()`), loads it with
`load_named_provider()`, and materializes with `make_backend_config()` — the
same parse the session runtime and the settings PATCH use, so command-time
and startup-time semantics cannot drift. Resolution is replacement, in line
with provider layering everywhere else: absent fields in the named config
fall back to `ModelBackendConfig` defaults, not to the character's previous
backend.

On failure the method throws `std::invalid_argument` whose message names the
problem and lists the available provider IDs — the user answers an error
notice at the keyboard, not a failed turn later.

### Backend replacement: the executor rebuilds one slot through a factory

`GenerationExecutor` owns backend lifetime, so replacement belongs there. The
definitions constructor gains a backend factory and the executor retains a
copy of the definitions as rebuild recipes:

```cpp
using BackendFactory =
    std::function<std::unique_ptr<ModelBackend>(CharacterDefinition)>;

GenerationExecutor(
    std::vector<CharacterDefinition> definitions,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    BackendFactory backend_factory = {});   // default: ProviderClient

void replace_backend(CharacterId character_id, const ModelBackendConfig& config);
void reset_backend(CharacterId character_id);
```

`replace_backend()` copies the retained definition, assigns the new backend
config, runs it through the factory, then swaps the slot and refreshes that
index's `runtime_info_` from the new backend's `info()`. Construction is
complete before the swap: a factory throw leaves `backends_` untouched. An
unknown ID throws, as staging already does for unknown targets.
`reset_backend()` is the same operation against the retained definition's own
`backend` config: the recipes live here, so the reset does too and no caller
needs a second copy of them.

Construction validates its backends in `build_runtime_info()` — non-null,
syntactically valid IDs and display names, unique across the forum — and a
swap must not step around that. Both `characters_` in the controller and
`backend_index()` here assume a slot's character ID never changes, so
`replace_backend()` rejects a null factory result and a backend whose
`info().character.id` differs from the slot's. The remaining checks are
uniqueness properties the swap cannot disturb: it replaces one slot with a
backend for the same character.

Consequences:

- **`/info` reflects the change for free.** `format_session_information()`
  reads `runtime_info()`, and the rebuilt backend reports its new model and
  API. No new snapshot field, no provider-ID bookkeeping in
  `ModelBackendInfo`.
- **Reset is a replacement**, not a restore path: `/provider default` calls
  `reset_backend()`, which re-materializes the retained definition's original
  `backend` config, i.e. the open-time effective backend, forum overrides
  included. The controller keeps no copy of those configs. `default` is a
  reserved word, intercepted before the resolver runs; a provider config
  actually named `default` is unreachable through the command (see
  limitations).
- **Safety comes from the existing discipline, not new locking.** Commands
  run on the owner thread; workers borrow backends only inside a live batch,
  and a live batch is exactly what `busy()` reports. The controller rejects
  the command while busy, so the swap can never race a generation. Destroying
  the old `ProviderClient` on the owner thread is therefore destruction of an
  idle object.

The factory seam is what keeps the executor testable: unit tests pass a
factory returning fake backends recording their configs and observe the swap
directly. The default factory preserves today's `build_backends()` semantics,
including its error wrapping.

### Controller: one typed method behind an injected resolver

```cpp
using ProviderResolver = std::function<ModelBackendConfig(std::string_view)>;

[[nodiscard]] ControllerUpdate set_session_provider(std::string_view name);
```

The resolver is a constructor argument on the production constructor,
`from_shared_definitions()`, and the definitions-based test constructor,
defaulted to empty on all three and placed after the existing defaulted
`restored` (the same slot `from_backends_for_testing()` already uses for
`ActivationHook`). An absent resolver yields a fixed notice ("Provider
override is not available in this session.") and is what keeps every existing
call site compiling — both `session_open.cpp:35` and
`tests/support/test_live_session.h:325` pass `restored` as the last
positional argument. Session open supplies the real closure; the test helper
does not need one.

The method handles its own argument forms, following the
`set_default_character()` precedent:

- **busy** → `busy_notice()`.
- **empty name** → a report notice: whether the default character has an
  override, and which. It reports the *override*, not the baseline —
  `ModelBackendConfig` retains no provider ID by design (resolution forgets
  which layer won), so "the configured provider" has no name to print.
- **`default`** → call `reset_backend()` first; only on success erase the
  override and notice. A failed rebuild must not report "configured
  provider" while the override backend is still in the slot.
- **otherwise** → run the resolver; an `invalid_argument` becomes the error
  notice unchanged. Then `replace_backend()`. Catch `std::exception` around
  that call (and around `reset_backend()`): `ProviderClient` construction
  throws `std::runtime_error` when `api_key_env` is unset
  (`provider_client.cpp:287`), the default factory rewraps it the way
  `build_backends()` already does (`generation_executor.cpp:59`), and
  `LiveSession::owner_loop` maps an uncaught exception to
  `ShutdownReason::session_failed` (`live_session.cpp:395`). Turn the
  message into the error notice and do not record the override. On success,
  record the override and notice.

The four notice strings are fixed here so the tests, the command table and
`application-guide.md` cannot drift apart:

```text
set       "<Name> now uses provider '<id>' for this session."
report    "<Name>'s provider override for this session is '<id>'."
report    "<Name> is using its configured provider for this session."   (no override)
reset     "<Name> is back to its configured provider for this session."
```

Every form sets `input_consumed` and returns a **notice-only** update — the
same shape `/info` uses. The override appears in no snapshot, so
`SnapshotRequired` has nothing to carry, and no append classification is
involved.

Unlike `/@Name` and `/!Name`, nothing here persists. The notice says "for
this session" precisely because its sibling commands silently save; the
asymmetry must be audible in the feedback.

### Wiring: the workspace hands the controller a function, not a path

`session_open.cpp` builds the resolver as a closure over
`WorkspaceDefinition` (which owns `providers_directory`):

```cpp
[&model](std::string_view name) {
    return model.resolve_session_provider(name);
}
```

passed as the new last positional argument to `from_shared_definitions()`,
after `restored`. `OpenedSession` gains nothing — the resolver goes into the
controller and is reachable only through the command. The `session/` layer
sees only a `std::function`; workspace file layout stays in `workspace/`.

The closure outlives `open_session()`: the controller stores it and calls it
every time `/provider` runs, so the captured reference must outlive the session.
It does — `WorkspaceDefinition` is the authoritative workspace for one server
process, loaded at startup and outliving every session opened against it,
which is the same borrow `persist_default_character` already takes. Calling it
from the session's owner thread is safe for a second reason:
`resolve_session_provider()` only reads a file under `providers_directory`,
so it never touches the document lock the character-settings PATCH holds.

### Web grammar: one argument-carrying exact command

- `text_command.*`: `CommandKind::session_provider`, descriptor
  `{"/provider", CommandKind::session_provider}` in the `exact` form — the
  argument arrives in `command.argument` like `/mcast`'s.
- `text_input.cpp`: dispatch before the generic argument rejection, mirroring
  the multicast branch:

  ```cpp
  if (command.kind == CommandKind::session_provider) {
      result.clear_input = true;
      result.session = controller.set_session_provider(command.argument);
      return result;
  }
  ```

  The `switch` on `CommandKind` (`text_input.cpp:69`) is exhaustive under
  `-Wswitch`; `/mcast` has both the early branch and a dummy
  `case CommandKind::mcast: return result;` (`text_input.cpp:78`). The new
  enumerator needs the same dummy case.

  The generating gate above it already answers mid-generation attempts with
  the in-progress notice; the controller's own `busy()` guard keeps the typed
  action safe independent of the grammar.
- No persist callback, no `CommandResult` field, no route, no
  `resources/cha.yaml`, no generated browser types, no webapp change.
  `command_names()` picks the new spelling up automatically for the
  unknown-command notice.

### What the command never touches

- **Configuration files.** `character.toml` and forum configs are not read
  for selection and never written. The only file read at command time is the
  provider config being resolved.
- **Persistence schema.** No journal write, no `turns`/`entries` row, no
  history-epoch interaction. The SQLite database cannot observe that an
  override ever existed; a restored session starts from configured truth.
- **Protocol and snapshot.** No DTO, OpenAPI, or browser change.
- **Model context.** `project_model_context()` is untouched; the provider
  switch changes where the next request goes, never what it contains.
- **The roster.** `ForumCharacters`, display names, styles, and mention
  resolution are unaffected.

## What we are deliberately not doing

- **No `/style` command.** Appearance is presentation data living in the
  roster and snapshot — a different mechanism (no backend, `SnapshotRequired`,
  a browser-visible field). A natural follow-up, not bundled.
- **No save variant.** Persisting a provider change already exists as the
  Settings screen with its documented restart semantics; a command that
  sometimes wrote config would blur exactly the line this feature draws.
- **No mid-generation switch.** Busy rejection only. Interrupting a live
  answer to reroute it is `/stop` plus a new prompt, which the user can
  already do.
- **No provider name in `/info` or the composer line.**
  `ModelBackendInfo` reports the new backend's model and API after a swap;
  carrying the provider ID into the snapshot would duplicate resolution
  knowledge for one display line.
- **No per-target override syntax for multicast.** Targets keep their own
  per-character overrides; that composition needs no grammar.

## Known limitations

- A provider config whose ID is literally `default` cannot be selected
  through the command (the word is the reset spelling). The Settings screen
  is unaffected.
- The bare report form can name an override but not the baseline provider,
  because resolution deliberately retains no winning-layer ID. It can only
  say "its configured provider".
- A provider file that parses but whose backend cannot be constructed (unset
  `api_key_env`, failed model discovery) fails at command time: the factory
  throw becomes the error notice and the old backend stays. A backend that
  constructs but cannot serve (bad credentials, dead host) still fails at
  generation as an ordinary turn error — the same split Settings has between
  a failed reload and a later turn error.
- In `Mode::net` with an empty model, `ProviderClient` construction may
  perform HTTP model discovery on the owner thread (up to ~10s), the same
  work session open already does. The command is rejected while busy, so
  this only stalls the owner loop when idle.
- As with `reloading`-based restarts, the override is process-local: a second
  `chaweb` process holding a session of the same forum never sees it. That is
  the lease model's existing shape, not new state to manage.
