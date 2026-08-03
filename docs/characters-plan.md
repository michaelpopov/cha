# Workspace-level characters: implementation plan

Status: ready for implementation, 2026-08-03.

This plan implements the contract in
[`characters-design.md`](characters-design.md). The design document is the source
of truth for behavior; this document supplies the execution order, concrete code
changes, tests, migration work, and completion criteria.

The implementation is complete only when the source code, tests, checked-in
workspace, and user-facing documentation all describe and use the new layout.

This plan has been checked clause by clause against the design. It adds one
thing the design does not mention — splitting `Config::display_name` from
`Config::name` in section 1.1 — which conforms to the design's vocabulary rather
than changing its contract. Everything else here is an execution detail of a
design decision, and no clause of the design is contradicted. The design remains
the source of truth; if implementation reveals a conflict, change the design
first.

## 1. Decisions that must not be revisited during implementation

Implement these decisions as written:

- Character definitions are workspace-level under `characters/<id>/`.
- Forum membership is explicit through `forums/<forum>/members/<id>/`.
- An empty member directory is a valid membership record.
- `members/<id>/character.toml` is optional.
- `members/<id>/CHARACTER.md` is optional and, when present, replaces the
  definition's `CHARACTER.md` completely.
- Configuration files never replace lower-level configuration files wholesale.
  They overlay them one key at a time.
- Configuration precedence, weakest to strongest, is:
  1. `characters/<id>/character.toml`;
  2. `forums/<forum>/members/character_defaults.toml`;
  3. `forums/<forum>/members/<id>/character.toml`.
- The `[prompt]` table follows the same three-layer, per-key precedence.
- `display_name` and `tags` are definition-only fields. Their presence in either
  forum-local configuration layer is an error.
- `Config` carries `display_name` and `name` as two separate fields.
  `display_name` is loaded from the definition's `display_name` key and is the
  value used everywhere a character's display name is needed. `name` is reserved
  for a future distinct concept: it keeps its built-in default, is never written
  by the loader, and has no TOML key. Rejecting the `id` and `name` TOML keys as
  removed identity fields is existing behavior and is preserved in all three
  layers.
- Tags use ASCII case folding for duplicate detection and grouping.
- A definition-level `CHARACTER.md` is contained to the workspace-level
  `characters/` directory. A member-level `CHARACTER.md` and `FORUM.md` are
  contained to their forum directory.
- Do not introduce a special `characters/shared/` directory, a shared-file data
  model, or a shared-file API. Files included by templates remain ordinary files
  governed only by the containment rule.
- Character and agent ordering remains lexicographic by character ID.
- `default_agent` is a character ID in the forum's `config.toml`. It is passed to
  `SessionController` separately and never changes character ordering.
- Workspace validation is intentionally all-or-nothing.
- Live reload and post-construction filesystem edits do not need a new design.
- Forum distribution/bundling and the cost of scanning all memberships are not
  blockers for this implementation.
- Web/lobby character endpoints and the Characters UI remain out of scope.

## 2. Target filesystem layout

The implemented workspace must have this shape:

```text
workspace/
  app.toml
  personas/
    <persona-id>/
      persona.toml
      PERSONA.md                 # optional, unchanged
  characters/
    <character-id>/
      character.toml             # required definition
      CHARACTER.md               # required definition prompt
      ...                        # ordinary files reachable by template includes
  forums/
    <forum-id>/
      config.toml                # display_name; optional default_agent
      FORUM.md
      members/
        character_defaults.toml  # optional forum-wide overlay
        <character-id>/           # directory presence means membership
          character.toml         # optional per-member overlay
          CHARACTER.md           # optional full prompt replacement
      sessions/                  # unchanged
```

The old paths must no longer be read:

```text
forums/<forum>/characters/<id>/character.toml
forums/<forum>/characters/<id>/SYSTEM.md
forums/<forum>/characters/character_defaults.toml
```

Do not add compatibility fallback for the old layout unless the design document
is explicitly changed first. A partial migration must fail clearly instead of
silently mixing layouts.

## 3. Implementation order

Follow the phases below in order. Keep the tree buildable at the end of every
phase when practical. Add or update the focused tests in the same phase as the
production behavior they cover.

### Phase 0: establish the baseline

1. Configure and build the current tree:

   ```sh
   cmake --preset ninja
   cmake --build --preset ninja
   ```

