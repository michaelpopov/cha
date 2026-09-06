# CHA workspace maintainer guide

This guide is the operating manual for changing CHA's personas, characters,
forums, providers, styles, and built-in Assistant. It is written for a Codex
session that has been asked to perform changes such as:

- add or update a persona;
- add or update a character;
- add a forum;
- add or remove a character from a forum;
- change a forum's default character or persona;
- add a model provider and assign characters to it;
- add a visual style and assign it to characters.

The complete example currently available on this machine is
`~/var/modify/`. The smaller repository-owned example shipped with the Linux
package is `packaging/linux/import-seed/`.

## 1. Operating rules for an automated maintainer

Follow these rules before changing anything:

1. Read the repository's `AGENTS.md` and inspect `git status` before editing.
   Preserve unrelated changes.
2. Establish which directory the user wants changed. Do not assume that an
   exported directory is live configuration.
3. Unless the user says otherwise, a request to add or change workspace
   content means: edit the requested configuration directory and validate it.
   It does **not** authorize importing it into the production database.
4. Do not edit the SQLite database directly. Do not edit a temporary
   `cha-runtime-*` materialization.
5. Do not modify `packaging/linux/import-seed/` unless the user explicitly
   asks to change the product's initial packaged workspace.
6. Use the smallest set of files that expresses the requested change. Do not
   add schemas, generators, registries, or abstractions for ordinary content
   maintenance.
7. Validate the complete workspace after every change. All configured forums
   are loaded together, so an error in an otherwise unused forum can prevent
   CHA from starting.
