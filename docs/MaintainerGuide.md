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
- add a visual style and assign it to characters;
- deploy or upgrade ChaWeb on Linux.

An exported workspace is the starting point for filesystem edits. The smaller
repository-owned example is `packaging/shared/import-seed/`; it is not the
active vault's configuration.

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
5. Do not modify `packaging/shared/import-seed/` unless the user explicitly
   asks to change the repository's example workspace.
6. Use the smallest set of files that expresses the requested change. Do not
   add schemas, generators, registries, or abstractions for ordinary content
   maintenance.
7. Validate the complete workspace after every change. All configured forums
   are loaded together, so an error in an otherwise unused forum can prevent
   CHA from starting.
8. Import into the real database only when explicitly requested, after
   identifying the exact configuration directory and active vault, and reviewing
   the destructive effects described in [Import and export](#12-import-and-export).

Resolve the user's export and database paths before writing or importing. Do
not assume a machine-specific example path or the current working directory is
the requested workspace.

## 2. Configuration sources and runtime state

CHA configuration can appear in several places. They have different roles.

| Location | Role | Edit directly? |
| --- | --- | --- |
| An exported directory such as `~/var/modify/` | Human-editable workspace bundle | Yes |
| `packaging/shared/import-seed/` | Example workspace configuration in the repository | Only when explicitly requested |
| The SQLite file named by the active vault's `data` field | Authoritative runtime configuration and sessions | Never by hand |
| A `cha-runtime-*` directory under the system temporary directory | Private materialization of committed SQLite rows | Never |
| `system/keys/` rows in each vault database | Model and R2 keys saved through Settings | Only through Settings → API Keys |
| `<config-directory>/openai-auth.json` | OpenAI subscription OAuth credentials | Only through Settings → OpenAI |
| `<config-directory>/api-keys.json` | Legacy model-key migration source | Carefully; each empty vault may import it |

Normal runtime reads configuration from SQLite. It does not continue reading
the directory that was imported. Therefore editing `~/var/modify/` alone does
not alter a running application. The normal lifecycle is:

```text
SQLite database -> export directory -> edit -> validate -> import -> SQLite database
```

Provider, style, and key definitions are workspace configuration and therefore
live inside each vault's SQLite database. Model and R2 secrets are plaintext in
owner-private `system/keys/api_key_N/config.toml` rows. They are vault-specific
and round-trip through workspace import/export.

An old export can also be stale. CHA can update characters, personas, forums,
providers, styles, voices, and model/R2 credentials while it is running. Those
edits are committed to SQLite. If a request concerns the current live
configuration, begin from a fresh export or explicitly reconcile the existing
bundle with one; otherwise a later import can overwrite changes made through
the UI. Do not replace a user's existing edit directory merely to refresh it
without first preserving or reviewing its contents.

The external application configuration is a directory, not part of a workspace
import. `app.toml` selects the startup vault and holds mirror/modify bases and
logging settings. Each other `.toml` file is one vault:

```toml
# app.toml
vault = "Personal"
mirror = "mirror"
modify = "modify"

[logging]
file = "/absolute/path/cha.log"
level = "info"
```

```toml
# personal.toml
vault_name = "Personal"
data = "/absolute/path/workspace.sqlite3"
protected = false
parent = "Main" # optional name of another locally registered vault
```

The optional `mirror` and `modify` values belong to `app.toml` and name base
directories. They may be relative to the configuration directory. A vault
named `Personal` uses `mirror/Personal` and `modify/Personal`; renaming it moves
existing derived directories to the new name. An existing derived `modify`
directory must be empty or a valid CHA workspace. `data` and `logging.file` may
also be relative to the configuration directory. Vault names must be valid
single path components because they become directory and default database
names. Obsolete extra fields in vault TOML, including old per-vault `mirror`
and `modify` values, are ignored with warnings instead of blocking startup.

The native CHA application runs inside its desktop host and has no application
HTTP listener. ChaWeb uses nginx to serve browser files and forward requests
to `cha-daemon` over a Unix socket; see [ChaWeb deployment](#16-chaweb-deployment-on-linux).
An obsolete `[web]` table is ignored with a warning, including unused invalid
listener values. Session runtime limits and deadlines are internal
`cha::RuntimeSettings`, not listener configuration.

The configuration directory must be outside the workspace import directory.
There is no automatic migration from a single `cha.toml`; create the directory,
split selection, mirror/modify bases, and logging into `app.toml`, put the
data path and display name in a vault file, adjust paths, and move
`openai-auth.json` into the directory. A legacy `api-keys.json` file may supply
model credentials to each empty vault the first time it is opened. It is left
unchanged, so remove or secure it after migration if future empty vaults should
not import it.
Export revalidates a nonempty modify directory and refuses to replace it unless
it is a valid CHA workspace.

### Vault management and switching

Vault files are discovered when CHA starts. Direct filesystem edits still
require a restart, but Settings → Vaults can create, rename, protect, download,
and remove vault definitions in the running application. Creation derives the
database filename from the display name. With no copy source, CHA carries the
active vault's workspace configuration into the new database without its
sessions. Choosing an existing vault as the source copies its full database,
including sessions. A protected source can be copied only while it is active.
Copying an existing vault also records that vault's name as `parent` in the
new definition. Neither choice makes the new vault active. A missing parent
or an invalid unused `parent` value produces a warning rather than blocking
startup.

The editable name on the vault's detail screen renames it; the database path
stays fixed, while existing derived mirror and modify directories move with the name. Settings can
encrypt a new or existing vault with a password, but cannot remove protection
or change that password. Only an inactive vault can be removed, and the last
vault cannot be removed. Removal deletes the vault's TOML definition but
deliberately keeps its database and derived directories.

Settings → Vaults → Download vault lists root-level `.sqlite3` objects in the
active vault's configured R2 bucket, excluding database filenames already
registered locally. Selecting one downloads and validates it, creates a local
vault definition, and leaves the active vault unchanged. A legacy object with
no companion `.toml` is named from its database filename. A protected R2 vault
cannot currently be added through this screen because the download flow has no
password entry.

Settings → Vaults → the active vault → Merge overlays configuration from an
inactive source into the active vault after confirmation. Source files replace
destination files at matching stored paths; destination-only files remain.
Personas, characters, forums, providers, styles, and saved model/R2 keys are
included, but source conversations are not copied. A source R2 key replaces
the destination R2 key even when their IDs differ. The next saved-key ID uses
the larger counter from the two validated workspaces. The source database and
active vault selection stay unchanged. A protected source prompts for its
password, which is not retained.

The combined workspace is validated before commit. Validation failure keeps
the old configuration and live sessions; success closes live sessions and
refreshes the interface for the new application context. A failed
voice-settings or discovery refresh does not undo the merge. Identical
configuration and already synchronized forums require no destination write.
Failure to restore or publish the workspace, or synchronize forums after
commit, makes the application unavailable and requires a restart. Mirror rebuild failures are
logged without undoing the merge.

Selecting a vault changes the vault for the whole running application, including
Import, Export, Upload, and Download. A successful switch closes live sessions,
opens the selected database, and reloads the native interface at Welcome.
Context epochs prevent requests captured before the switch from acting on the
new vault.

Protected vault databases use SQLCipher. The password is never stored: the
vault TOML contains only `protected = true`, and opening the vault requires the
password. The macOS and Windows applications prompt at launch, and switching
to a protected vault opens a password dialog. An incorrect password is
indistinguishable from a damaged encrypted database at the storage boundary.
There is no recovery path for a lost password. Workspace exports contain
plaintext configuration, while R2
upload copies the encrypted database bytes.

Password-protected vaults are never mirrored to local Markdown files. A
configured mirror is ignored with a warning at startup and after updates,
switches, merges, and downloads; an unused mirror path does not block updates
to a protected vault. Enabling protection disables the active mirror before
sessions resume. Existing Markdown copies are not deleted or encrypted, so
remove them separately when necessary. Explicit conversation downloads and
workspace exports remain plaintext. Switching to an unprotected vault resumes
its configured mirroring.

Failures have deliberately small, explicit outcomes:

| Failure | Runtime result |
| --- | --- |
| The target is unknown, busy, or invalid | The old vault remains active. |
| Session maintenance cannot enter the runtime queue or release live sessions before its deadline | The old vault remains active and the switch can be retried. Sessions already stopped must be reopened. |
| The target cannot be reopened after the database path changes | The application becomes unavailable because its database state is unusable. Quit and restart CHA; `app.toml` still selects the old vault. |
| The target mirror cannot be rebuilt | The switch succeeds with mirroring inactive. |
| The new selection cannot be saved to `app.toml` | The switch succeeds for the running process. The next launch uses the previously saved vault. |

Live sessions share one runtime thread; provider requests use independent
workers. Switching sessions leaves an accepted generation running in the
background until completion. Vault maintenance instead stops live controllers
and waits for their journals to close. Its session reservation has one deadline
covering queue admission, queued work, and journal release. Timeout cleanup
does not wait for the runtime queue, and a cancelled queued reservation cannot
start later. Recovery publishes a new context epoch so old queued commands
cannot run against the resumed application context. The shared runtime remains
alive across a successful switch; sessions open again as needed.

The macOS application stores this directory at
`~/Library/Application Support/CHA`. During native startup, an empty
configuration directory is bootstrapped with `app.toml`, `default.toml`,
`default.sqlite3`, and relative `mirror` and `modify` bases, with both base
directories created privately. The resulting vault is named `Default`; it has
no saved sessions and contains the built-in Assistant with a ChatGPT OAuth
provider. A nonempty configuration directory is not bootstrapped. When the
workspace loads, the macOS main window title is `CHA: <Vault name>`.

### Parent vaults and ChaWeb maintenance

A vault definition can contain `parent = "Main"`, where `Main` is the display
name of another vault registered in the same configuration directory. The
parent must be inactive for a merge. This field is outside workspace imports.

ChaWeb's session list provides Upload, Download, and Parent merge when the
active vault has R2 credentials. Parent merge first downloads and validates
the parent's database and companion definition from R2, using the active
vault's R2 key. It then merges the refreshed parent's configuration into the
active vault using the ordinary merge rules above. It does not copy parent
conversations or switch vaults. A protected parent prompts for its password.
The parent download keeps `.bac` backups; a later merge failure does not undo
that successful download. After Download or Parent merge, ChaWeb reloads its
bootstrap and session lists. There is no browser vault selector or general
settings editor.

### R2 database transfer

The active vault's R2 record under `system/keys/` enables Upload and Download
under Settings → Vaults → the active vault. Upload validates the vault
definition and schema-v2 database. It makes a temporary database copy, removes
all `entry_audio` rows from that copy, and runs `VACUUM` before transfer. Local
cached speech remains intact; uploaded vaults contain configuration and
conversations but no cached audio. Protected copies remain encrypted.
It then writes `<database-filename>` followed
by `<database-filename>.toml` at the bucket root. The companion uses the bare
database filename so its path is portable. The local vault definition records
`r2_etag` after the database upload, even if companion upload subsequently fails.
Before uploading, CHA checks the remote ETag. A missing local version, missing
remote object, or mismatch asks the user before replacement; a matching version
uses a conditional upload to reject a concurrent remote change. Because R2
cannot replace the pair atomically, retry any failed upload before relying on
Download.

Download stages and validates both objects before changing local state. It
keeps the previous definition and database beside them with `.bac` suffixes;
an HTTP, password, schema, or definition mismatch leaves the local pair
untouched. Database-only objects uploaded by an older CHA version must be
uploaded again with the current version before they can replace the active
vault. The Settings → Vaults → Download vault flow is deliberately different:
it may install a legacy database-only object as a new inactive vault, but it
cannot install a protected remote vault because it has no password input.

## 3. Workspace directory map

A representative workspace looks like this:

```text
workspace/
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
    ├── styles/
    │   └── serif-bold/config.toml
    ├── jev/
    │   └── config.toml                       # optional recipient detection
    ├── web-search/
    │   └── config.toml                       # Brave/Tavily search settings
    ├── voice-input/
    │   └── config.toml                       # optional transcription settings
    ├── voice-output/
    │   └── config.toml                       # optional FishAudio output settings
    └── voices/
        └── warm-narrator/config.toml
```

The loader requires `personas/`, `characters/`, `forums/`, and
`system/providers/` directories. `system/styles/` and `system/voices/` are
optional, but referenced character styles and voices, and persona styles, must exist.
`system/assistant/character.toml` and at least one usable provider are
effectively required because the built-in Assistant must select a provider.

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
system/voices/warm-narrator/config.toml        -> voice ID "warm-narrator"
```

Characters and personas may be nested under grouping directories. The grouping
path is organizational only and is not a namespace. Therefore
`characters/historical/guide/` and `characters/fictional/guide/` conflict: both
define character ID `guide`. Forums, providers, styles, voices, and forum member
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
- Provider, style, and voice IDs: one filesystem path component. For
  maintainability, still use the conservative lowercase convention above.

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

`persona.toml` supports these fields:

```toml
display_name = "Michael"                         # required
description = "A programmer living in Redmond." # optional, one line
style = "serif"                                # optional style ID
```

`persona.toml` also accepts a `[prompt]` table of scalar template variables.
`PERSONA.md` supports `$${variable}` substitutions and `$$(relative/file.md)`
includes. Reserved `$${persona.id}` and `$${persona.display_name}` values
cannot be overridden. Includes must remain within `personas/`; nested
persona definitions can share fragments there. The editor reads and saves the
unexpanded source, while model requests receive its expanded text.

Unknown fields are rejected. `PERSONA.md`, when present, describes the user to
the forum's characters. It can contain substantial first-person context and
communication preferences. If it is absent, the persona still exists but has
no prompt body.

`style` controls the appearance of human messages and references a definition
under `system/styles/`. Human messages do not support voice output. Legacy
`voice` settings are ignored with a warning and removed when the persona is saved.

Every forum chooses its active starting persona with `default_persona` in the
forum's `config.toml`. If omitted, CHA uses the built-in `guest` persona. A user
can change a forum's saved default persona from its Members screen. Saving
rewrites the forum's members and persona together in the configuration stored
in SQLite.

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
voice = "warm-narrator"           # optional voice ID
reasoning_effort = "high"         # optional: none | low | medium | high | xhigh
web_search = "auto"               # optional: provider-hosted off | auto | required
web_search_tool = true            # optional: override workspace on-demand search
tags = ["science", "historical"] # optional; unique case-insensitively

[prompt]                           # optional template variables
register = "plainspoken"
```

Unknown fields are rejected. `provider`, `style`, and `voice` are
case-sensitive ID references. Character `reasoning_effort` and `web_search`
override the selected provider's defaults. Omitting either inherits the
provider value. Omitting `voice` uses the configured FishAudio default
voice during playback. Tags must be nonempty strings after trimming, contain
no control characters, and be unique case-insensitively.

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
2. Choose an existing provider and, optionally, style and voice.
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

`display_name` is the name shown by the UI. If it is omitted, the name is
derived from the provider ID: `open_router` and `open-router` become
`Open router`. The pencil beside the provider title edits `display_name`; the
stable provider ID does not change.

### Provider fields

| Field | Required/default | Meaning and constraints |
| --- | --- | --- |
| `display_name` | derived from ID | User-facing provider name |
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
| `api_key` | `""` | ID of a model key stored under `system/keys/` in this vault |
| `api_key_env` | `""` | Legacy field spelling; its value is resolved as an exact display name among this vault's model keys, never as an environment variable |
| `reasoning_effort` | `"none"` | `none`, `low`, `medium`, `high`, or `xhigh`; legacy `minimal` becomes `low` |
| `reasoning_format` | `"auto"` | `auto`, `none`, `reasoning_content`, or `reasoning` |
| `api` | `"responses"` | `responses` or `chat_completions` |
| `auth` | no special auth | Only special value is `openai_subscription` |
| `web_search` | `"off"` | `off`, `auto`, or `required` |
| `cache_retention` | `"short"` | `off`, `short`, or `long` |

Unknown fields are rejected. A malformed unused provider is logged and omitted;
if any character selects it, workspace loading fails with the provider error.
Do not leave knowingly malformed unused provider directories behind.

Direct OpenAI Responses requests omit reasoning settings for non-reasoning
models, with a warning. Direct Mistral Chat Completions also omits
`reasoning_effort` for `mistral-large` and `mistral-large-*` models on
`api.mistral.ai`, with a warning rather than a failed request. Reasoning content is not shown in chat or stored in the
transcript. Completed character entries retain reported input/output token
counts; the UI shows a total when both are available. Diagnostic logs also
report cache usage and system-prompt size. Debug logs include conversation
payloads, Jev decisions, search queries/results, and FishAudio text with active
credentials redacted; use debug logging only where that content can be stored.

### Provider editor

The provider editor intentionally exposes only settings that distinguish one
normal provider from another:

- the provider name, edited with the pencil beside the title;
- Model;
- Base URL;
- API format (`Responses` or `Chat completions`);
- Credentials: a key created under Settings → API Keys, `OpenAI OAuth`, or
  `No credentials`.

API format and Credentials share one row. Base URL combines `host` and
`base_path`, and also carries the HTTP/HTTPS choice and any non-default port.
Saving through this screen fixes `mode = "net"`, `stream = true`, clears the
provider-level `reasoning_effort`, and sets `web_search = "off"`. Character
settings remain the place to select reasoning effort and web search.

The editor does not offer environment variables as credentials. Saving a
provider replaces any legacy `api_key_env` selection with the explicit
Credentials choice.

`Test` exercises the candidate currently shown in the form, including unsaved
changes. It does not write configuration or reload live sessions. The native
`provider.test` operation accepts the same `ProviderUpdate` data as Save and
sends one small, empty-history request asking the selected model to reply with
`OK`. It forces network mode, disables web search, and uses fixed 10-second overall and idle
timeouts. It calls `ProviderClient` directly and creates no session or
transcript. The bridge returns success or a typed operation error; provider and
authentication failures are shown in the form. The probe can consume a
small amount of provider usage.

`Delete provider` succeeds only when no character, built-in Assistant, or
enabled query rewriter uses the provider.

For ordinary providers, CHA constructs endpoints as follows:

```text
responses:        {scheme}://{host}:{port}{base_path}/v1/responses
chat_completions: {scheme}://{host}:{port}{base_path}/v1/chat/completions
```

### API-key provider example

Direct OpenAI Responses API:

```toml
display_name = "Sol"
host = "api.openai.com"
port = 443
https = true
mode = "net"
model = "gpt-5.6-sol"
api_key = "api_key_1"
```

OpenAI-compatible Chat Completions endpoint under a base path:

```toml
display_name = "OpenRouter"
host = "openrouter.ai"
port = 443
base_path = "/api"
https = true
mode = "net"
model = "qwen/qwen3.8-max"
api = "chat_completions"
api_key = "api_key_2"
```

Create keys under Settings → API Keys before selecting them in a provider.
The `api_key` value is an opaque local ID, not the secret. Secrets are stored in
the vault database as owner-private `system/keys/api_key_N/config.toml` rows and
are never returned to the frontend. They are included as plaintext in workspace
exports and imports, so treat every export as secret material. A missing
referenced key does not prevent workspace loading, but requests and `Test` fail
when they try to use it.

For limited compatibility, a hand-authored provider may still use
`api_key_env = "Name"` instead of `api_key`. Despite the old field name, CHA
looks for one API-key record whose display name is exactly `Name` in
the active vault; it never consults `.env` or the process environment for model
credentials. A missing or ambiguous name does not prevent workspace loading,
but that provider's requests and `Test` fail. When the name resolves, the
provider editor shows the matching saved key and writes the normal opaque
`api_key` ID when saved.

For migration only, an empty vault imports model keys from a legacy
configuration-directory `api-keys.json`. The source is left unchanged, so the
migration repeats for every subsequently opened empty vault unless the file is
removed manually. R2 credentials must be saved through Settings → API Keys.

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
- no `api_key` or `api_key_env`;
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

In the provider editor, selecting `OpenAI OAuth` makes this intent explicit and
requires API format `Responses` and Base URL
`https://chatgpt.com/backend-api/codex`.

### Web search compatibility

`web_search` other than `off` is accepted for:

- the Responses API, except an `openai_subscription` provider;
- OpenRouter Chat Completions (`host` equal to `openrouter.ai`, ignoring case
  and one trailing dot).

It is rejected for other Chat Completions hosts. This check applies to both a
provider default and a character override.

### Recipient detection and Search API

Settings → Recipient detection configures Jev. Its exported file is
`system/jev/config.toml`:

```toml
url = "https://openrouter.ai/api/alpha/decisions"
model = "typesafe/jev-1.13"
api_key = "api_key_1"
```

The saved key ID must identify an API key in this vault. Disable removes this
file. Invalid saved configuration is ignored with a warning. Jev classifies
the prompt's intended recipient, not which character is best qualified to
answer. It can choose one member, all characters, or Self. An accepted decision
updates the session's active target. A single-character decision also saves
that forum's default character; All and Self remain session targets only.
An undefined decision keeps the current target. Failure or the five-second timeout also uses the
captured target and reports a notice. Explicit mentions and `/mcast` recipients
stay fixed; Self-notes bypass classification.

Settings → Search API is separate from provider-hosted `web_search`. Its file
is `system/web-search/config.toml`:

```toml
provider = "brave"                # brave | tavily
api_key = "api_key_2"             # saved search-service key
tool_enabled = true              # workspace default for on-demand search
read_provider = "firecrawl"      # off | firecrawl
firecrawl_api_key = "api_key_3"   # saved Firecrawl key
```

On-demand search exposes CHA's `web_search` function to the answering model.
It needs a search key and does not require Jev. It works through Responses
and Chat Completions, including subscription Responses, when the model
supports function calls. A character's optional `web_search_tool = true` or
`false` overrides `tool_enabled`; omission inherits it. Provider-hosted search
has its own compatibility rules and is not controlled by this setting.

Page reading exposes `web_read` for a URL supplied by the user or found by
search. Select Firecrawl and its saved key independently of the search switch.
A character with `web_search_tool = false` receives neither CHA web tool;
omitting the override allows configured page reading even when the workspace
search default is off. An unavailable saved key disables the corresponding
tool with a warning. Jina Reader is no longer supported.

Each answer allows up to four function-call attempts shared by search and
reading. Errors return tool error JSON so the model can continue. At the limit
CHA removes tools and asks for a final answer. Intermediate tool-round content
is not shown as the answer. Brave/Tavily result JSON is stripped of media
metadata and capped at 32 KiB. Firecrawl returns title, URL, and Markdown capped
at 64 KiB. Tool descriptions instruct the model to name sources in plain words
without links. Raw results and calls do not become transcript entries; replies
retain a web-use marker and aggregate token usage across rounds.

Search before generation and query rewriting were removed. Old `enabled`,
`query_provider`, and Jina fields are ignored with warning logs. Unused obsolete
settings do not prevent startup or otherwise valid saves.

### Add a provider

The normal path is entirely in Settings:

1. For API-key authentication, create the key under Settings → API Keys.
   For OAuth, connect the ChatGPT account under Settings → OpenAI.
2. Open Settings → Providers, select `New provider`, and enter its name. Start
   from the default OpenAI settings or copy the settings of an existing
   provider, including its saved API-key selection.
3. Set or review Model, Base URL, API format, and Credentials.
4. Select `Test`. Fix any reported endpoint, credential, or provider error,
   then save the tested settings.
5. Select the provider in the intended character settings.

For a hand-authored import bundle, choose a provider ID, create
`system/providers/<id>/config.toml`, use the smallest suitable example above,
and add only needed optional fields. Change the intended global character
definitions and/or Assistant to `provider = "<id>"`, then validate the
workspace. Validation itself does not make a network request.

Removing a provider requires first changing every character and the Assistant
that references it. Search with:

```sh
rg -n 'provider\s*=' /absolute/workspace/characters \
  /absolute/workspace/system/assistant
```

## 9. Styles, voices, and the built-in Assistant

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

### Voices

Voices live at `system/voices/<voice-id>/config.toml`. Add or edit them in the
application's voice settings, or export, edit, validate, and reimport the workspace.

A minimal definition is:

```toml
elevenlabs_voice_id = "fish-reference-id"
```

A definition with all supported fields is:

```toml
display_name = "Warm Narrator"
description = "A warm, relaxed voice."
elevenlabs_voice_id = "fish-reference-id"
speed = 1.0
```

| Field | Required/default | Meaning and constraints |
| --- | --- | --- |
| `display_name` | derived from ID | User-facing name |
| `description` | omitted | Optional voice description |
| `elevenlabs_voice_id` | required | Nonempty FishAudio reference ID; the field name is retained for compatibility |
| `speed` | omitted | Number from `0.7` through `1.2`; `1.0` is normal speed |

Only `elevenlabs_voice_id` is required. CHA sends an optional `speed` as FishAudio's
`prosody.speed`; when omitted, FishAudio uses its default. Obsolete `stability`,
`similarity_boost`, `style`, and `use_speaker_boost` fields are ignored with a
warning once per process and removed when the voice is saved. API keys and voice
output settings are vault settings. ElevenLabs output is no longer supported.
The endpoint must use HTTPS on `api.fish.audio`;
the playable output formats are `mp3`, `wav`, and `opus`. Legacy format strings
such as `mp3_44100_128` and `opus_48000_64` normalize to their container names on
load and save; their encoded sample rate and bitrate are no longer used. Unsupported
saved formats fall back to `mp3` with a warning, and new saves reject them.

Configure output under Settings → Voices → Voice settings. Save the FishAudio key
under Settings → API Keys first, then select that key and a default voice.
The equivalent exported `system/voice-output/config.toml` is:

```toml
url = "https://api.fish.audio/v1/tts"
model = "s2.1-pro"
api_key = "fish-audio"             # saved API-key ID, not the secret
output_format = "mp3"             # mp3 | wav | opus
default_voice = "Warm Narrator"   # voice display name, not directory ID
```

An invalid saved output configuration, including an old ElevenLabs endpoint,
is ignored with a warning instead of preventing the workspace from loading.
Replace its endpoint, model, key, and reference IDs with FishAudio settings to
restore synthesis. Speech requests and credential access run in native code;
the interface receives a resource handle for playback.

Assign the stable directory ID in a global character definition:

```toml
voice = "warm-narrator"
```

The built-in Assistant accepts the same field. Forum defaults and member
overrides do not. A missing assignment retains the application fallback voice
and does not hide the playback button. An unknown voice reference or invalid
voice definition makes workspace validation fail.

To add or tune a voice through exported files:

1. Export the vault's workspace configuration.
2. Create or edit `system/voices/<voice-id>/config.toml`.
3. Add `voice = "<voice-id>"` to the intended global character definitions or
   `system/assistant/character.toml`.
4. Validate the complete exported workspace.
5. Import the directory back into the vault.

Voice output is available for completed character replies. Characters can
select a voice; unassigned characters use the configured default voice. Changing a voice affects future synthesis;
an already cached clip keeps its original voice until the session's audio cache
is cleared.

Obsolete `instrumentation_provider` and `instrumentation_reasoning_effort`
fields are ignored with warnings and removed on save. Speech text goes directly
to FishAudio without an intermediate model request.

### Voice input

Voice input is a separate file, `system/voice-input/config.toml`. Saving it
does not read or write voice output. A file with no `provider` field loads as
OpenAI. The application writes `provider` on every save. There is no migration.

```toml
provider = "openai"  # openai | xai; omit to load as openai
url = "https://api.openai.com/v1/realtime/calls"
model = "gpt-live-transcribe"
api_key = "openai"   # saved API-key ID, not the secret
delay = "low"        # low | medium | high | xhigh; used by OpenAI only
prompt = ""
send_phrase = "over to you"  # empty disables hands-free sending
```

Use `provider = "xai"`, `url = "wss://api.x.ai/v1/stt"`, and
`model = "grok-voice-transcribe-2.0"` for xAI. OpenAI accepts an absolute
`http://` or `https://` URL. xAI accepts an absolute `ws://` or `wss://` URL.
Each provider rejects the other scheme. An invalid `delay` on an xAI file
becomes `low`, and the warning does not include the old value. xAI does not
send `delay` or `prompt`.

OpenAI dictation uses the browser WebRTC session and a native HTTPS call.
xAI dictation captures microphone audio in the page and sends it through the
native WebSocket client. The API key stays in native code. xAI keeps one
word-time cursor for the dictation and appends a word only when its end time
is later than that cursor. A final event that has text and no word timings
fails with `xAI returned a transcript without word timings.` A curl build
without the `ws` and `wss` protocols rejects xAI at startup and leaves OpenAI
usable. The macOS build uses bundled curl 8.14.1, which includes `wss`.

The composer removes a trailing send phrase and submits after a one-second
pause once generation is idle, keeping the microphone active. An empty phrase
disables hands-free sending. Manual Send finishes dictation first. Microphone
capture pauses during speech playback and resumes afterward. xAI interim text
is a replaceable preview; only finalized word-timed text is committed.

### Stored audio and background downloads

Audio is stored in SQLite's `entry_audio` table as one audio BLOB and MIME type
per `(session_key, entry_id)`. Saved-session clips survive page reloads and
application restarts and travel with direct complete database copies. R2
uploads exclude cached audio from their temporary copy.
Welcome uses its temporary session database, so its audio is not durable across
application restarts. Cached clips can be played without a working synthesis
configuration; playback obtains a native resource handle for the stored bytes.

Selecting a completed character response's speaker control
submits a short download request if audio is missing. `AudioDownloadManager`
owns the queue and three worker threads; repeated requests for the same entry
share the existing job. It captures output settings, the key, and voice at
acceptance, derives text from the stored entry, and retries transport failures
up to four attempts. Storage failures are terminal. The frontend polls status
while jobs are pending. A selected growing MP3 resource can begin playback
before the download finishes when MediaSource is supported; other formats
and resumed clips use complete-clip playback. Native resources handle partial
reads and keep the connection/context lifetime checks.
Failed entries expose a retry control. Stopping playback preserves its position
in document memory; replay resumes there until the clip finishes or the page reloads.

The speaker toggle before `Rus` enables automatic audio responses and, when
voice input is available, toggles the microphone with it. Replies completed
before enabling are skipped. New nonempty completed character replies are
submitted in batches and played once in transcript order, even if downloads
finish out of order. A batch is fully validated before new jobs are admitted.

Automatic MP3 playback at the beginning of a clip adds 2.5 seconds of silence
to let the output device start without losing the first spoken samples. This
padding is generated for playback and is not saved in the cached clip. Manual
playback and resumed clips have no padding. Saved playback positions exclude
the silence. Microphone capture pauses during generation, speech loading,
playback, and a 400 ms echo tail, then reconnects when voice mode is enabled.

Disabling automatic audio stops its current playback and future submissions.
Changing sessions and clearing cached audio also turn it off, but accepted
jobs continue independently of the displayed screen or document connection.
Jobs and failure state are in memory and do not resume after an application
restart. Vault switching, configuration import, and database replacement cancel
jobs so stale results cannot enter another vault. Other maintenance pauses
admission while repository access is fenced.

Recent → session menu → Clear audio cache cancels that session's queued and
running jobs and removes its stored clips without changing the transcript.
Deleting transcript entries removes their audio through the table's cascading
foreign key. Character and voice previews use a separate uncached native
FishAudio operation, which retains the key in native code and admits at most
four requests at once. They generate a fresh sample each time.

### Built-in Assistant

`system/assistant/character.toml` configures the built-in character used in the
Entrance forum. A minimal file is:

```toml
display_name = "Assistant"
provider = "sol"
```

It accepts the same definition fields as a global character, including style,
voice, reasoning, web search, tags, and description. Its prompt is compiled into CHA;
there is no `system/assistant/CHARACTER.md`. The Assistant ID, name, and
Entrance membership are built in. Do not create user definitions with the
reserved `assistant` or `entrance` IDs.

## 10. Prompt templates and overrides

CHA expands two macro forms in character, forum, and persona prompts:

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
register = "definition"
```

```toml
# members/character_defaults.toml
[prompt]
register = "forum default"
relationship = "colleague"
```

```toml
# members/feynman/character.toml
[prompt]
register = "forum-specific"
```

The effective values for Feynman are `register = "forum-specific"` and
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

### Forum-specific character additions and session variations

`forums/<forum>/members/<character-id>/CHARACTER.md` adds to the global
character prompt. It no longer replaces the global `CHARACTER.md`. When it
contains `<character_profile>`, CHA extracts that profile's text for the
addition. It inserts the addition before the global profile's closing tag,
or appends it if there is no profile boundary. Global includes remain within
`characters/`; includes in the member addition remain within that forum.
Do not include the global character again in the member addition.

A global character directory can also contain Markdown files named
`_<category>_<variant>.md`, for example `_1_1.md`, `_1_2.md`, and `_2_1.md`.
Both filename components must parse as integers. CHA chooses one file per
category, in category order, and adds its trimmed text to the effective profile
and character description. Selection is seeded by forum ID, session ID,
character ID, and category. Reopening the same session with unchanged files
keeps the same selection; adding another category does not change existing
category choices. Variation text is read literally, without template expansion. Variations are session-specific and do not change the global
catalog description.

### Final system-prompt composition

For each forum member, CHA constructs the system prompt in this order:

1. generated Conversation protocol naming the character and other members,
   defining shared JSONL history and timestamp metadata;
2. `<character_instructions>` containing the expanded global character,
   forum-member additions, and session variations;
3. `<forum_instructions>` containing expanded `FORUM.md`;
4. Participants containing the expanded persona in `<participant_profile>`.

Entrance also wraps its inventory in `<workspace_inventory>` as reference data.
When encoding a generation request, CHA appends `<tool_availability>` that
reports the web capabilities actually attached to that request. Continuations
update it when tools are removed. This lets a character continue with available
information and state uncertainty when verification is unavailable or fails.
Session naming omits this tool block. Message timestamps are metadata and
must not be spoken or written as part of the answer.

This explains where a change belongs:

- identity and writing voice shared everywhere: global character prompt;
- behavior shared by every character in one forum: `FORUM.md`;
- information about the human participant: `PERSONA.md`;
- shared forum variables: `character_defaults.toml`;
- one character's additional behavior or variables in one forum: member files;
- session-specific character traits: numbered variation files.

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

Validate an edited workspace in a separate vault before importing it into the
vault whose conversations matter. The old `chaweb --import`/`--export` executable
is no longer built; use Settings → Vaults → the active vault.

1. In Settings → Vaults, create a uniquely named validation vault. Leave the
   copy source unset so it receives configuration without copying conversations.
2. Switch to that vault and verify its name in the application. Its database
   must be distinct from the production database.
3. Choose Export in the validation vault's settings. This creates or refreshes
   its derived modify directory, `<modify-base>/<validation-vault-name>`.
4. Replace the contents of that disposable directory with a copy of the edited
   workspace. Copy the contents, not an extra enclosing directory, and leave
   the original edit bundle untouched.
5. Choose Import in the validation vault's settings. Read any validation error
   before changing the production vault. Do not send prompts or request speech
   for this check.
6. After success, choose Export again and inspect the normalized
   result. Compare the forum IDs and accepted files with the intended changes.
7. Switch back to the original vault. Removing the inactive validation vault
   removes only its definition; its database and derived directories remain
   for explicit cleanup after their paths and contents have been checked.

Import and Export require a `modify` base in `app.toml`. If it is absent, add a
suitable base and restart before this workflow. A successful import proves the
TOML, IDs, references, templates, provider constraints, and required files can
form a complete `Workspace`. It does not prove provider credentials or endpoint
availability; those have a separate Test action.

The validation vault and its export can contain copied credentials. Treat them
with the same care as the source workspace.

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

Open Settings → Vaults → the intended active vault. Import and Export
are enabled when `app.toml` supplies a `modify` base, and use the directory
formed by appending the active vault's display name. Export replaces that
directory; it must be missing, empty, or a valid CHA workspace. Preserve any
unimported edits before exporting again.

1. Verify the active vault and resolve its database and derived modify paths.
2. Preserve a recoverable database backup using the user's backup practice. If
   making a filesystem copy, close CHA first and account for SQLite sidecars;
   reopen the same vault afterwards. External configuration and OAuth files
   are outside workspace export and need their own backup when relevant.
3. Choose Export, then edit the exported files.
4. Review removed or renamed forum IDs and validate the edited bundle in a
   disposable vault as described above.
5. Return to the intended vault and choose Import within the user's
   authorized scope. This replaces configuration, preserving sessions only for
   surviving forum IDs.
6. Let the interface refresh, then check one affected forum and use provider
   Test if a connection changed.

The runtime retains its database lease and coordinates session, configuration,
repository, and media owners during maintenance. A successful native import
reopens and publishes the workspace in process; it does not require a manual
stop/import/restart sequence. If storage cannot be reopened, the application
becomes unavailable and must be restarted.

Workspace export contains configuration only, not conversations or cached audio.
Use a full database backup for conversations and cached audio. The active
vault's Upload action includes conversations but excludes cached audio. Never
copy a live SQLite database file without accounting for its WAL and sidecars.

### Files deliberately outside workspace export

These are not workspace configuration rows and are not exported:

- the configuration directory (`app.toml` and vault files);
- legacy `<config-directory>/api-keys.json` migration input;
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
connectivity test. Run `Test` against the candidate before saving when the user
asks for connectivity verification.

### Session creation and naming

New session opens immediately under the label `New session`, without a naming
form. It is initially absent from Recent and can be discarded on navigation.
A nonempty submission retains it, including a submission rejected before a
turn is stored. Its first stored human message starts a title request using
the configured naming provider, or Assistant's provider by default, the
configured effort (low by default), no web tools, and at most 10 seconds. The reply
and naming request run independently. Naming publishes the session in Recent
whether it succeeds or fails. Manual rename cancels naming and keeps the
session. Startup/maintenance recovery removes empty pending sessions and
publishes pending sessions that already have entries.

## 14. Troubleshooting map

| Symptom | Likely cause |
| --- | --- |
| `requires character.toml and CHARACTER.md` | A definition is missing one required file, or a grouping directory contains a stray specially named file |
| `references unknown provider` | Character/Assistant provider ID does not match a provider directory, or that provider was invalid and omitted |
| `references unknown style` | Missing or invalid style config |
| `default character ... is not a member` | Forum config points to an ID without a direct member directory |
| `references unknown persona` | `default_persona` does not match a loaded persona ID |
| `Provider test failed: ...` | Candidate endpoint, model, credential selection, provider availability, or network failure |
| duplicate character/persona ID | Two nested definitions have the same leaf directory name |
| display-name conflict | Character and persona public names must be globally unique case-insensitively |
| `unknown variable` | `$${...}` has no value in the merged prompt scope |
| include escapes the forum/workspace | Relative include crossed its containment root |
| unsupported web search | API/auth/host combination cannot use requested search mode |
| invalid `openai_subscription` settings | OAuth provider differs from one of the mandatory invariants |
| `Sign in to ChatGPT before using this provider.` | OAuth provider is configured but Settings has no connected account |
| `Password required to open this vault` | The selected vault has `protected = true`; enter its SQLCipher password in the launcher or vault-switch dialog |
| `The vault password is incorrect, or its database is damaged` | The supplied password cannot open the protected database; retry carefully, then restore a known-good backup if the password is correct |
| `Protected vaults cannot be downloaded from R2 without a password` | Settings → Vaults → Download vault cannot add encrypted remote vaults; register the vault locally and use Download under Settings → Vaults → the active vault instead |
| import/export reports database busy | A CHA runtime or another maintenance operation holds the database lease |
| editing exported files changes nothing | Runtime reads committed SQLite configuration; the edited bundle has not been imported |
| vault switch reports that restart is required, or the page becomes unavailable during a switch | Reopening the selected database failed and the application became unavailable; quit and restart CHA |
| startup reports that the selected vault is unknown after a vault file was deleted | `app.toml` still selects the deleted vault; select an existing vault in that file |

## 15. Source-of-truth implementation files

When behavior changes, inspect these implementation files before updating this
guide:

- `src/workspace/workspace.cpp`: schemas, IDs, references, prompt composition,
  built-ins, and runtime-editable fields;
- `src/workspace/workspace_config_store.cpp`: accepted import files,
  materialization, validation, pruning, import/export, and database replacement;
- `src/util/text_template.cpp` and `.h`: prompt macros, scopes, containment,
  and limits;
- `src/util/path_name.cpp`: path component and forum ID validation;
- `src/util/public_name.cpp`: public-name and description validation;
- `src/characters/character_config.cpp` and `.h`: provider endpoint and enum
  semantics;
- `src/providers/api_key_store.cpp` and `.h`: vault-backed model and R2 key lifecycle;
- `src/providers/openai_oauth.cpp`: OAuth credential lifecycle;
- `src/app/settings_operations.cpp` and `src/app/application.cpp`: provider,
  style, key, and voice settings operations plus asynchronous provider testing;
- `src/app/application_config.cpp`: application and vault configuration
  discovery, validation, and empty-directory bootstrap;
- `src/app/application.cpp` and `src/app/vault_operations.cpp`: application
  ownership, context admission, vault lifecycle, and database maintenance;
- `src/bridge/settings_dispatch.cpp` and `src/bridge/workspace_dispatch.cpp`:
  native operation dispatch;
- `packaging/macos/main.swift`: native runtime ownership and window-title
  synchronization;
- `packaging/shared/import-seed/`: example workspace seed;
- `tests/workspace/unit_workspace.cpp` and
  `tests/workspace/unit_workspace_config_store.cpp`: executable examples of
  accepted and rejected configurations;
- `tests/app/unit_application.cpp` and `tests/app/unit_vault_maintenance.cpp`:
  runtime maintenance and vault switching, including failure outcomes.

For the ownership and native message flow, see [the codebase tutorial](tutorial.md).
For adding an editor, see [editing workspace entities](editing.md).

The directory `~/var/modify/` is a useful content example, but the source and
tests above are authoritative when the example and code disagree.

## 16. ChaWeb deployment on Linux

Deploy the daemon and browser files from the same Linux package. nginx serves
`chaweb/` and forwards `/api/cha/v1/` to a user's systemd socket. There is no
Node.js server. The deployment machine needs nginx and systemd; Node.js is
needed only to build the package. The packaging reference is
[CHA daemon on Linux](../packaging/linux/README.md).

### Current installation

The installation on ThinkStation uses these paths and addresses. Confirm them
before deploying to another machine.

| Setting | Value |
| --- | --- |
| Application owner and service account | `mpopov` |
| Application directory | `/home/mpopov/opt/cha` |
| Static root | `/home/mpopov/opt/cha/chaweb` |
| User configuration and data | `/home/mpopov/var/cha/<user>/config` |
| nginx site | `/etc/nginx/conf.d/chaweb.conf` |
| Tailscale hostname | `thinkstation.tailb1b984.ts.net` |
| Tailscale address | `100.93.184.7` |
| LAN address | `192.168.86.39` |
| Michael's port and socket | `8443`, `/run/cha/michael.sock` |
| Annushka's port and socket | `8444`, `/run/cha/annushka.sock` |

Both users share the static files but have separate daemons and vaults. The
port selects the user's socket. ChaWeb has no login or API key, so bind its
listeners only to the intended private networks. The current text interface
works over HTTP. HTTPS is used on the Tailscale address and localhost; HTTP is
used on the LAN address, with the same port assignments.

### Build, back up, and install

Run these commands as `mpopov` from the repository root. Select the release
version; `0.2.3.1` is the package used for the initial ChaWeb deployment.

```sh
cha_version=0.2.3.1
make package-linux VERSION="$cha_version"
```

This produces `packages/cha-linux-$cha_version.tar.gz`, including the daemon,
the typechecked ChaWeb build, and installation scripts. If that package is
already built, use it directly instead of rebuilding.

Before an upgrade, finish or stop active conversations and copy any unsent
browser drafts. Stop both sockets and services before copying the databases;
an active socket can start a stopped daemon again. Back up the application,
data, nginx configuration, and systemd units:

```sh
export CHA_DEPLOY_PATH=/home/mpopov/opt/cha
export CHA_DATA_PATH=/home/mpopov/var/cha
backup_dir="$HOME/var/cha-deployment-backups/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$backup_dir"
chmod 700 "$backup_dir"
sudo cp -a /etc/nginx/conf.d "$backup_dir/nginx-conf.d"
sudo cp -a /etc/systemd/system/cha@.service \
  /etc/systemd/system/cha@.socket "$backup_dir/"
sudo systemctl stop cha@michael.socket cha@annushka.socket \
  cha@michael.service cha@annushka.service
sudo tar -czf "$backup_dir/application-and-data.tar.gz" \
  -C /home/mpopov opt/cha var/cha
sudo chown -R mpopov:mpopov "$backup_dir"
```

Extract and install the package. `install.sh` invokes sudo for system changes;
start it as the regular user so it selects the correct application owner.

```sh
stage_dir=$(mktemp -d "$HOME/var/cha-deploy.XXXXXX")
tar -xzf "packages/cha-linux-$cha_version.tar.gz" -C "$stage_dir"
"$stage_dir/cha-linux-$cha_version/install.sh"
sudo chown -R mpopov:mpopov "$CHA_DEPLOY_PATH" "$CHA_DATA_PATH"
```

The installer replaces the daemon, static files, example vault, and helper
scripts. It preserves existing systemd units, nginx sites, and user vaults. It
installs a ChaWeb nginx example but does not create the per-user listeners.
Keep data and TLS keys outside the static root.

For existing users, start their sockets after installation:

```sh
sudo systemctl start cha@michael.socket cha@annushka.socket
```

Their next API request starts the new daemon. If installing without stopping
the services first, run `sudo systemctl try-restart 'cha@*.service'` to load
the new binary. Check that `/etc/systemd/system/cha@.service` uses `User=mpopov`
and these deployment/data paths; existing units are not rewritten.

For a new user, prepare its existing configuration directory and database,
then run `"$CHA_DEPLOY_PATH/add_user.sh" USER` with the same exported paths.
The script enables the socket. Configure working server provider credentials
in the vault.
ChatGPT subscription providers are not supported by the daemon. For a
protected vault, supply `config/password`, owned by `mpopov`, with mode `0600`.

### nginx access and listeners

The nginx worker is `www-data`. It needs read access to the static files and
search permission on every parent directory. For the current paths:

```sh
sudo setfacl -m u:www-data:--x /home/mpopov
sudo -u www-data test -r "$CHA_DEPLOY_PATH/chaweb/index.html"
```

Grant search permission on other parent directories only if needed. Keep
vault directories private. The systemd socket template uses group `www-data`,
mode `0660`, and `Accept=no`; nginx needs access to that socket group.

On first setup, copy `nginx-chaweb.conf.example` from the installed package
to `/etc/nginx/conf.d/chaweb.conf`. On upgrades, compare the new example with
the existing site and apply only the required changes. Use one server block
per user, retaining the template's locations and SCGI settings. Before enabling
HTTPS, obtain the certificate as described under
[TLS certificate setup and renewal](#tls-certificate-setup-and-renewal).
Michael's server-level settings are:

```nginx
listen 100.93.184.7:8443 ssl;
listen 127.0.0.1:8443 ssl;
listen 192.168.86.39:8443;
server_name thinkstation.tailb1b984.ts.net;
ssl_certificate /home/mpopov/opt/cha/tls/thinkstation.tailb1b984.ts.net.crt;
ssl_certificate_key /home/mpopov/opt/cha/tls/thinkstation.tailb1b984.ts.net.key;
ssl_protocols TLSv1.2 TLSv1.3;
root /home/mpopov/opt/cha/chaweb;
```

In its `/api/cha/v1/` location, use
`scgi_pass unix:/run/cha/michael.sock;`. Duplicate the server block for
Annushka, replacing all three ports with `8444` and the socket with
`unix:/run/cha/annushka.sock`.

The LAN `listen` line intentionally omits `ssl`. A plain HTTP request to an
SSL listener fails even when the IP and port are correct. The same server
block can serve HTTP and HTTPS on different local addresses. For an HTTP-only
installation, omit the SSL listeners and TLS directives.

Keep the template's 256 KiB request limit, buffered SCGI responses, JSON gzip,
and `Cache-Control: no-store` on API successes and errors. The entry document
uses `no-cache`; content-hashed assets use the immutable cache header. Include
nginx's `mime.types` and standard `scgi_params`, which forwards `CONTENT_TYPE`.
ChaWeb routes directly to fixed sockets. The daemon no longer serves the
OpenAI-compatible `/v1/` API. If `/etc/nginx/conf.d/cha.conf` and
`/etc/nginx/cha-users.map` remain from that listener, remove them.

Validate before reloading:

```sh
sudo nginx -t
sudo systemctl reload nginx
```

### TLS certificate setup and renewal

The current HTTPS listeners use a Tailscale certificate copied into
`$CHA_DEPLOY_PATH/tls/`. Obtain or renew it with:

```sh
install -d -m 700 "$CHA_DEPLOY_PATH/tls"
sudo tailscale cert \
  --cert-file="$CHA_DEPLOY_PATH/tls/thinkstation.tailb1b984.ts.net.crt" \
  --key-file="$CHA_DEPLOY_PATH/tls/thinkstation.tailb1b984.ts.net.key" \
  thinkstation.tailb1b984.ts.net
sudo chown -R mpopov:mpopov "$CHA_DEPLOY_PATH/tls"
chmod 644 "$CHA_DEPLOY_PATH/tls/thinkstation.tailb1b984.ts.net.crt"
chmod 600 "$CHA_DEPLOY_PATH/tls/thinkstation.tailb1b984.ts.net.key"
openssl x509 -in "$CHA_DEPLOY_PATH/tls/thinkstation.tailb1b984.ts.net.crt" \
  -noout -dates
sudo nginx -t
sudo systemctl reload nginx
```

Renewal is currently manual. The certificate installed on September 30, 2026
expires on December 27, 2026. Refresh it before expiry and reload nginx;
Tailscale Serve renewing its own certificate does not update these copied files.

### Verify the deployment

Check both users through nginx, not by connecting directly to the daemon:

```sh
systemctl status cha@michael.socket cha@annushka.socket
curl --fail --silent --show-error -I http://192.168.86.39:8443/
curl --fail --silent --show-error http://192.168.86.39:8443/api/cha/v1/bootstrap
curl --fail --silent --show-error http://192.168.86.39:8444/api/cha/v1/bootstrap
curl --fail --silent --show-error https://thinkstation.tailb1b984.ts.net:8443/api/cha/v1/bootstrap
curl --fail --silent --show-error https://thinkstation.tailb1b984.ts.net:8444/api/cha/v1/bootstrap
sudo find "$CHA_DEPLOY_PATH" "$CHA_DATA_PATH" \! -user mpopov -print
```

The page must return `200`, `text/html`, and `Cache-Control: no-cache`.
Request the JavaScript and CSS paths from `index.html` and check their MIME
types and immutable cache header. Bootstrap and session responses must be JSON
with `no-store`; request a snapshot with `Accept-Encoding: gzip` to verify
compression. Confirm each port reaches its own vault, using session lists if
both vaults have the same forum names. The ownership check must print nothing.

Open each user's page in a browser. Send a short test message, wait for a
complete reply, reload, and verify the saved conversation. Restart that user's
service and reopen the same session to check persistence. On an
iPhone, check keyboard opening and dismissal, editor resizing, rotation, and
keyboard dictation while replies are polled.

After a successful check, remove the extracted staging directory and keep the
backup. Reload browser tabs after upgrades; unsent drafts do not survive reload.
For `403`/`404` static-file errors, check nginx's parent-directory permissions
and static root. For `502` API errors, check the socket, service, and
`journalctl -u cha@USER.service`, then inspect nginx's error log.