2. Run the existing suite before changing behavior:

   ```sh
   ctest --test-dir build/ninja --output-on-failure
   ```

3. Record any pre-existing failure. Do not attribute a pre-existing failure to
   this migration.

### Phase 1: represent definition metadata and three configuration layers

Primary files:

- `src/agents/config.h`
- `src/agents/config.cpp`
- `tests/agents/unit_config_loader.cpp`

#### 1.1 Separate `display_name` from `name` in `Config`

`Config::name` is currently the display name: `build_config()` assigns it from
the parsed `display_name` key, and every display-name consumer reads
`config.name`. Split the two before anything else in this phase, so the rest of
the work uses one unambiguous field.

- Add `std::string display_name{"Assistant"};` to `Config` and assign it from the
  definition's `display_name` in `build_config()`.
- Keep `std::string name{"Assistant"};` as a reserved field. Do not parse it, do
  not assign it, and do not delete it. Comment it as reserved for a future
  character-name concept distinct from the display name.
- Keep rejecting the `name` and `id` TOML keys in every layer. A reserved struct
  field is not an authorable key; accepting `name` in TOML would silently do
  nothing.
- Migrate every current reader of `config.name` to `config.display_name`:
  `src/session/workspace.cpp` (`CharacterInfo` construction),
  `src/agents/agent.cpp` (the `character.display_name` template variable, the
  other-agent roster in `forum_context()`, JSONL speaker encoding, and the
  persona collision check), and `src/agents/agent_registry.cpp`. Update the
  tests that set or assert `config.name` in `tests/agents/`, `tests/session/`,
  and `tests/integration/`.

`CharacterInfo` stays `{id, name}` per the design; its `name` member is now fed
from `config.display_name`. `forum_characters.*` remains unchanged.

#### 1.2 Add a definition metadata value

Add a small value type for data needed at workspace validation time. A suitable
shape is:

```cpp
struct CharacterDefinitionMetadata {
    std::string id;
    std::string display_name;
    std::vector<std::string> tags;
};
```

Expose a loader that takes the required definition `character.toml` path and
returns this value without requiring an effective `host` or `port`. Derive `id`
from the definition directory name, require and validate `display_name`, and
parse and validate `tags`.

Keep parsing logic shared with the session-time configuration loader. Do not
write two independent TOML parsers for the same fields.

#### 1.3 Make configuration-layer roles explicit

Replace the current boolean `base` distinction in `parse_config()` with an
explicit internal layer enum, for example:

```cpp
enum class ConfigLayer {
    definition,
    forum_defaults,
    member_override,
};
```

Enforce layer-specific fields while parsing:

| Field | Definition | Forum defaults | Member override |
| --- | --- | --- | --- |
| `display_name` | required | forbidden | forbidden |
| `tags` | optional | forbidden | forbidden |
| connection/behavior fields | optional | optional | optional |
| `[prompt]` | optional | optional | optional |
| `id` / `name` keys | forbidden | forbidden | forbidden |

Errors must name the offending file and field.

The `id` and `name` rows preserve behavior that `parse_config()` already has:
both are removed identity keys and are rejected with the existing "use the
directory name and 'display_name'" wording. No workspace TOML currently authors
them, so no checked-in file needs migrating for this row. The `display_name` row
is the change: it moves from "rejected in the base file" to "required in the
definition, rejected in both forum-local layers."

#### 1.4 Implement tag parsing and validation

For definition-level `tags`:

- The field is optional and defaults to an empty vector.
- It must be a TOML array containing only strings.
- Trim each authored string using the project's existing whitespace trimming
  convention.
- Reject a tag that is empty after trimming.
- Reject control characters and line breaks.
- Preserve the trimmed authored spelling in `CharacterDefinitionMetadata::tags`.
- Use `fold_ascii()` as the comparison key.
- Reject duplicates within one character after ASCII folding; for example,
  `"Stoic"` and `"stoic"` conflict.
- Do not add tags to the runtime `Config`; tags are workspace metadata, not
  provider configuration.

#### 1.5 Replace the two-path loader with a three-layer request

Change the public configuration loader so call sites cannot accidentally swap
layer meanings. Prefer a named request value over three positional paths, for
example:

