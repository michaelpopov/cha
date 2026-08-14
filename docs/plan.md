# Implementation plan — session style override (`/style`)

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

## Block A — roster appearance mutator

**Goal:** `ForumCharacters` can change one character's appearance in place.
Nothing else changes; the snapshot path picks it up unchanged.

### Steps

1. **`set_appearance()`** in `src/session/forum_characters.h` / `.cpp`:

   ```cpp
   bool set_appearance(std::string_view id, const CharacterAppearance& appearance);
   ```

   Find by ID, assign `appearance` on the stored `CharacterMetadata`, return
   true; unknown ID returns false. No other field is reachable, so the
   construction invariants (non-empty, unique IDs, unique folded names,
   syntax) hold by inspection — say so in a comment.

2. **Tests** (`tests/session/unit_forum_characters.cpp`, extending the
   existing roster tests):
   - Set: `find(id)->appearance` reports the new value; `all()` reflects it.
   - Unknown ID returns false and changes nothing.
   - IDs, display names, ordering, and handle resolution are untouched by a
     mutation.

---

## Block B — controller command

**Goal:** `SessionController::set_session_style()` implements the whole
command semantics — report, reset, set — behind an injected resolver, with
snapshot-carrying updates.

### Steps

1. **Resolver type and members** in `src/session/session_controller.h`:
   - `using StyleResolver = std::function<CharacterAppearance(std::string_view)>;`
   - Members `StyleResolver style_resolver_;` and
     `std::unordered_map<CharacterId, std::string> style_overrides_;`
     (character ID → style name, for the report form), beside the provider
     override state.

2. **Constructor plumbing.** The production constructor,
   `from_shared_definitions()`, and the definitions-based test constructor
   each gain a `StyleResolver style_resolver = {}` parameter, appended after
   the `provider_resolver` slot (after `backend_factory` on the test
   constructor). Defaulted, so the two existing `from_shared_definitions()`
   call sites (`workspace/session_open.cpp`,
   `tests/support/test_live_session.h`) and all test call sites keep
   compiling; production sessions answer "not available" until Block C passes
   the real closure.

3. **`set_session_style(std::string_view name)`** in
   `session_controller.cpp`, mirroring `set_session_provider()` with the
   differences the design fixes:
   - **No `busy()` check** — appearance touches no generation machinery; the
     web gate owns mid-generation behavior.
   - Always `input_consumed = true`. The mutating forms call
     `require_snapshot(update)`; the report form is notice-only.
   - Empty name → report the default character's override state (or absence),
     using the design doc's notice strings verbatim.
   - `"default"` → restore the open-time appearance: find the character in
     `generation_executor_.runtime_info()` and apply its
     `character.appearance` through `characters_.set_appearance()`. Intercept
     the word **before** the resolver runs. The restore cannot fail (the
     default is always in both collections), but tolerate a false return by
     still reporting the configured state.
   - Otherwise → resolve through `style_resolver_`; catch
     `std::invalid_argument` and turn it into the notice unchanged. On
     success: `characters_.set_appearance()`, record `style_overrides_`,
     snapshot, notice `"<Name> now uses style '<id>' for this session."`
   - Absent resolver → the fixed "Style override is not available in this
     session." notice.

4. **Tests** (`tests/session/unit_session_controller.cpp`), built on
   `from_definitions_for_testing()` with a stub resolver (a name→appearance
   map, or a throwing one):
   - Set: notice text, `requires_snapshot(update)` is true, and
     `view().characters` shows the new appearance for that character only.
   - Bare report with and without an override names it or says "configured
     style"; report is notice-only (no snapshot).
   - Reset: restores the open-time appearance in the view, clears the
     override, snapshot required.
   - Per-character: override follows the character, not the default slot —
     set on A, `/@B`, report on B shows no override, `/@A` again shows A's.
   - Resolver failure → the thrown message becomes the notice; appearance
     and override map unchanged.
   - Absent resolver (`from_backends_for_testing` path) → "not available"
     notice.
   - `/clear` leaves the override in effect (view still shows it).
   - Mid-generation: with a blocking fake backend and a prompt in flight,
     the typed call succeeds — this pins the deliberate absence of a busy
     guard (the web gate is what rejects the command during generation).

---

## Block C — workspace resolver and open wiring

**Goal:** the workspace exposes named-style resolution as a value-returning
helper, and session open injects it. No controller changes.

### Steps