8. Import into the real database only when explicitly requested, after
   identifying the exact configuration directory, stopping CHA, and reviewing the
   destructive effects described in [Import and export](#12-import-and-export).

For commands concerning the example on this machine, use this exact root:

```text
/home/mpopov/var/modify
```

Do not spell it through an unresolved `$HOME` in destructive or import
commands. Resolve the path first.

## 2. Configuration sources and runtime state

CHA configuration can appear in several places. They have different roles.

| Location | Role | Edit directly? |
| --- | --- | --- |
| An exported directory such as `~/var/modify/` | Human-editable workspace bundle | Yes |
| `packaging/linux/import-seed/` | Initial configuration shipped in a Linux package | Only when explicitly requested |
| The SQLite file named by the active vault's `data` field | Authoritative runtime configuration and sessions | Never by hand |
| A `cha-runtime-*` directory under the system temporary directory | Private materialization of committed SQLite rows | Never |
| `<config-directory>/openai-auth.json` | OpenAI subscription OAuth credentials | Only through Settings → OpenAI |
| `.env` in the configuration directory | Runtime API-key environment values | Carefully, as a secret |

Normal runtime reads configuration from SQLite. It does not continue reading
the directory that was imported. Therefore editing `~/var/modify/` alone does
not alter a running application. The normal lifecycle is:

```text
SQLite database -> export directory -> edit -> validate -> import -> SQLite database
```

An old export can also be stale. CHA can update a character's provider, style,
reasoning effort, and web-search setting, and a forum's default character and
persona, while it is running. Those narrow edits are committed to SQLite. If a
request concerns the current live configuration, begin from a fresh export or
explicitly reconcile the existing bundle with one; otherwise a later import
can overwrite changes made through the UI. Do not replace a user's existing
edit directory merely to refresh it without first preserving or reviewing its
contents.

The external application configuration is a directory, not part of a workspace
import. `app.toml` selects the startup vault and holds web and logging
settings. Each other `.toml` file is one vault:

```toml
# app.toml
vault = "Personal"

[web]
host = "127.0.0.1"
port = 8086

[logging]
file = "/absolute/path/cha.log"
level = "info"
```

```toml
# personal.toml
vault_name = "Personal"
data = "/absolute/path/workspace.sqlite3"
mirror = "/optional/session/mirror"
modify = "/optional/editable/export/directory"
```

`mirror` and `modify` are optional. Relative `data`, `mirror`, `modify`, and
`logging.file` values are resolved relative to the configuration directory.
That directory must be outside a directory passed to `--import`. There is no
automatic migration from a single `cha.toml`; create the directory, split
selection/web/logging into `app.toml` and data paths into a vault file, adjust
relative paths, and move `.env` and `openai-auth.json` into the directory.

## 3. Workspace directory map

A representative workspace looks like this:

```text
workspace/
├── .env                                      # optional, validation only
├── personas/
│   └── michael/
│       ├── persona.toml                      # required persona metadata
│       └── PERSONA.md                        # optional persona prompt
├── characters/
│   ├── character-voice.md                    # optional shared prompt fragment
│   └── historical/
│       └── feynman/
│           ├── character.toml                # required character settings
│           ├── CHARACTER.md                  # required character prompt
│           └── FEYNMAN.md                    # optional included prompt fragment
├── forums/
│   └── braintrust/
│       ├── config.toml                       # required forum metadata/defaults
│       ├── FORUM.md                          # required forum prompt and description
│       └── members/
│           ├── character_defaults.toml       # optional shared prompt variables
│           ├── feynman/
│           │   ├── character.toml            # membership marker/variables
│           │   └── CHARACTER.md              # optional forum-specific prompt
│           └── muller/
│               └── character.toml
└── system/
    ├── assistant/
    │   └── character.toml                    # required built-in Assistant settings
    ├── providers/
    │   ├── sol/config.toml                   # API-key provider
    │   └── chatgpt/config.toml               # OAuth subscription provider
    └── styles/
        └── serif-bold/config.toml
```

The loader requires `personas/`, `characters/`, `forums/`, and
`system/providers/` directories. `system/styles/` is optional, but every style
referenced by a character must exist. `system/assistant/character.toml` and at
least one usable provider are effectively required because the built-in
Assistant must select a provider.

Only regular `.toml` and `.md` files are stored during import. Symlinks whose
names would otherwise be imported are rejected; symlinked directories are not
followed. Other extensions are ignored. Consequently, a prompt include must
also be a `.md` or `.toml` file if it must survive import. For example,
`$$(notes.md)` can work, while `$$(notes.txt)` disappears from the imported
workspace and makes validation fail.

## 4. IDs, directory names, and public names

An entity's ID comes from the name of the directory containing its definition,
not from a TOML field.

For example:

```text
characters/historical/feynman/character.toml -> character ID "feynman"
personas/private/michael/persona.toml         -> persona ID "michael"
forums/braintrust/config.toml                 -> forum ID "braintrust"
system/providers/openrouter/config.toml       -> provider ID "openrouter"
system/styles/serif-bold/config.toml           -> style ID "serif-bold"
```

Characters and personas may be nested under grouping directories. The grouping
path is organizational only and is not a namespace. Therefore
`characters/historical/guide/` and `characters/fictional/guide/` conflict: both
define character ID `guide`. Forums, providers, styles, and forum member
directories are direct children and are not recursively grouped.

Use lowercase ASCII `snake_case` or simple hyphenated IDs unless an existing ID
must be retained. That convention is narrower than the loader's rules and
avoids case and URL surprises.

The enforced rules are:

- Character IDs: ASCII letters, digits, `_`, and `-`; nonempty. `-`, `guest`,
  `assistant`, `entrance`, and `builtin-welcome` are reserved.
- Persona IDs: first character must be an ASCII letter or `_`; remaining
  characters may be ASCII letters, digits, or `_`. Participant words such as
  `persona`, `system`, `error`, `human`, `assistant`, `agent`, `character`,
  `you`, and `guest` are reserved case-insensitively.
- Forum IDs: URL-safe ASCII letters, digits, `-`, `.`, `_`, and `~`; nonempty.
  The same built-in IDs listed for characters are reserved.
- Provider and style IDs: one filesystem path component. For maintainability,
  still use the conservative lowercase convention above.

`display_name` is the public label. It must be nonempty valid UTF-8, must not
contain line breaks or control characters, and must not start or end with
whitespace. Character and persona display names also cannot start with `@` or
`/`. Character and persona display names are globally unique when compared
case-insensitively, including the built-in names `Assistant` and `Guest`.
Forum display names are unique case-insensitively and cannot be `Entrance`.

An optional `description` is one line with the same UTF-8, control-character,
and surrounding-whitespace restrictions.

## 5. Personas

A persona represents the human participant: who is speaking to the characters.
It is not a model-backed character and is not added to a forum's `members/`
directory.

### Files

```text
personas/<persona-id>/persona.toml   required
personas/<persona-id>/PERSONA.md     optional
```

Both character and persona definitions may be nested under grouping folders.
A directory containing either `persona.toml` or `PERSONA.md` is treated as a
persona definition directory. Do not leave a stray `PERSONA.md` without its
`persona.toml`.

`persona.toml` accepts exactly these fields:

```toml
display_name = "Michael"                         # required
description = "A programmer living in Redmond." # optional, one line
```

Unknown fields are rejected. `PERSONA.md`, when present, describes the user to
the forum's characters. It can contain substantial first-person context and
communication preferences. If it is absent, the persona still exists but has
no prompt body.

Every forum chooses its active starting persona with `default_persona` in the
forum's `config.toml`. If omitted, CHA uses the built-in `guest` persona. A user
can change a forum's saved default persona at runtime; that narrow online edit
rewrites the forum configuration stored in SQLite.

### Add a persona

1. Choose a unique persona ID and display name.
2. Create `personas/<id>/persona.toml`.
3. Add `PERSONA.md` if the characters need context about this speaker.
4. Set `default_persona = "<id>"` in any forum that should start as this
   persona.
5. Validate the complete workspace.

### Remove or rename a persona

Before removing or renaming it, search all forum configs:

```sh
rg -n 'default_persona' /absolute/workspace/forums
```

Update every reference first. Renaming means renaming the definition directory
and every `default_persona` reference. It does not require membership changes.

## 6. Characters

A character is a model-backed participant. Its definition chooses one provider,
an optional visual style, optional per-character model overrides, metadata, and
the character prompt.

### Files

```text
characters/<optional-groups>/<character-id>/character.toml  required
characters/<optional-groups>/<character-id>/CHARACTER.md    required
characters/<optional-groups>/<character-id>/*.md            optional fragments
characters/character-voice.md                               optional shared fragment
```

A directory containing either `character.toml` or `CHARACTER.md` is treated as
a character definition directory, and then both files are required. Avoid
placing either specially named file in a grouping directory.

`character.toml` accepts these fields:

```toml
display_name = "Richard Feynman"  # required
description = "Physicist."        # optional, one line
provider = "sol"                  # required provider ID
style = "serif"                   # optional style ID
reasoning_effort = "high"         # optional: low | medium | high | xhigh
web_search = "auto"               # optional: off | auto | required
tags = ["science", "historical"] # optional; unique case-insensitively

[prompt]                           # optional template variables
register = "plainspoken"
```

Unknown fields are rejected. `provider` and `style` are case-sensitive ID
references. Character `reasoning_effort` and `web_search` override the selected
provider's defaults. Omitting either inherits the provider value. Tags must be
nonempty strings after trimming, contain no control characters, and be unique
case-insensitively.

`CHARACTER.md` is the actual character prompt. A simple prompt may be entirely
self-contained. A maintainable larger definition often uses a small wrapper:

```markdown
$$(../../character-voice.md)

<character_profile>
$$(FEYNMAN.md)
</character_profile>
```

The include path is relative to the file containing the include. The example
uses `../../` because the character is under
`characters/historical/feynman/`. A direct child such as
`characters/feynman/` would use `../character-voice.md`.

The `<character_profile>...</character_profile>` markers are optional, but they
have a UI effect: when both are present, the Characters page publishes only
the expanded text between them as the character's description. Without a
complete marker pair, it publishes the entire expanded `CHARACTER.md`.

### Add a character

1. Choose a globally unique character ID and participant display name.
2. Choose an existing provider and, optionally, style.
3. Create the definition directory, `character.toml`, and `CHARACTER.md`.
4. Add any `.md` fragments referenced by `CHARACTER.md`.
5. Add the character to one or more forums by creating member directories as
   described below. A defined character is allowed to belong to no forum.
6. Validate the complete workspace.

### Rename a character ID

An ID rename is a coordinated change:

1. Rename the character definition directory.
2. In every forum containing the character, rename
   `forums/<forum>/members/<old-id>/` to `<new-id>/`.
3. Replace every matching `default_character` value.
4. Search all TOML and Markdown for application-specific uses of the old ID.
5. Validate.

Changing only `display_name` is much smaller and does not require reference
changes.

## 7. Forums and membership

A forum is a named conversation environment. It has at least one character
member, one starting character, one starting persona, and a prompt that is
shared by every member.

### Required layout

```text
forums/<forum-id>/config.toml
forums/<forum-id>/FORUM.md
forums/<forum-id>/members/<character-id>/character.toml
```

Forum definitions must be direct children of `forums/`. Each direct child of
`members/` is interpreted as a character ID and must resolve to a global
character definition. A forum must have at least one member.

`config.toml` accepts:

```toml
display_name = "Brain Trust"       # required
description = "Brainstorming team" # optional, one line
default_character = "muller"       # optional, must be a member
default_persona = "employee"       # optional, must exist
```

If `default_character` is omitted, the lexicographically first member ID is
used. If `default_persona` is omitted, built-in `guest` is used. The legacy key
`default_agent` is accepted as an alias for `default_character`, but new work
must use `default_character`; defining both is an error.

`FORUM.md` has two roles:

1. its expanded text is appended to every member's character prompt;
2. its raw, unexpanded source is displayed as the forum's description in the
   UI/API.

Write it so the raw Markdown remains understandable even if it contains
template expressions such as `$${character.display_name}`.

### Add a forum

1. Choose a unique URL-safe forum ID and display name.
2. Create `config.toml`, `FORUM.md`, and `members/`.
3. Add at least one member directory. Use a comment-only marker file when no
   member-specific variables are needed:

   ```toml
   # Membership marker; this character uses its global prompt.
   ```

4. Set `default_character` explicitly to one of those member IDs. Explicit is
   clearer than depending on lexicographic order.
5. Set `default_persona` when the forum should not start as Guest.
6. Validate.

### Add a character to a forum

For character `feynman` and forum `braintrust`, create:

```text
forums/braintrust/members/feynman/character.toml
```

The file may be empty or comment-only. Do not copy the global character
definition into the forum. Optionally add `[prompt]` values or a forum-specific
`CHARACTER.md`, as described in [Prompt templates and overrides](#10-prompt-templates-and-overrides).

If the new character should be the starting character, also change:

```toml
default_character = "feynman"
```

### Remove a character from a forum

1. If it is the forum's `default_character`, choose another existing member
   first.
2. Remove only `forums/<forum>/members/<character-id>/`.
3. Keep the global character definition unless the user also asked to remove
   the character everywhere.
4. Do not leave the forum with zero members.
5. Validate.

### Remove or rename a forum

Forum IDs are part of persistent session identity. On import, a missing old
forum ID is treated as removal and its stored sessions are deleted. Renaming a
forum directory is therefore a delete-and-add operation from the session
database's perspective. Never remove or rename a forum in a production import
without explicitly warning the user and confirming that session loss is
intended or backed up.

Changing only the forum's `display_name` preserves its ID and sessions.

## 8. Providers

A provider is a reusable connection plus model configuration. It lives only at:

```text
system/providers/<provider-id>/config.toml
```

Each character selects exactly one provider with `provider = "<id>"` in the
global character definition. The built-in Assistant does the same in
`system/assistant/character.toml`. Provider fields in forum defaults or member
overrides do not provide inheritance and should not be used.

Provider labels shown by the UI are derived from IDs: `open_router` and
`open-router` become `Open router`.

### Provider fields

| Field | Required/default | Meaning and constraints |
| --- | --- | --- |
| `host` | required | Host name without scheme or path |
| `port` | effectively required | Integer `1..65535`; omitted becomes invalid `0` |
| `base_path` | `""` | Optional leading path; must start with `/`, not end with `/`, and contain no query, fragment, or whitespace |
| `https` | `false` | Use HTTPS when true |
| `mode` | `"test"` | `net` for real HTTP; `test` is only for deterministic tests |
| `model` | required | Provider model identifier |
| `stream` | `true` | Request streaming when true |
| `temperature` | omitted | Number from `0` through `2` |
| `max_tokens` | omitted | Positive output-token limit |
| `timeout_s` | `600` | Positive overall request timeout |
| `idle_timeout_s` | `60` | Positive timeout after response bytes stop arriving |
| `api_key_env` | `""` | Environment variable containing an API key |
| `reasoning_effort` | `""` | Provider default forwarded to the backend |
| `reasoning_format` | `"auto"` | `auto`, `none`, `reasoning_content`, or `reasoning` |
| `api` | `"responses"` | `responses` or `chat_completions` |
| `auth` | no special auth | Only special value is `openai_subscription` |
| `web_search` | `"off"` | `off`, `auto`, or `required` |
| `cache_retention` | `"short"` | `off`, `short`, or `long` |

Unknown fields are rejected. A malformed unused provider is logged and omitted;
if any character selects it, workspace loading fails with the provider error.
Do not leave knowingly malformed unused provider directories behind.

For ordinary providers, CHA constructs endpoints as follows:

```text
responses:        {scheme}://{host}:{port}{base_path}/v1/responses
chat_completions: {scheme}://{host}:{port}{base_path}/v1/chat/completions
```

### API-key provider example

Direct OpenAI Responses API:

```toml
host = "api.openai.com"
port = 443
https = true
mode = "net"
model = "gpt-5.6-sol"
reasoning_effort = "low"
stream = true
api = "responses"
web_search = "off"
api_key_env = "OPENAI_API_KEY"
```

OpenAI-compatible Chat Completions endpoint under a base path:

```toml
host = "openrouter.ai"
port = 443
base_path = "/api"
https = true
mode = "net"
model = "qwen/qwen3.8-max"
reasoning_effort = "low"
stream = true
api = "chat_completions"
web_search = "off"
api_key_env = "OPEN_ROUTER_API_KEY"
```

The environment variable's value is not placed in `config.toml`. A missing key
does not prevent workspace loading, but an actual request that needs it will
fail. Runtime loads a `.env` file from the configuration directory, without
overriding variables already inherited by the process.

An optional `.env` at the root of an import source is parsed and overlaid only
while the import is validated. It is **not** stored in SQLite and is not
exported. Put runtime API keys in the service environment or in `.env` in the
configuration directory. Protect that file as a secret.

### OpenAI subscription OAuth provider

OAuth authentication is a different connection type, selected by
`auth = "openai_subscription"`:

```toml
auth = "openai_subscription"
host = "chatgpt.com"
port = 443
https = true
base_path = "/backend-api/codex"
mode = "net"
api = "responses"
model = "gpt-5.6-terra"
stream = true
web_search = "off"
cache_retention = "off"
```

For this auth type, all of the following are mandatory invariants:

- `host = "chatgpt.com"`;
- `port = 443` and `https = true`;
- `base_path = "/backend-api/codex"`;
- `mode = "net"` and `api = "responses"`;
- `stream = true`;
- no `api_key_env`;
- no `temperature` or `max_tokens`;
- `web_search = "off"`;
- `cache_retention = "off"` must be explicit because its ordinary default is
  `short`.

The endpoint is fixed to
`https://chatgpt.com/backend-api/codex/responses`. Character-level web-search
overrides other than `off` are also invalid for this provider.

The provider TOML declares that OAuth must be used; it does not contain or
create a login. The user connects in the application's Settings page under
OpenAI. CHA stores the resulting process-wide credentials in:

```text
<config-directory>/openai-auth.json
```

For example, `/srv/cha/cha-config` uses
`/srv/cha/cha-config/openai-auth.json`. All
`openai_subscription` providers in that CHA process share this one connected
ChatGPT account. The credential file is excluded from workspace import/export
and must never be copied into a configuration bundle or committed. Disconnect
through the UI rather than editing the JSON. If no account is connected,
characters using this provider fail with `Sign in to ChatGPT before using this
provider.` A rejected or unusable login requires reconnecting in Settings.

### Web search compatibility

`web_search` other than `off` is accepted for:

- the Responses API, except an `openai_subscription` provider;
- OpenRouter Chat Completions (`host` equal to `openrouter.ai`, ignoring case
  and one trailing dot).

It is rejected for other Chat Completions hosts. This check applies to both a
provider default and a character override.

### Add a provider

1. Choose a provider ID and create
   `system/providers/<id>/config.toml`.
2. Use the smallest suitable example above and add only needed optional fields.
3. For API-key auth, arrange the runtime environment separately. Never put the
   key in TOML.
4. Change the intended global character definitions and/or Assistant to
   `provider = "<id>"`.
5. Validate the workspace. Validation does not make a network request.
6. If connectivity testing was requested, run CHA and send a real prompt with
   one assigned character. For OAuth, connect in Settings first.

Removing a provider requires first changing every character and the Assistant
that references it. Search with:

```sh
rg -n 'provider\s*=' /absolute/workspace/characters \
  /absolute/workspace/system/assistant
```

## 9. Styles and the built-in Assistant

### Styles

Styles live at `system/styles/<style-id>/config.toml` and accept only:

```toml
font = "serif"        # sans | serif | mono; default sans
style = "italic"      # normal | italic; default normal
weight = "bold"       # light | normal | medium | semibold | bold
size = "large"        # small | normal | large
text_color = "accent" # normal | muted | accent
```

Every field is optional. Omitted values use the defaults shown in the comments
or implied above. Character definitions select a style by ID. Without `style`,
a character uses the interface defaults. An invalid style config is logged and
omitted; a character reference to that style then makes loading fail.

To add a style, create its config, assign it in one or more global
`character.toml` files, and validate. Forum member files cannot override style.

### Built-in Assistant

`system/assistant/character.toml` configures the built-in character used in the
Entrance forum. A minimal file is:

```toml
display_name = "Assistant"
provider = "sol"
```

It accepts the same definition fields as a global character, including style,
reasoning, web search, tags, and description. Its prompt is compiled into CHA;
there is no `system/assistant/CHARACTER.md`. The Assistant ID, name, and
Entrance membership are built in. Do not create user definitions with the
reserved `assistant` or `entrance` IDs.

## 10. Prompt templates and overrides

CHA expands two macro forms in character and forum prompts:

```text
$$(relative/file.md)   include another file
$${variable.name}      substitute a variable
```

Use `$$$(...)` when literal output must contain `$$(...)`. Includes are
relative to the including file. Absolute includes, missing files, directories,
cycles, and paths that escape the allowed containment root are rejected.
Unknown variables and malformed macros are rejected. Expanded output has
resource limits, including a 16-level include depth, 256 includes, and 1 MiB
of output.

The reserved variables are:

```text
character.id
character.display_name
forum.id
forum.display_name
```

They cannot be overridden. When CHA expands a global character only for its
catalog description, forum values are empty. When it builds a forum member's
effective prompt, all four contain their real values.

### Prompt variable precedence

For a character participating in a forum, `[prompt]` variables are overlaid in
this order; later values win:

1. global `characters/**/<id>/character.toml`;
2. `forums/<forum>/members/character_defaults.toml`;
3. `forums/<forum>/members/<id>/character.toml`.

Example:

```toml
# global character.toml
[prompt]
voice = "definition"
```

```toml
# members/character_defaults.toml
[prompt]
voice = "forum default"
relationship = "colleague"
```

```toml
# members/feynman/character.toml
[prompt]
voice = "forum-specific"
```

The effective values for Feynman are `voice = "forum-specific"` and
`relationship = "colleague"`.

Forum `character_defaults.toml` and member `character.toml` accept only the
legacy-compatible `provider` field and a `[prompt]` table, but provider values
there have no runtime effect. Do not write provider overrides there; set the
provider in the global character definition.

Values in `[prompt]` must be scalar TOML values. Variable names must follow the
template variable grammar; ordinary simple names and dotted names are safest.
An auxiliary directory-local `config.toml` may also contain `[prompt]` values
used while expanding Markdown in that directory. This is an advanced local
scope: it can override the initial values inside that file/include subtree but
does not leak back to its caller. Prefer the three explicit layers above unless
local include reuse actually requires this behavior.

### Forum-specific character prompt override

If this file exists:

```text
forums/<forum>/members/<character-id>/CHARACTER.md
```

it completely replaces the character's global `CHARACTER.md` for that forum.
It does not append to it automatically. Add an explicit include if reuse is
desired. Includes from a forum-specific override must remain within that forum
directory; they cannot escape into `characters/`.

### Final system-prompt composition

For each forum member, CHA constructs the system prompt in this order:

1. expanded global `CHARACTER.md`, or the forum member's replacement
   `CHARACTER.md`;
2. expanded forum `FORUM.md`;
3. a generated Participants section containing the selected default persona's
   `PERSONA.md` (or the built-in Guest text);
4. generated forum context naming the current and other characters and
   explaining shared conversation history.

This explains where a change belongs:

- identity and voice shared everywhere: global character prompt;
- behavior shared by every character in one forum: `FORUM.md`;
- information about the human participant: `PERSONA.md`;
- shared forum variables: `character_defaults.toml`;
- one character's behavior or variables in one forum: member override files.

## 11. Command recipes

These recipes assume `WORKSPACE` has first been resolved to an absolute,
verified directory. An automated session should inspect existing neighboring
files and follow their formatting rather than mechanically copying quote style.

### Inventory before editing

```sh
find "$WORKSPACE/personas" -name persona.toml -print | sort
find "$WORKSPACE/characters" -name character.toml -print | sort
find "$WORKSPACE/forums" -mindepth 1 -maxdepth 1 -type d -print | sort
find "$WORKSPACE/system/providers" -mindepth 2 -maxdepth 2 \
  -name config.toml -print | sort
rg -n '^(display_name|provider|style|default_character|default_persona)\s*=' \
  "$WORKSPACE"
```

### Add an existing character to an existing forum

Change exactly one relationship by creating:

```text
forums/<forum-id>/members/<character-id>/character.toml
```

Use a comment-only file unless prompt variables were requested. Confirm the
global character exists. Update `default_character` only if requested. Then
run disposable validation.

### Create a new character and add it to one forum

Create exactly:

```text
characters/<group>/<id>/character.toml
characters/<group>/<id>/CHARACTER.md
characters/<group>/<id>/<optional-profile>.md
forums/<forum-id>/members/<id>/character.toml
```

Do not edit other forums. Do not create a new provider or style if an existing
one meets the request.

### Create a new forum from an existing pattern

Copy the structural pattern, not its prose:

```text
forums/<new-id>/config.toml
forums/<new-id>/FORUM.md
forums/<new-id>/members/<member-id>/character.toml
```

Add `members/character_defaults.toml` only when shared prompt variables are
needed. Confirm every member ID and the chosen persona/provider references.

### Change a character's provider

Edit only the global definition's `provider` field:

```text
characters/**/<character-id>/character.toml
```

Check compatibility of the character's `web_search` override with the new
provider. Do not add `provider` to forum member configs.

## 12. Import and export

### Safe disposable validation

There is no standalone `--validate` mode. Validate by importing into a new
temporary database, never by using the production database as a validator.
From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target chaweb_app

VALIDATION_ROOT="$(mktemp -d)"
cp -R packaging/linux/cha-config.example "$VALIDATION_ROOT/cha-config"
./build/ninja/chaweb \
  --config="$VALIDATION_ROOT/cha-config" \
  --vault=Personal \
  --import /absolute/path/to/workspace
./build/ninja/chaweb \
  --config="$VALIDATION_ROOT/cha-config" \
  --vault=Personal \
  --export "$VALIDATION_ROOT/exported"
find "$VALIDATION_ROOT/exported" -type f -print | sort
```

This performs no provider network requests. A successful import proves that
the TOML, IDs, references, templates, provider constraints, and required files
can form a complete `Workspace`. The export shows the exact accepted
configuration rows. Leave cleanup of the temporary directory until its path
and contents have been verified.

If validating `~/var/modify`, use the resolved path
`/home/mpopov/var/modify`.

### Import's normalization and pruning behavior

Import is not a pure parser. Before validation and commit it normalizes some
relationship changes:

- a forum member directory without `character.toml` receives a stored
  comment-only placeholder;
- member directories whose character definition is absent are dropped from
  the imported rows;
- a forum left with no valid members is dropped;
- an invalid `default_character` line is removed so the first surviving member
  becomes default;
- sessions belonging to forum IDs absent from the surviving import are deleted
  from the target database.

Do not rely on these repairs for ordinary edits. Keep the source directory
explicit and internally consistent. Most importantly, removing or renaming a
forum can delete its sessions during a real import.

An invalid import leaves an existing valid database configuration unchanged.
A valid import replaces the complete configuration row set while preserving
sessions for surviving forum IDs.

### Export from and import into the real database

The external config used by the real process is required for both commands.
The application must be stopped because runtime, import, and export compete for
the same non-blocking database lease.

Export requires a destination that is missing or empty:

```sh
/absolute/path/chaweb \
  --config=/absolute/path/cha-config \
  --vault=Personal \
  --export /absolute/path/empty-export-directory
```

After editing and disposable validation, import only when explicitly approved:

```sh
/absolute/path/chaweb \
  --config=/absolute/path/cha-config \
  --vault=Personal \
  --import /absolute/path/edited-workspace
```

Then restart CHA with the same external config. Import may create a missing
database, upgrade a supported schema-1 database, or replace configuration in a
schema-2 database. It preserves sessions only for forum IDs that survive.

Before a production import:

1. identify and stop the exact CHA process;
2. identify the exact configuration directory and resolve the target vault's
   `data` path;
3. make an offline, recoverable backup of the database and relevant secret
   files, including `<config-directory>/openai-auth.json` when present,
   according to the user's backup practice;
4. review forum IDs for removals or renames;
5. run disposable validation;
6. import;
7. restart and smoke-test one affected forum and provider.

Do not copy a live SQLite file casually; CHA uses WAL and sidecar files.

### Files deliberately outside workspace export

These are not workspace configuration rows and are not exported:

- the configuration directory (`app.toml` and vault files);
- source or configuration-directory `.env`;
- `<config-directory>/openai-auth.json` OAuth credentials;
- SQLite databases, journals, WAL/SHM sidecars, and `.cha-lock` files;
- session mirror output;
- files other than `.toml` and `.md`.

## 13. Verification checklist for each completed command

Before reporting completion, verify the relevant subset:

- `git status` still shows unrelated user changes untouched.
- The intended workspace root, not a live temporary materialization, was
  changed.
- Every new ID obeys the rules and is globally unique where required.
- Every character has both `character.toml` and `CHARACTER.md`.
- Every character and Assistant references an existing valid provider.
- Every referenced style exists.
- Every forum has `config.toml`, `FORUM.md`, and at least one valid member.
- Every `default_character` is a member.
- Every `default_persona` exists or is intentionally omitted for Guest.
- Include paths are correct for the file's actual nesting depth.
- Every included persistent fragment uses `.md` or `.toml`.
- No API key, OAuth token, credential JSON, database, or sidecar was added.
- Disposable import validation succeeds.
- Production import was not performed unless the user explicitly requested it.

When a real provider change is applied, configuration validation is not a
connectivity test. Also run an application smoke test with an assigned
character when the user asks for connectivity verification.

## 14. Troubleshooting map

| Symptom | Likely cause |
| --- | --- |
| `requires character.toml and CHARACTER.md` | A definition is missing one required file, or a grouping directory contains a stray specially named file |
| `references unknown provider` | Character/Assistant provider ID does not match a provider directory, or that provider was invalid and omitted |
| `references unknown style` | Missing or invalid style config |
| `default character ... is not a member` | Forum config points to an ID without a direct member directory |
| `references unknown persona` | `default_persona` does not match a loaded persona ID |
| duplicate character/persona ID | Two nested definitions have the same leaf directory name |
| display-name conflict | Character and persona public names must be globally unique case-insensitively |
| `unknown variable` | `$${...}` has no value in the merged prompt scope |
| include escapes the forum/workspace | Relative include crossed its containment root |
| unsupported web search | API/auth/host combination cannot use requested search mode |
| invalid `openai_subscription` settings | OAuth provider differs from one of the mandatory invariants |
| `Sign in to ChatGPT before using this provider.` | OAuth provider is configured but Settings has no connected account |
| import/export reports database busy | A CHA runtime or another maintenance operation holds the database lease |
| editing exported files changes nothing | Runtime reads committed SQLite configuration; the edited bundle has not been imported |

## 15. Source-of-truth implementation files

When behavior changes, inspect these implementation files before updating this
guide:

- `src/workspace/workspace.cpp`: schemas, IDs, references, prompt composition,
  built-ins, and runtime-editable fields;
- `src/workspace/workspace_config_store.cpp`: accepted import files,
  materialization, validation, pruning, import/export, `.env`, and database
  replacement;
- `src/util/text_template.cpp` and `.h`: prompt macros, scopes, containment,
  and limits;
- `src/util/path_name.cpp`: path component and forum ID validation;
- `src/util/public_name.cpp`: public-name and description validation;
- `src/characters/character_config.cpp` and `.h`: provider endpoint and enum
  semantics;
- `src/providers/openai_oauth.cpp`: OAuth credential lifecycle;
- `src/web/application_runtime.cpp`: configuration-directory OAuth credential
  path and runtime maintenance operations;
- `packaging/linux/import-seed/`: minimal package seed;
- `tests/application/unit_workspace.cpp` and
  `tests/application/unit_workspace_config_store.cpp`: executable examples of
  accepted and rejected configurations.

The directory `~/var/modify/` is a useful content example, but the source and
tests above are authoritative when the example and code disagree.
