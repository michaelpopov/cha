# Workspace-level characters: design

Status: agreed design, 2026-08-03. Supersedes the forum-local character layout.

This document is the implementation contract for moving characters from
forum-local definitions to workspace-level definitions with forum-local
overrides. It records every decision reached, the reasoning behind the
load-bearing ones, and the items deliberately left out of scope.

## 1. Motivation

Three properties of the current system point at this change.

1. **Personas are already workspace-level.** `personas/<id>/persona.toml` plus
   `PERSONA.md` is exactly the shape proposed for characters. Characters being
   forum-local is an asymmetry with no principled justification.
2. **The validation rules already assume a global character namespace.** A
   persona ID cannot equal a character ID, and a persona display name cannot
   equal a character display name case-insensitively. Personas are global, so
   enforcing those rules already requires checking every character in every
   forum against a workspace-wide namespace. The system has global identity
   semantics implemented over forum-local storage.
3. **The prompt is already assembled across this seam.** `load_definition_files()`
   in `src/agents/agent.cpp` expands `SYSTEM.md` (character-owned) and `FORUM.md`
   (forum-owned) as two independent templates and concatenates them. Promoting
   characters does not cut a new seam; it moves storage to where the seam
   already is.

The change also gives the browser UI a workspace-level Characters navigation
level, which currently has no home.

Accepted cost: editing a character definition now affects every forum that
includes it. This widens an existing property rather than introducing a new one,
since prompts are already reconstructed from files at session open rather than
snapshotted into the session database.

## 2. Directory layout

### 2.1 Character definitions

```
workspace/
  characters/
    epictetus/
      CHARACTER.md
      character.toml
    marcus_aurelius/
      CHARACTER.md
      character.toml
```

- The directory name is the stable character ID. Charset is unchanged from
  `validate_character_id()`: ASCII letters, digits, `_`, and `-`.
- `CHARACTER.md` is required. It replaces the former `SYSTEM.md` and holds the
  character's own prompt text. The rename makes the pair symmetric with
  `personas/<id>/{persona.toml, PERSONA.md}`.
- `character.toml` holds `display_name`, connection configuration, `tags`, and
  the `[prompt]` variable scope.

### 2.2 Forum membership

```
workspace/
  forums/
    stoics/
      config.toml
      FORUM.md
      members/
        character_defaults.toml
        epictetus/
          character.toml
        marcus_aurelius/
```

- The forum-local directory is named `members/`, not `characters/`. `characters/`
  means definitions; `members/` means membership plus per-forum overrides. The
  same word must not carry two meanings.
- A subdirectory of `members/` names a character by ID. Its presence is the
  membership record.
- `members/<id>/character.toml` is **optional**. Absent means the forum applies
  no per-character overrides. An empty member directory is valid; the workspace
  is not managed by version control, so empty directories are a sound membership
  marker.
- `members/<id>/CHARACTER.md` is **optional**. When present it shadows the
  definition's `CHARACTER.md` entirely.
- `members/character_defaults.toml` holds forum-wide defaults for all members. It
  moves from its current location at `forums/<forum>/characters/character_defaults.toml`.

### 2.3 Filenames are identical across layers

`character.toml` and `CHARACTER.md` keep the same names in both the definition
and the member directory. Same filename means same role, and the parent
directory carries the distinction. For configuration, the member file overlays
the lower-level files one key at a time. For prompt text, the member file
replaces the definition file entirely. Renaming the member-side file to
something like `overrides.toml` would hide the pairing.

## 3. Overlay model

The governing rules distinguish configuration from prompt text:

- Configuration files always overlay lower-level configuration files one key at
  a time. A member `character.toml` never replaces a lower-level configuration
  file wholesale.
- Prompt text does not merge. A member `CHARACTER.md`, when present, replaces
  the definition's `CHARACTER.md` entirely; when absent, the definition file is
  used.

### 3.1 Configuration precedence

`character.toml` values merge across three layers. Weakest first:

| Rank | File | Role |
| --- | --- | --- |
| 1 | `characters/<id>/character.toml` | Base definition |
| 2 | `forums/<f>/members/character_defaults.toml` | Forum-wide member defaults |
| 3 | `forums/<f>/members/<id>/character.toml` | Per-member override |

Higher rank wins on a per-key basis. The merge is the existing key-wise
`overlay()` in `src/agents/config.cpp`, applied to both the configuration patch
and the `[prompt]` variable table.