```cpp
struct CharacterConfigPaths {
    std::filesystem::path definition;
    std::optional<std::filesystem::path> forum_defaults;
    std::optional<std::filesystem::path> member_override;
};

LoadedConfig load_config(const CharacterConfigPaths& paths);
```

The implementation must:

1. parse the required definition;
2. start with built-in `Config` defaults;
3. apply definition behavior fields and `[prompt]` values;
4. apply forum defaults per key;
5. apply the member override per key;
6. take `id`, `display_name`, and `tags` only from the definition;
7. require effective `host` and `port` only when building `LoadedConfig` for an
   agent;
8. validate the effective port and temperature and attribute an invalid
   effective value to the highest-precedence file that supplied it.

Track the source path of effective values where needed for accurate diagnostics.
Do not report the definition as the source of a bad value supplied by a forum
default or member override.

#### 1.6 Configuration-loader tests

Update existing tests to the named three-layer API and add cases for:

- definition-only configuration;
- missing optional forum-default and member-override files;
- definition < forum defaults < member override precedence for every runtime
  field;
- the same precedence for `[prompt]` keys;
- a member `character.toml` that is empty, and one that contains only comments,
  each acting as a no-op overlay and leaving every effective value and `[prompt]`
  key at its lower-layer value;
- the same for an empty and a comment-only `character_defaults.toml`;
- `display_name` populated from the definition and `Config::name` left at its
  built-in default after a full three-layer load;
- the `name` and `id` TOML keys still rejected in all three layers;
- identity derived from the definition directory even when a member override is
  present;
- missing definition `display_name`;
- `display_name` rejected in both forum-local layers;
- `tags` rejected in both forum-local layers;
- absent tags and an empty tag array;
- trimming and authored-case preservation;
- non-string tag elements;
- empty, control-containing, and line-break-containing tags;
- ASCII-case duplicate tags;
- correct source paths in three-layer validation errors.

Run the focused test binary after this phase. Use `--gtest_filter=Config.*` if
the configured build exposes the current aggregate `cha_tests` executable.

### Phase 2: load agent definitions from definition/member pairs

Primary files:

- `src/agents/agent.h`
- `src/agents/agent.cpp`
- `tests/agents/unit_agent_definition_loader.cpp`

#### 2.1 Introduce an explicit source description

Replace the current vector of forum-local character directories with one source
value per member. A suitable shape is:

```cpp
struct AgentDefinitionSource {
    std::filesystem::path definition_directory;
    std::filesystem::path member_directory;
};
```

Change `load_agent_definitions()` to receive the ordered sources, the forum
directory and display name, the persona roster, and the optional forum-default
configuration path. The source vector order is authoritative and must be
preserved.

#### 2.2 Resolve configuration paths

For each source:

- Require `definition_directory/character.toml`.
- Use the common optional
  `forum/members/character_defaults.toml` as the middle layer.
- Use `member_directory/character.toml` only when it is a regular file.
- If an optional path exists but is not a regular file, report a load error.
- Call the three-layer configuration loader from Phase 1.
- Derive identity only from the definition directory and definition config.

#### 2.3 Resolve the character prompt

For each source:

1. Require the definition's `CHARACTER.md` as part of a valid definition.
2. If `member_directory/CHARACTER.md` is a regular file, select it.
3. Otherwise select `definition_directory/CHARACTER.md`.
4. If an optional member prompt path exists but is not a regular file, fail.

Create separate `TemplateOptions` values for character and forum expansion:

- Definition prompt selected:
  `containment_root = definition_directory.parent_path()`, which is the
  workspace-level `characters/` directory.
- Member prompt selected:
  `containment_root = forum_directory`.
- `FORUM.md`:
  `containment_root = forum_directory`.

All three use the same effective `[prompt]` scope and reserved variables. Keep
the existing reserved values:

- `character.id`;
- `character.display_name`;
- `forum.id`;
- `forum.display_name`.

Keep independent template counters for `CHARACTER.md` and `FORUM.md`, as today.

#### 2.4 Keep prompt construction unchanged

The effective prompt order remains:

1. expanded selected `CHARACTER.md`;
2. expanded `FORUM.md`;
3. complete static persona roster;
4. generated forum context.

Do not change context projection, JSONL history encoding, or participant naming.

Remove the old `SYSTEM.md` path and error wording. Diagnostics must identify
`CHARACTER.md` and the character ID.

