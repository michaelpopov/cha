# Characters move to the workspace level

Short version. Full detail in [characters-design.md](characters-design.md).

## What changes

Characters stop being forum-local. They become workspace-level definitions that
forums include by reference, the same way personas already work.

```
workspace/
  characters/              <- definitions live here
    epictetus/
      CHARACTER.md         <- was SYSTEM.md
      character.toml       <- display_name, model config, tags
  forums/
    stoics/
      config.toml
      FORUM.md
      members/             <- was characters/
        character_defaults.toml
        epictetus/
          character.toml   <- optional, per-key overrides only
          CHARACTER.md     <- optional full prompt replacement
```

## Why

Three things already pointed this way:

- Personas are already workspace-level. Characters being forum-local was an
  asymmetry with no real justification.
- The rule "a persona ID cannot equal a character ID" already required checking
  characters against a workspace-wide namespace. The identity semantics were
  already global; only the storage wasn't.
- The prompt already concatenates a character-owned document and a forum-owned
  one. The seam existed; this moves the files to match it.

It gives a future browser Characters level a workspace home.

## How overrides work

One rule per file kind: a member `character.toml` overlays the definition key by
key, while a member `CHARACTER.md` shadows the definition file wholesale.

Config merges across three layers, weakest to strongest:

1. `characters/<id>/character.toml` — the definition
2. `forums/<f>/members/character_defaults.toml` — forum-wide defaults
3. `forums/<f>/members/<id>/character.toml` — per-member override

`CHARACTER.md` does not merge. A member copy replaces the definition wholesale,
so "copy it in and edit it" stays available when a room genuinely wants a
divergent character.

Includes from a definition prompt stay within workspace `characters/`; includes
from a member prompt and `FORUM.md` stay within that forum.

Note the consequence: forum defaults outrank the definition, so a forum that
sets a value overrides every member's own declared value in that room.

## What cannot be overridden

`display_name` and `tags`. They are declared only in the definition, and putting
either in an override is a load error. `display_name` is the sole authorable
identity key; `id` and `name` are rejected in every configuration layer.

It carries too much weight elsewhere: it is written into every transcript row in
every session database, it appears in the prompts the model reads, it is the
`@mention` handle, and workspace-wide name uniqueness is only checkable if the
name is declared in one place.

The general rule for future settings:

> An override may change how a character **behaves or renders** in a room. It may
> not change **who the character is.**

So thinking level, model, font, and color are all fair game. Identity is not.

## Names are unique workspace-wide

There are no two Socrates characters in one workspace. Character IDs and display
names are unique across the whole workspace, and they still cannot collide with
persona IDs or persona display names.

## Tags

```toml
tags = ["stoics", "philosopher"]
```

Free-form strings for future grouping. Optional, no registry, matched with ASCII
case folding while preserving authored casing. Tags are trimmed; empty tags,
controls, line breaks, and case-folded duplicates within one character are
errors. They are for navigation only — they never determine which forum a
character belongs to. Membership is always an explicit member directory.

## Validation

**At workspace load:** referential integrity of the directory structure — every
`members/<id>/` has a matching `characters/<id>/` — plus workspace-wide name
collisions. `characters/` is now required, like `forums/`.

`default_agent = "character-id"` in a forum's `config.toml` must name one of its
members. When omitted, the lexicographically first member ID is used; an
explicit default never reorders the roster. Workspace validation is
all-or-nothing, while definitions with no forum membership are valid.

**At session open:** everything expensive, unchanged. Template expansion, agent
construction, model connections.

Load stays cheap precisely because of the promotion: there is now exactly one
definition per character to parse, instead of one per character per forum.

## What doesn't change

The four prompt sections and their order. `CharacterInfo` and `ForumCharacters`.
Addressing, `@mentions`, `/mcast`, `/@Name`, the transcript, the web protocol.
If the loader produces the same values, the session and agent layers never
notice the difference.

Also fine: characters that belong to no forum, and characters dropped from a
forum whose old messages stay in past sessions exactly as recorded.

## Not covered yet

The web Characters API, the Characters screen itself, and how tag-based
grouping should look.