Consequence to be aware of: because forum defaults outrank the definition, a
forum that sets a value overrides every member's own declared value in that
room. Restoring one character's declared value there requires restating it in
that character's member override.

### 3.2 Prompt text

`CHARACTER.md` does not merge. The member copy, if present, replaces the
definition copy wholesale. This preserves "just copy it in" as a deliberate
authoring choice for a room that wants a divergent character, without making
divergence the default.

### 3.3 Template containment

Containment root follows the layer. A `CHARACTER.md` read from
`characters/<id>/` is contained to the workspace-level `characters/` directory;
one read from `forums/<f>/members/<id>/` is contained to the forum. `FORUM.md` is
unchanged and stays contained to the forum directory.

This replaces the current single `containment_root = forum_directory` in
`TemplateOptions`, which cannot reach a definition living outside the forum tree.

### 3.4 Reserved template variables

`character.id`, `character.display_name`, `forum.id`, and `forum.display_name`
remain available when expanding `CHARACTER.md`. They already exist; retaining
them is the do-nothing option and costs nothing. They are how a shared character
can reference the room it is currently loaded into without hardcoding it.

## 4. Identity

### 4.1 `display_name` is not overridable

`display_name` is declared only in `characters/<id>/character.toml`. It cannot
appear in `members/<id>/character.toml` or in `members/character_defaults.toml`.
Doing so is a **load error**, not a silently ignored value — a silently ignored
override reads as a broken program to whoever wrote it.

Four reasons, all load-bearing:

1. **It is persisted.** `display_name` and `addressed_to_name` are `NOT NULL`
   columns with non-empty `CHECK` constraints on every transcript row in every
   session database (`src/session/session_database.cpp`). A per-forum rename
   would put one character ID into the record under several names.
2. **It is in the prompt.** `forum_context()` lists the other agents by display
   name, and shared history encodes `speaker` and `addressed_to` by display name
   (`src/agents/agent.cpp`). The model's notion of who is present is built from
   display names.
3. **It is the `@mention` handle.** `resolve_handle()` matches against it. An
   override would make one character addressable by different names per room.
4. **Workspace uniqueness depends on it.** Uniqueness is only enforceable if the
   name is declared in exactly one place. With overrides, two different
   characters could each be renamed to the same name in two different rooms and
   no single check would catch it.

### 4.2 The general rule for future fields

> The override layer may change how a character **behaves or renders** in a room.
> It may not change **who the character is** or the workspace metadata by which
> the character is organized.

Identity is `id` and `display_name`. Workspace metadata is `tags`. Neither
identity nor workspace metadata is layerable. Behavioral and presentation
settings are layerable: model, thinking level, and presentation fields such as
font or color. None of those are written into a transcript, a prompt, or a
handle.

For presentation fields, keep the default at the definition layer so a character
looks consistent across rooms unless a forum deliberately re-themes it.

### 4.3 Identity source inversion

`load_config()` currently documents that "the parent directory name becomes its
ID and display_name must come from character_path." Under the overlay,
`character_path` is the member-level file, so this inverts: identity comes from
the **definition** layer. `build_config()` must take ID and display name from
`characters/<id>/`, require `display_name` there, and reject it in the member
layers.

### 4.4 Uniqueness

- Character IDs are unique workspace-wide (guaranteed by the directory layout).
- Character display names are unique workspace-wide, case-insensitively. There
  are no two Socrates characters in one workspace.
- A persona ID cannot equal a character ID.
- A persona display name cannot equal a character display name,
  case-insensitively.
- Reserved participant names continue to apply to character display names:
  `persona`, `system`, `error`, `human`, `assistant`, `agent`, `you`.

`resolve_handle()` still resolves against the forum's member subset, so
addressing ambiguity behavior within a forum is unchanged.

## 5. Tags

```toml
tags = ["stoics", "philosopher"]
```

- Declared only in the definition's `character.toml`. Optional; zero tags is
  valid. `tags` in `members/character_defaults.toml` or
  `members/<id>/character.toml` is a load error.
- Free-form. There is no workspace-level registry of permitted tags.
- Each tag is trimmed. Empty tags, control characters, and line breaks are
  rejected. Internal spaces are allowed.
- Matched using ASCII case folding for grouping; the authored casing is
  preserved for display.
- Duplicate tags within one character, compared using ASCII case folding, are a
  load error. This is consistent with how every other collision in the workspace
  is handled.