Move persona/character collision enforcement to workspace validation in Phase 3.
This is a move, not a duplication: delete
`check_persona_character_collisions()` and the folded display-name equality check
from the agent loader rather than leaving a subset check behind. The design
assigns this check to workspace load, and a forum-scoped copy can only produce a
second error message for a condition the workspace already rejected — an
unreachable path that still has to be maintained and tested.

Move the reusable comparison helpers to where Phase 3 can call them and keep the
error wording identical, so the existing collision tests change only in where
they expect the failure, not in what it says. Agent-loader tests that construct
a colliding persona/character pair move to `tests/session/unit_workspace.cpp`.

#### 2.5 Agent-definition tests

Update fixtures to create separate workspace definition and forum member trees.
Cover:

- definition prompt used when member prompt is absent;
- member prompt fully replacing, not concatenating with, the definition prompt;
- definition prompt includes allowed anywhere under the workspace
  `characters/` containment root;
- definition prompt escape outside `characters/` rejected;
- member prompt include inside the forum accepted and escape outside the forum
  rejected;
- `FORUM.md` remains forum-contained;
- three-layer runtime and `[prompt]` precedence visible in expansion;
- missing required definition files;
- optional member files absent;
- optional member paths that exist but are not regular files;
- multiple definitions retain input order;
- the four final prompt sections remain in the existing order.

### Phase 3: validate and resolve the new workspace layout

Primary files:

- `src/session/workspace.h`
- `src/session/workspace.cpp`
- `tests/session/unit_workspace.cpp`
- `tests/session/unit_concurrent_controllers.cpp`

#### 3.1 Add forum default-agent metadata

Extend `Forum` with a stable initial default character ID, for example:

```cpp
std::string default_agent_id;
```

Keep the member/character ID vector lexicographically ordered. Do not move the
default member to the front.

Extend forum `config.toml` parsing:

- `display_name` remains required.
- `default_agent` is optional and must be a non-empty string when present.
- Reject a `default_agent` that is not exactly one of that forum's member IDs.
- When absent, resolve it to the first lexicographic member ID.

#### 3.2 Validate workspace character definitions

During `Workspace` construction:

1. Require `forums/` as today.
2. Require workspace-level `characters/`.
3. Enumerate immediate character subdirectories in lexicographic ID order.
4. Validate every directory name with `validate_character_id()`.
5. Require and parse each `character.toml` through the definition metadata
   loader from Phase 1.
6. Require each definition `CHARACTER.md` to be a regular file. Do not expand it
   at workspace load.
7. Enforce workspace-wide case-insensitive display-name uniqueness using
   `fold_ascii()`.
8. Load the persona roster and enforce all persona/character ID and folded
   display-name collisions against the complete definition set.
9. Continue applying reserved participant-name validation to character display
   names.

Workspace construction is deliberately all-or-nothing: an invalid definition,
persona collision, or invalid forum membership prevents construction.

Place reusable collision logic in the agent/workspace boundary identified by
the design, but keep filesystem traversal in `Workspace`.

#### 3.3 Validate forum memberships

For every discovered forum during workspace validation:

1. Require `forums/<forum>/members/`.
2. Enumerate only its immediate subdirectories as member IDs; the
   `character_defaults.toml` file is not a member.
3. Validate member IDs with `validate_character_id()`.
4. Require at least one member.
5. Require an exactly matching workspace definition directory for every member.
6. Parse the forum config and resolve `default_agent` as described above.

A character definition that appears in no forum is valid.

#### 3.4 Update forum and definition loading paths

Change `Workspace::load_forum()` to enumerate `members/`, not `characters/`.
Update the internal definition-loading helper to build one
`AgentDefinitionSource` per ordered member:

```text
definition_directory = <workspace>/characters/<id>
member_directory     = <workspace>/forums/<forum>/members/<id>
forum defaults       = <workspace>/forums/<forum>/members/character_defaults.toml
```

Use this same path construction for `check_forum()`, `create_stored_session()`,
`create_session()`, and `open_session()`. There must not be separate path rules
for validation and real session opening.

Continue doing template expansion, effective agent construction, and provider
initialization at their current session/check boundaries, not in the workspace
constructor.

#### 3.5 Workspace tests

Rewrite the workspace fixture layout and cover:

- missing workspace `characters/`;
- empty or malformed definitions;
- missing definition `character.toml` or `CHARACTER.md`;
- global character ID and display-name validation;
- workspace-wide character display-name collisions even when the characters are
  members of different forums;
