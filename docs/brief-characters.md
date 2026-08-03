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
          character.toml   <- optional, overrides only
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

It also gives the browser UI a Characters level, which had nowhere to live.

## How overrides work

One rule: **a forum's member directory shadows the workspace character
directory, file by file.** Present locally means used; absent means the
workspace file.

Config merges across three layers, weakest to strongest:

1. `characters/<id>/character.toml` — the definition
2. `forums/<f>/members/character_defaults.toml` — forum-wide defaults
3. `forums/<f>/members/<id>/character.toml` — per-member override

`CHARACTER.md` does not merge. A member copy replaces the definition wholesale,
so "copy it in and edit it" stays available when a room genuinely wants a
divergent character.

Note the consequence: forum defaults outrank the definition, so a forum that
sets a value overrides every member's own declared value in that room.

## What cannot be overridden

`display_name`. It is declared once, in the definition, and putting it in an
override is a load error.

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

Free-form strings for grouping characters in the UI. Optional, no registry,
matched case-insensitively. Tags are for navigation only — they never determine
which forum a character belongs to. Membership is always an explicit member
directory.

## Validation

**At workspace load:** referential integrity of the directory structure — every
`members/<id>/` has a matching `characters/<id>/` — plus workspace-wide name
collisions. `characters/` is now required, like `forums/`.

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

The web API changes, the Characters screen itself, and how tag-based grouping
should look.
