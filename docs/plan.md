# Implementation plan — session provider override (`/provider`)

Read `docs/design.md` first. It explains what is being built and why each
choice was made; this file is only the sequence.

The work is split into four blocks. Each block ends with a green build and
leaves the tree in a committable state. Run them in order — B depends on A,
C supplies the resolver B defaulted, D depends on all three.

Verification commands, from the repository root:

```bash
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

```bash
make web-check
```

---

## Block A — executor backend replacement

**Goal:** `GenerationExecutor` can rebuild one character's backend while idle —
from a new `ModelBackendConfig` or from the definition it was built from —
through an injectable factory. Nothing outside `agents/` changes.

### Steps

1. **Factory type and retained definitions.** In
   `src/agents/generation_executor.h` / `.cpp`:
   - Add `using BackendFactory = std::function<std::unique_ptr<ModelBackend>(CharacterDefinition)>;`.
   - Extend the definitions constructor with `BackendFactory backend_factory = {}` and store it.
   - Retain a copy of the definitions vector as `rebuild_recipes_`.
     `CharacterDefinition` is plain value data; one extra copy per session is
     not a resource question. The definitions constructor delegates to the
     backends constructor, so there is no point in the init list to copy from:
     pass the lvalue to `build_backends` (which takes its vector by value and
     therefore copies) and move into the member in the body —
     `: GenerationExecutor(build_backends(definitions), …) { rebuild_recipes_ = std::move(definitions); }`.
     Neither constructor needs restructuring beyond that.
   - An empty factory defaults to today's behavior exactly: construct
     `ProviderClient`, with the same error wrapping `build_backends()` applies
     (`generation_executor.cpp:49-68`) — move that body behind the default.

2. **`replace_backend()` and `reset_backend()`.** New methods:
   - Find the character in `runtime_info_` (same lookup discipline as
     `backend_index()`); unknown ID throws `std::invalid_argument`, matching
     staging's unknown-target behavior.
   - Copy the retained definition, assign `definition.backend = config`, run
     it through the factory, *then* swap the `backends_` slot and refresh
     `runtime_info_[index]` from the new backend's `info()` so `/info`
     reflects the change. A factory throw must leave the old slot in place.
   - Guard the invariants `build_runtime_info()` enforces at construction but
     this path skips: throw on a null factory result, and throw if the new
     backend's `info().character.id` is not the slot's ID. Uniqueness and name
     syntax cannot change under a same-character swap.
   - `reset_backend(character_id)` is `replace_backend()` against the retained
     definition's own `backend` config. It lives here because the recipes do;
     the controller keeps no copy of them.
   - The backends-only test constructor keeps its current signature and gets
     no retained definitions; both methods there throw `std::logic_error` — a
     defensive guard, not a reachable state: controller code that can call
     replace requires the definitions path (Block B ties availability to the
     resolver, which only the definitions path receives).

3. **Tests** (`tests/agents/unit_generation_executor.cpp`):
   - Definitions constructor with a fake factory: backends are built through
     it, in order.
   - `replace_backend()` swaps the backend: a staged request afterwards
     reaches the replacement fake (record which fake served it), the old one
     is idle.
   - `runtime_info()` reports the replacement's model/API after a swap.
   - Unknown ID throws; a factory returning null throws; a factory returning a
     backend for a different character ID throws; a factory that throws leaves
     the existing slot in place.
   - Replace does not change backend count — the one-worker-per-backend pool
     invariant (`generation_executor.cpp` constructor check) still holds.
   - `reset_backend()` after a replace rebuilds a backend with the original
     config (design doc, "Reset is a replacement").

---

## Block B — controller command

**Goal:** `SessionController::set_session_provider()` implements the whole
command semantics — report, reset, set — behind an injected resolver, with
busy discipline and notice-only updates.

### Steps

1. **Resolver type and member.** In `src/session/session_controller.h`:
   - `using ProviderResolver = std::function<ModelBackendConfig(std::string_view)>;`
   - Member `ProviderResolver provider_resolver_;` and
     `std::unordered_map<CharacterId, std::string> provider_overrides_;`
     (character ID → provider name, for the report form).

2. **Constructor plumbing.** The production constructor,
   `from_shared_definitions()`, and the definitions-based test constructor
   each gain a `ProviderResolver` parameter, defaulted to empty and placed
   after the existing defaulted `restored` — the same slot
   `from_backends_for_testing()` already uses for `ActivationHook`. Required
   (or inserted *before* `restored`) would break the two existing
   `from_shared_definitions()` call sites (`workspace/session_open.cpp:35`,
   `tests/support/test_live_session.h:325`), which pass `restored` as the
   last positional argument. The definitions-based test constructor also
   gains an optional `GenerationExecutor::BackendFactory` pass-through;
   `from_backends_for_testing()` leaves the resolver empty. Absent resolver →
   the command answers with the fixed "not available in this session" notice.
   Existing call sites keep compiling; production sessions answer "not
   available" until Block C passes the real closure.

3. **`set_session_provider(std::string_view name)`** in
   `session_controller.cpp`, following the `set_default_character()` shape:
   - `busy()` → `busy_notice()`.
   - Always `input_consumed = true`; always notice-only — never
     `require_snapshot`, never a journal write.
   - Empty name → report the default character's override state (or absence).
     The display name comes from `characters_.find(default_character_id_)`.
     Use the notice strings fixed in the design doc's controller section
     verbatim — the tests assert them and the guide quotes them.
   - `"default"` → `generation_executor_.reset_backend(default_character_id_)`
     first; only on success erase the override and notice. The controller
     stores no backend configs of its own: the executor already retains the
     recipes, and a second copy here would be the same data in two layers,
     free to drift.
   - Otherwise → resolve through `provider_resolver_`; catch
     `std::invalid_argument` and turn it into the notice unchanged. Then
     `replace_backend`. Catch `std::exception` around `replace_backend` and
     `reset_backend` — `ProviderClient` construction throws
     `std::runtime_error` (unset `api_key_env`, failed discovery), and an
     uncaught exception from `handle_text_input` is
     `ShutdownReason::session_failed`. Turn the message into the error
     notice; do not record or erase the override. On success: record
     `provider_overrides_`, notice
     `"<Name> now uses provider '<id>' for this session."`
   - Intercept `"default"` **before** the resolver runs — the resolver must
     never see the reserved word.

4. **Tests** (`tests/session/unit_session_controller.cpp`), built on
   `from_definitions_for_testing()` with a stub resolver (a name→config map,
   or a throwing one) and a recording fake backend factory:
   - Set: notice text, override recorded, the next prompt is served by the
     replacement backend with the resolved config.
   - Bare report with and without an override names it or says "configured
     provider".
   - Reset: restores the open-time backend config, clears the override,
     report reflects it.
   - Per-character: override follows the character, not the default slot —
     set on A, `/@B`, bare report on B shows no override, `/@A` again shows
     A's.
   - Busy rejection: submit a prompt against a blocking fake (existing
     fixtures), run the command, expect `busy_notice()`.
   - Resolver failure → the thrown message becomes the notice, backend and
     override map unchanged.
   - Factory / `ProviderClient` throw from `replace_backend` or
     `reset_backend` → the thrown message becomes the notice, backend and
     override map unchanged, session still alive.
   - Absent resolver (`from_backends_for_testing` path) → "not available"
     notice.
   - `/clear` leaves the override in effect for the next prompt.

---

## Block C — workspace resolver and open wiring

**Goal:** the workspace exposes named-provider resolution as a value-returning
helper, and session open injects it. No controller or executor changes.

### Steps

1. **`WorkspaceDefinition::resolve_session_provider(std::string_view name)`**
   in `src/workspace/workspace_definition.cpp`:
   - `require_path_component(name, …)` → `load_named_provider(config_.providers_directory, name, …)` → `make_backend_config()`.
   - Wrap any failure in `std::invalid_argument` whose message names the
     provider and lists the available IDs via `named_config_ids()` (already
     file-local at `workspace_definition.cpp:561`); mirror the PATCH path's
     "is not usable" phrasing (`workspace_definition.cpp:1060-1106`) so the
     two surfaces read alike.
   - Declaration in `workspace_definition.h` beside the other runtime reads.

2. **Wire at open** in `src/workspace/session_open.cpp`: pass
   `[&model](std::string_view name) { return model.resolve_session_provider(name); }`
   as the new last positional argument to `from_shared_definitions()`, after
   `restored`. `OpenedSession` is unchanged. `tests/support/test_live_session.h`
   keeps the default (no resolver). The closure borrows the process-lived
   `WorkspaceDefinition` and is called throughout the session, not just at
   construction — the same borrow `persist_default_character` already takes
   two lines below.

3. **Tests** (`tests/application/unit_workspace_definition.cpp`), using the
   existing temporary-workspace builder:
   - A known provider name resolves to a complete `ModelBackendConfig`
     (defaults filled, no merging with the workspace default provider).
   - Unknown name, path-component abuse (`../x`), and a malformed provider
     file each throw `invalid_argument` with a message that lists available
     IDs.
   - A session opened after wiring carries a working resolver — assert
     through `open_session()` + one controller command, or keep it at the
     helper level if the former needs more scaffolding than it proves.

---

## Block D — web grammar and documentation

**Goal:** `/provider` parses, dispatches, and reads correctly everywhere a
user can meet it. No protocol or browser change.

### Steps

1. **Parser** (`src/web/text_command.h` / `.cpp`): add
   `CommandKind::session_provider` and descriptor
   `{"/provider", CommandKind::session_provider}` in the `exact` form. The
   argument arrives in `command.argument`; `command_names()` lists it
   automatically.

2. **Dispatch** (`src/web/text_input.cpp`): before the generic
   "Command does not accept arguments" rule, beside the multicast branch:

   ```cpp
   if (command.kind == CommandKind::session_provider) {
       result.clear_input = true;
       result.session = controller.set_session_provider(command.argument);
       return result;
   }
   ```

   The `switch` on `CommandKind` is exhaustive under `-Wswitch`. Add
   `case CommandKind::session_provider: return result;` next to the
   multicast dummy case (`text_input.cpp:78`), same shape as `/mcast`.

   The `is_generating()` gate above already answers mid-generation attempts;
   no new handling.

3. **Tests**:
   - `tests/web/unit_text_command.cpp`: `"/provider terra"` → kind +
     argument; `"/provider"` → kind + empty argument; `"/providerx"` is not
     the command (exact form).
   - `tests/web/unit_text_input.cpp`: dispatch passes the argument through;
     while generating, the input gets the in-progress notice and the
     controller is untouched; input is consumed on all forms.
   - No route test additions: the input route is unchanged and already
     generic over commands.

4. **Documentation**:
   - Root `README.md`: command table row for `/provider`.
   - `resources/application-guide.md`: commands list row, including the
     "for this session" phrasing so Assistant quotes it correctly.
   - `src/web/README.md`: one line where the chat grammar is described.
   - `src/agents/README.md`: the sentence that `GenerationExecutor` "owns
     one backend per forum character" for the session's life
     (`src/agents/README.md:159`) needs a one-line correction — a slot can
     be replaced while idle.
   - `docs/tutorial.md`: it walks the chat commands, `/mcast` included.
   - `docs/web-ui/behavior.md`: only if it enumerates chat commands; check
     before editing.

   `grep -rn "/mcast" --include='*.md' .` lists the files that document the
   command set; `/provider` belongs wherever `/mcast` appears.

5. **Final verification**: full `ctest` run, `make web-check`, and a manual
   smoke against `make run`: open a session, `/provider` (report),
   `/provider <id>` (notice), a prompt (answer via the new backend), `/@` to
   another character and back (override kept), `/provider default` (reset
   notice), `/exit` + reopen (override gone).

---

## Out of scope (per `docs/design.md`)

- `/style`, persistence, mid-generation switching, protocol/OpenAPI/browser
  changes, provider IDs in `/info` or the composer line, e2e additions.