- persona/character ID and display-name collisions at workspace construction;
- missing `members/`;
- a forum with no members;
- empty member directories accepted;
- dangling member IDs rejected;
- one definition used by multiple forums;
- orphan definitions accepted;
- lexicographic member order;
- omitted `default_agent` resolves to the first member;
- explicit non-first `default_agent` accepted without reordering;
- unknown and malformed `default_agent` rejected;
- moved forum-default path;
- session/check loading uses the definition, defaults, and member override;
- malformed template includes are still detected at forum check/session open,
  not by metadata-only workspace validation;
- create and reopen behavior remains unchanged after the layout migration;
- concurrent controllers can use the same workspace-level definitions safely.

Update tests that currently expect missing personas or malformed forums to be
tolerated after `Workspace` construction. The new contract is all-or-nothing.

### Phase 4: pass the default ID separately to `SessionController`

Primary files:

- `src/session/session_controller.h`
- `src/session/session_controller.cpp`
- `tests/support/test_controller.h`
- `tests/session/unit_session_controller.cpp`
- call sites found by searching for `from_definitions(` and
  `from_backends_for_testing(`

#### 4.1 Production factory and constructor changes

Add a required initial default character ID to the production
`from_definitions()` path. Thread it into the private constructor separately from
the ordered definitions.

After `ForumCharacters` has been constructed:

- verify the supplied ID exists with `characters_.find()`;
- throw `std::invalid_argument` if it does not;
- assign `default_agent_id_` from the supplied ID;
- do not reorder `AgentRegistry`, runtime information, or `ForumCharacters`.

The two `default_agent` checks have different audiences and must not be confused:

- Workspace load owns the **user-facing** diagnostic. A `default_agent` naming a
  non-member is rejected there, in the workspace's error vocabulary, naming the
  forum's `config.toml` and the offending value. A user with a typo must always
  reach this message.
- The controller check is a **programming-error** guard on an already-validated
  value. It exists because `from_definitions()` is callable with a default ID and
  a roster that were not derived from the same forum. Its wording does not need
  to be user-friendly, and it must not become the path a bad workspace file takes.

Consequently, no workspace-level test asserts the controller's message, and no
controller test constructs its input through `Workspace`.

Update both workspace production call sites to pass `forum.default_agent_id`.

#### 4.2 Testing factories

Avoid mechanically changing every unrelated controller test. Extend the test
helpers with an optional explicit default ID while retaining first-character
fallback only inside test convenience wrappers. Production APIs must never infer
the default after this change.

When an explicit test default is supplied, validate it through the same
controller constructor path used in production.

Update direct production-factory calls in web runtime tests to provide a valid
default ID.

#### 4.3 Default-agent tests

Add tests proving:

- a non-first configured default becomes `default_agent_id()`;
- an ordinary unaddressed prompt targets that non-first default;
- `/agents` and `/info` mark the configured default but retain lexicographic
  listing order;
- `/mcast` without explicit targets retains lexicographic target order;
- `set_default_agent()` and `set_default_agent_by_id()` continue to work;
- an unknown initial default ID is rejected;
- existing first-character behavior remains when the workspace resolves an
  omitted `default_agent` to the first member.

No transcript, command syntax, web protocol, or session database schema change
is needed.

### Phase 5: migrate reusable test workspaces and all call sites

Primary files:

- `tests/support/test_workspace.h`
- `tests/support/test_workspace.cpp`
- all tests containing literal `forums/.../characters` paths
- integration tests that call `load_config()` or `load_agent_definitions()`

#### 5.1 Update `TestWorkspace`

Make the default fixture create:

```text
characters/guide/character.toml
characters/guide/CHARACTER.md
forums/lobby/members/character_defaults.toml
forums/lobby/members/guide/
```

Change `write_character_config()` to write the workspace definition. Add focused
helpers only where repeated tests need them, such as:

- adding a workspace character definition;
- adding a forum member;
- writing a member override;
- writing a member `CHARACTER.md` replacement;
- setting a forum default agent.

Keep helper names explicit about definition versus member paths.

#### 5.2 Update every literal old-layout fixture

Search with:

```sh
rg -n 'SYSTEM\.md|character_defaults|forums/[^ `"]*/characters|config\.name' \
  src tests workspace docs README.md Makefile CMakeLists.txt