Tags exist for navigation and grouping in the Characters UI level. **Tags must
not drive forum membership.** A forum's roster is baked into every member's
prompt by `forum_context()`, so tag-driven membership would let an unrelated new
character silently rewrite an existing forum's prompts. Membership is always an
explicit set of member directories.

A character with no tags appears in an untagged grouping in the UI.

## 6. Default agent

- A forum may declare its default agent explicitly in `config.toml` by character
  ID:

  ```toml
  default_agent = "seneca"
  ```

  The value must name a member of that forum; otherwise workspace load fails.
- When not declared, the default is the lexicographically first member directory
  name — that is, by character ID, not by display name. This preserves current
  behavior.
- The member and agent-definition order remains lexicographic regardless of the
  configured default. The validated default character ID is passed separately
  to `SessionController`; selecting a default never reorders the forum roster,
  generated forum context, listings, or multicast targets.

## 7. Validation

### 7.1 At workspace load

Workspace load checks **referential integrity of the directory structure** plus
workspace-wide collisions.

- For every forum, every `members/<id>/` has a matching `characters/<id>/`. A
  dangling member reference is a load error. This is directory existence only;
  no file parsing is required.
- Every forum has at least one member.
- A forum's explicit `default_agent`, when present, names one of its members.
- Collision checks from §4.4, which require parsing each
  `characters/<id>/character.toml` and `personas/<id>/persona.toml` exactly once.
- `characters/` is required at workspace load, in the same way `forums/` is
  required today.

Cost is bounded by the number of characters and personas, not by characters ×
forums. The promotion is what makes this cheap: under the forum-local layout,
workspace-wide display-name uniqueness would have required parsing every
character file in every forum.

### 7.2 At session open

Template expansion, agent definition construction, persona roster assembly,
forum context generation, and model connection setup all stay at session open,
where they are today. Workspace load must not be allowed to collapse into
constructing every agent in every forum at startup.

The split: **workspace load parses and checks; session open constructs and
expands.**

## 8. Behavior that does not change

- The effective system prompt keeps its four sections in order: expanded
  character prompt, expanded `FORUM.md`, the complete static persona roster, and
  generated forum context. Only the source path of the first section moves.
- `CharacterInfo` remains `{id, name}`. `ForumCharacters` is still constructed
  from a `vector<CharacterInfo>`.
- Everything downstream of that value — `resolve_handle()`, addressing,
  `SessionSnapshot.characters[]`, the transcript, the web protocol, the
  `/agents`, `/info`, `/@Name`, and `/mcast` commands — keeps its existing
  behavior. The only additional session input is the validated initial default
  character ID; character ordering and the values themselves do not change.
- Orphan characters, defined in `characters/` but a member of no forum, are
  legal. Authoring a character before placing it in a room is a normal workflow,
  and the Characters UI level browses all definitions.
- Dropping a character from a forum's `members/` does not affect existing
  sessions. Transcript rows carry the display name as a durable record of who
  authored a message; those rows stay valid and unchanged.

## 9. Code touchpoints

| Area | File | Change |
| --- | --- | --- |
| Definition loading | `src/agents/agent.cpp` | Resolve `CHARACTER.md` and `character.toml` across the definition and member layers; containment root follows the layer |
| Config layering | `src/agents/config.cpp`, `config.h` | Three layers instead of two; `build_config()` identity source inversion; reject `display_name` and `tags` in member layers |
| Collision checks | `src/agents/agent.cpp` | `check_persona_character_collisions()` and the display-name equality check move from per-forum-at-open to workspace-wide-at-load |
| Workspace | `src/session/workspace.cpp` | Load `characters/`; `members/` paths; `character_defaults.toml` new location; referential integrity and `default_agent` validation at load |
| Forum characters | `src/session/forum_characters.*` | Unchanged |
| Initial default agent | `src/session/session_controller.*` | Accept the validated default character ID separately from the lexicographically ordered character roster |

## 10. Out of scope

Deliberately excluded from this design:

- **Web and lobby API changes.** The Characters UI level and the composer's
  character picker will need a workspace-level characters endpoint carrying tags,
  and member IDs on the forum listing. Deferred to a later stage.
- **The Characters UI screen.** Tag-based grouping and navigation are undesigned.
- **Distribution and bundling.** Not addressed here.
- **Snapshotting effective prompts into the session database.** Noted as a
  sharper question under shared characters, not solved.