1. **`WorkspaceDefinition::resolve_session_style(std::string_view name)`**
   in `src/workspace/workspace_definition.cpp`, beside
   `resolve_session_provider()`, mirroring its structure exactly:
   - `require_path_component(name, config_.styles_directory)`, then construct
     the path (`config_.styles_directory / path_from_utf8(name) / "config.toml"`)
     and check `is_regular_file` **before** calling `load_named_style`. Do not
     call `load_named_style` on a missing config: its absent-file message is
     framed around a *reference path* ("Config file '<X>' references style
     '<name>' without config file …"), but the name came from the keyboard and
     nothing references it. Throw a keyboard-appropriate message here instead
     ("no style config is installed under this name"), the same reason and
     shape the provider helper uses (see its comment at
     `workspace_definition.cpp` around the `is_regular_file` check). Only on a
     present file call `load_named_style(config_.styles_directory, name, path)`,
     passing the constructed `path` as the reference path.
   - Wrap any failure in `std::invalid_argument` with the same try/catch shape
     as the provider helper — catch `std::exception`, so `require_path_component`
     abuse (`../x`) flows through too — whose message names the style and lists
     the available IDs via `named_config_ids(config_.styles_directory)`,
     mirroring the provider helper's phrasing ("Style '<name>' is not
     usable: … Available styles: …").
   - Declaration in `workspace_definition.h` beside the provider one.

2. **Wire at open** in `src/workspace/session_open.cpp`: pass
   `[&model](std::string_view name) { return model.resolve_session_style(name); }`
   as the new last positional argument to `from_shared_definitions()`.
   `OpenedSession` is unchanged; the closure is the same process-lived borrow
   as the provider resolver one line above.

3. **Tests** (`tests/application/unit_workspace_definition.cpp`), beside the
   provider-resolution tests:
   - A known style name resolves to the configured `CharacterAppearance`.
   - Unknown name, path-component abuse (`../x`), and a malformed style file
     each throw `invalid_argument` with a message that lists available IDs.

---

## Block D — web grammar and documentation

**Goal:** `/style` parses, dispatches, and reads correctly everywhere a user
can meet it. No protocol or browser change.

### Steps

1. **Parser** (`src/web/text_command.h` / `.cpp`): add
   `CommandKind::session_style` and descriptor
   `{"/style", CommandKind::session_style}` in the `exact` form.
   `command_names()` lists it automatically.

2. **Dispatch** (`src/web/text_input.cpp`): before the generic
   "Command does not accept arguments" rule, beside the provider branch:

   ```cpp
   if (command.kind == CommandKind::session_style) {
       result.clear_input = true;
       result.session = controller.set_session_style(command.argument);
       return result;
   }
   ```

   Add `case CommandKind::session_style: return result;` to the exhaustive
   `switch`, same shape as the `/mcast` and `/provider` dummy cases.

3. **Tests**:
   - `tests/web/unit_text_command.cpp`: `"/style sans-bold"` → kind +
     argument; `"/style"` → kind + empty argument; `"/stylex"` is not the
     command. Update the `command_names()` list assertion — it names every
     accepted command and will fail until `/style` is added.
   - `tests/web/unit_text_input.cpp`: dispatch passes the argument through
     and sets no persist fields; while generating, the input gets the
     in-progress notice and the controller is untouched; input is consumed
     on all forms.
   - No route test additions: the input route is unchanged and already
     generic over commands.

4. **Documentation**:
   - Root `README.md`: command table row for `/style`.
   - `resources/application-guide.md`: commands list row, including the
     "for this session" phrasing so Assistant quotes it correctly.
   - `src/web/README.md`: add `/style` to the chat-grammar command list.
   - `docs/tutorial.md`: extend the §12.3 `/provider` sentence with the
     style counterpart (roster mutation + snapshot, no resolver hazard).
   - `docs/web-ui/behavior.md`: only if it enumerates chat commands; check
     before editing. (`grep -rn "/provider" --include='*.md' .` lists the
     files that document the command set.)

5. **Final verification**: full `ctest` run, `make web-check`, and a manual
   smoke against `make run`: open a session, `/style` (report),
   `/style <id>` (notice + visible font change on the character's messages,
   past ones included), `/@` to another character and back (override kept),
   `/style default` (reset), `/exit` + reopen (override gone).

---

## Out of scope (per `docs/design.md`)

- Persistence, mid-generation gate exemption, style names in `/info` or the
  composer line, per-target multicast syntax, protocol/OpenAPI/browser
  changes, e2e additions.