```

Anchor `character_defaults` on nothing: the old pattern required a leading
`/characters/`, which missed the bare `characters/character_defaults.toml`
references in `src/README.md` and `src/session/README.md` and the mermaid node
labels in both module READMEs. Matching the filename alone over-matches into the
new location, which is the correct trade — every hit gets classified anyway.
`config.name` is in the pattern to catch readers missed by the Phase 1 split.
`docs/web-ui/` is inside `docs` and is included deliberately; check
`api-requirements.md` and `mockup.html` for baked-in layout assumptions.

Classify every hit as one of:

- production path logic that must change;
- a test fixture that must migrate;
- documentation that must be updated;
- historical text in the design documents that intentionally describes the old
  layout.

Do not blindly replace every `characters/`: workspace-level definition paths are
now correct uses of that directory name.

Update integration and protocol tests to the named three-layer config API and
new agent-definition source type.

### Phase 6: migrate the checked-in workspace

Primary tree: `workspace/`.

#### 6.1 Create workspace definitions

Create one definition directory for each unique character ID currently present:

- `characters/Cheburashka/`;
- `characters/Ismael/`;
- `characters/epictetus/`;
- `characters/markus_aurelius/`;
- `characters/seneca/`.

Move each character's identity configuration to its definition
`character.toml`. Consolidate the two identical forum-local `Ismael`
definitions into the one workspace definition.

Rename every moved `SYSTEM.md` to `CHARACTER.md`. Move each character-owned
profile file (`EPICTETUS.md`, `MARCUS_AURELIUS.md`, `SENECA.md`, and any similar
file) with its definition.

Move the existing `character-voice.md` to the workspace-level `characters/`
directory so the existing `../character-voice.md` includes remain within the
new containment root. Treat it as an ordinary template include target; do not
create a special file category or directory for it.

Do not invent tags for the checked-in characters. Omitted `tags` is valid and
keeps this migration behavior-neutral.

#### 6.2 Create memberships

For each forum, create these member directories:

```text
forums/hall/members/Ismael/

forums/lobby/members/Cheburashka/
forums/lobby/members/Ismael/

forums/stoics/members/epictetus/
forums/stoics/members/markus_aurelius/
forums/stoics/members/seneca/
```

Move each forum's `character_defaults.toml` to its `members/` directory.

The checked-in Git tree cannot preserve a truly empty directory. For the sample
workspace, use a comment-only `members/<id>/character.toml` such as:

```toml
# This member has no forum-local overrides.
```

This is a valid no-op overlay and keeps the membership directory present in a
checkout. It only works because a comment-only file parses to an empty table and
therefore contributes no keys; that is the behavior pinned by the comment-only
overlay test in section 1.6. Unit tests must still cover the actual
absent-file/empty-directory case using temporary directories.

Do not add `default_agent` to the checked-in forum configs unless preserving a
current forum's first lexicographic member requires it. With the current IDs,
omission preserves existing defaults.

#### 6.3 Remove the old layout

After all definitions, member directories, defaults, prompts, and profile files
exist at their new paths, remove each old forum-local `characters/` tree.

Verify that no session database or forum `FORUM.md` was moved or modified unless
its content contains an old path reference that must be updated.

### Phase 7: update web, console, TUI, and integration expectations

No new character endpoint or UI is part of this change. Only adapt existing
consumers to the new workspace and default-agent construction contracts.

Primary areas:

- `src/ui/web/` and `tests/ui/web/`;
- `src/ui/console/` and `tests/ui/console/`;
- `src/ui/tui/` and `tests/ui/tui/`;
- `src/apps/`;
- `tests/integration/`.

Required behavior:

- Forum/persona/session response shapes do not change.
- `SessionSnapshot.characters[]` does not change.
- The configured initial default is visible only through existing default-agent
  behavior and existing snapshot fields.
- Existing sessions continue to reopen with their stored transcript even when
  the current forum roster differs from historical transcript participants.
- Web tests that directly construct a production `SessionController` provide the
  resolved default ID.
- Tests that create malformed workspaces expect construction failure under the
  all-or-nothing contract.

### Phase 8: update documentation

Update documentation only after code and tests establish the exact final API.

Required files:

- `README.md`;
- `src/session/README.md`;
- `src/agents/README.md`;
- `src/apps/README.md` if it contains layout or startup assumptions;
- `docs/brief-characters.md` so its summary matches the final design;
- any other file found by the stale-path search in Phase 5.

Document:

- the workspace-level definition and forum membership layout;
- `CHARACTER.md` replacing `SYSTEM.md`;
- three-layer per-key configuration precedence;
- definition-only `display_name` and tags;
- `display_name` as the sole authorable identity key, with `id` and `name`
  rejected; do not document `Config::name` as authorable configuration;
- tag validation and ASCII folding;
- prompt replacement and containment rules;
- explicit `default_agent` syntax, validation, and lexicographic fallback;
- all-or-nothing workspace validation;
- orphan definitions and durable historical transcript behavior.

Do not document the deferred web Characters API or UI as implemented.

## 4. Verification sequence

Run verification in this order after implementation.

### 4.1 Static searches

```sh
rg -n 'SYSTEM\.md|character_defaults|forums/[^ `"]*/characters|config\.name' \
  src tests workspace docs README.md Makefile CMakeLists.txt
```

Review every remaining match. Matches that intentionally describe the old
layout in `characters-design.md` or migration history are allowed; executable
paths, fixtures, and current user instructions are not. Remaining
`character_defaults` hits must all point at `members/character_defaults.toml`,
and there must be no remaining `config.name` reader outside the reserved-field
declaration in `config.h`.

Also search construction APIs to ensure all production callers pass the default
ID:

```sh
rg -n 'from_definitions\(|load_agent_definitions\(|load_config\(' src tests
```

### 4.2 Build and unit tests

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure
```

If a focused test fails, run the owning GoogleTest suite/filter directly before
rerunning all of `ctest`.

### 4.3 Integration test

```sh
make itest
```

The checked-in workspace must load successfully through the same paths used by
the applications.

### 4.4 Optional sanitizer verification

Run when the normal suite passes and the environment supports it:

```sh
cmake --preset console-asan-ubsan
cmake --build --preset console-asan-ubsan
ctest --test-dir build/console-asan-ubsan --output-on-failure
```

This migration is mainly filesystem and value plumbing, but sanitizer coverage
is useful after changing constructor signatures and object initialization.

### 4.5 Final diff checks

```sh
git diff --check
git status --short
```

Inspect the complete diff. Confirm that unrelated user changes were not modified
or reverted.

## 5. Completion criteria

All of the following must be true:

- [ ] `Workspace` requires and validates workspace-level character definitions.
- [ ] Every forum member resolves to one definition, and every forum has at least
      one member.
- [ ] Orphan definitions are accepted.
- [ ] Character and persona identity collisions are checked workspace-wide.
- [ ] Definition metadata includes validated, definition-only tags using ASCII
      folding.
- [ ] Runtime configuration overlays definition, forum defaults, and member
      override per key in that order.
- [ ] `[prompt]` values use the same precedence.
- [ ] `display_name` and `tags` are rejected in both forum-local layers.
- [ ] `Config::display_name` carries the display name, every consumer reads it,
      and `Config::name` remains an unwritten reserved field.
- [ ] Persona/character collision checking has exactly one implementation, at
      workspace load.
- [ ] Definition `CHARACTER.md` and optional member `CHARACTER.md` resolve with
      the correct replacement and containment rules.
- [ ] The four effective prompt sections and their order are unchanged.
- [ ] Forum members and agent definitions remain lexicographically ordered.
- [ ] An explicit non-first `default_agent` is honored without reordering.
- [ ] The checked-in workspace uses only the new layout and starts successfully.
- [ ] Test fixtures use the new layout and cover optional/empty member files.
- [ ] No current production path reads forum-local `characters/` or `SYSTEM.md`.
- [ ] Existing session, transcript, command, and web protocol behavior remains
      unchanged outside initial-default selection.
- [ ] Current documentation matches the implemented layout and rules.
- [ ] The complete unit and integration suites pass.

## 6. Explicitly deferred work

Do not expand this implementation into these areas:

- workspace-level Characters web endpoints;
- forum member IDs in lobby payloads;
- Characters navigation or tag-grouping UI;
- character editing APIs;
- forum/workspace packaging or distribution;
- prompt snapshotting in session databases;
- a live-reload or filesystem-watching system;
- a tag registry;
- tag-driven forum membership;
- compatibility loading of the old forum-local character layout.

