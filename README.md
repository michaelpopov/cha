# CHA

CHA is a C++20 browser application for chatting with OpenAI-compatible model
servers. The `chaweb` process serves the browser client and its HTTP/SSE API.

## Start chatting

Initialize a development database once, then start the staged application:

```sh
make import-dev CONFIG="$PWD/cha-config" VAULT=Dev
make run
```

Open the address printed by the launcher. CHA starts in the process-local
**Entrance / Welcome** conversation as **Guest** with **Assistant**. Use the
browser to inspect forums, create or reopen a stored session, and select a forum
character. Which persona you speak as follows the forum you are in and is set in
that forum's configuration, not in the browser.

Welcome is private to the running server and is deleted on shutdown. All stored
sessions and workspace metadata remain in the SQLite database selected by the
active vault.

The chat input also accepts these controller-level commands:

| Command | Purpose |
| --- | --- |
| `/mcast` | Send one prompt to multiple forum characters. |

Leading `@Name` addresses a prompt to one character. `@@` starts literal text
with an at-sign. A handle may be a display name, an unambiguous part of one, or
the character's ID — useful when the display name is spelled differently or
written in another script.

The reserved handle `-` is the null target: `@- <text>` records one message in
the transcript without calling a model or producing a reply. Choosing
Self-notes in the target selector records plain messages the same way until
the selector switches back to a character. Recorded messages are
durable and reach a later character as shared conversation history, never as a
message addressed to it.

## Configuration

A configuration import directory contains `characters/`, `forums/`,
`personas/`, and `system/` as needed. `characters/` and `personas/` may use
nested grouping directories;
the directory containing a definition file supplies that character or persona's ID.
The `personas/` directory may be empty because the built-in Guest
persona is always available, and is what a forum that names no `default_persona`
speaks as. Persona, character, and forum definitions have a public
`display_name` and may have a one-line `description`.

A forum's `config.toml` can name its starting character with
`default_character = "character-id"`. The ID must be a forum member; when the
setting is omitted, the first member ID in lexicographic order is used. The
chat target selector changes the live session immediately and saves that ID to
the forum config, so the next session in that forum starts with it. CHA validates this narrow online
edit, commits the complete configuration to SQLite, and then publishes it.

A forum's persona is selected when the forum is created or from its Members
screen. Saving Members stores the selected ID as `default_persona` in the forum
config and reloads the forum's live sessions.

The external application configuration is a directory. `app.toml` selects the
startup vault and holds web and logging settings. Each other `.toml` file is
one vault and supplies that vault's data paths:

```text
cha-config/
├── app.toml
└── personal.toml
```

```toml
# app.toml
vault = "Personal"

[web]
host = "0.0.0.0"
port = 8086

[logging]
file = "logs/cha.log"
level = "info"
```

```toml
# personal.toml
vault_name = "Personal"
data = "/var/lib/cha/workspace.sqlite3"
mirror = "/home/user/cha-mirror"
```

The optional `mirror` setting continuously writes each persistent session as
Markdown under a display-named forum directory; omit it to disable mirroring.
The root directory must already exist, sessions are refreshed after terminal
responses and context-boundary changes, renamed with their sessions, and
retained after deletion.
This copies transcripts out of the SQLite workspace into plain files. CHA
writes those files with mode `0600`, but does not change permissions on the
configured root or existing forum directories, so choose the location
accordingly.

Each completed provider request writes its HTTP metadata, provider request ID,
and reported `input_tokens`, `output_tokens`, `cache_read_tokens`, and
`cache_write_tokens` to this diagnostic log. Chat Completions streaming
requests ask for the final usage block explicitly; Responses includes usage in
its completion object. A compatible provider that omits a field is logged as
`unreported` rather than estimated locally.

Each provider lives in `system/providers/<id>/config.toml`. Its file contains
the connection, model, protocol, and optional saved-key reference:

```toml
host = "api.openai.com"
port = 443
https = true
mode = "net"
model = "gpt-5.6-terra"
reasoning_effort = "low"
stream = true
api = "responses"          # responses | chat_completions
web_search = "off"         # required | auto | off
api_key = "api_key_1"
cache_retention = "short"  # off | short | long
timeout_s = 600
idle_timeout_s = 60
max_tokens = 4096
# temperature = 0.7        # omitted from requests when unset
```

Generation stops after `timeout_s` overall, or after `idle_timeout_s` without a
single received byte. That idle timer starts at the first byte of the response,
so a model that thinks for minutes before answering is bounded by `timeout_s`
alone. Both timeout values and `max_tokens` must be positive. `max_tokens` is
sent under that name for Chat Completions and as `max_output_tokens` for
Responses, whose value is clamped to at least 16.

For the direct `api.openai.com` host, `cache_retention` other than `off` sends a
stable prompt-cache key for each forum/session/character. For GPT-5.6 and later,
`long` additionally requests implicit caching with the longest currently
supported minimum TTL, 30 minutes, from the Responses API. For `openrouter.ai`,
the same stable value is sent as `session_id` so successive requests retain
provider affinity. These cache fields are omitted for other hosts. `off` omits
CHA's cache hints but does not disable a provider's automatic caching.

Set `base_path` when a compatible provider exposes its API below a path rather
than at the host root. For example, OpenRouter uses `base_path = "/api"`, which
produces `/api/v1/chat/completions`.

A provider config is the only place connection and model settings may appear.
Each character selects exactly one of those configs in its own `character.toml`
with `provider = "<id>"`. Provider keys in workspace, forum-default, and member
configuration are ignored; there is no provider inheritance or override chain.
A missing character provider or provider config stops startup. Provider config
files are loaded when the committed configuration is materialized. To change
their host, model, prompt, or other hand-edited settings, use the offline
export/edit/import workflow below and restart CHA.

A character's chosen provider, reasoning effort, web search mode, and style can
be changed from the browser:
Characters → the character → the row naming it above the description → Settings. Save writes
those settings in the character's `character.toml` (under `characters/`, including
through any grouping directories) and restarts live sessions containing that
character. The built-in Assistant reads `system/assistant/character.toml` but
has no settings screen.

Character appearance is selected in the character definition with
`style = "<id>"`. The matching config lives at
`system/styles/<id>/config.toml`:

```toml
# characters/margaret/character.toml
style = "sans-bold"

# system/styles/sans-bold/config.toml
font = "sans"
weight = "bold"
```

Style configs may contain `font`, `style`, `weight`, `size`, and `text_color`;
omitted fields use the interface defaults. A style reference must resolve during startup.

Provider `reasoning_effort` and `web_search` values are defaults. A character's
`character.toml` may override them with `reasoning_effort = "low"`, `"medium"`,
`"high"`, or `"xhigh"`, and `web_search = "off"`, `"auto"`, or `"required"`.
Omitting either character key inherits the provider value.

`web_search` other than `off` normally requires `api = "responses"`. OpenRouter
also supports it with `api = "chat_completions"` through its server-side web
search tool. With `web_search = "auto"`, the model may search when the prompt
and turn warrant it; `required` forces a search tool call on every generation.
Other Chat Completions hosts cannot be selected by a character that enables
web search.

Search queries, progress, retrieved pages, annotations, and tool-call details
stay inside the provider interaction. Only the character's synthesized answer
text enters the transcript.

Provider secrets are managed on the Settings > API Keys screen and stored in
the active vault database under `system/keys/api_key_N/config.toml`. Provider
configs contain only the selected key ID; they never read model credentials
from the process environment or `.env`. Keys are vault-specific and are
included as plaintext TOML in workspace exports. OpenAI subscription
credentials live in `openai-auth.json` in the application configuration
directory.

Normal startup takes the application root for installed `web/` assets from the
executable directory, or from `--root`. Relative `data` and `logging.file` paths
resolve against the configuration directory; `mirror` and `modify` must be
absolute. Template includes resolve beneath the private materialized workspace.

### Command line and configuration maintenance

The complete public command interface is:

```text
chaweb --config=CONFIG_DIR [--root PATH]
chaweb --config=CONFIG_DIR --vault=NAME --import DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --export DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --upload
chaweb --config=CONFIG_DIR --vault=NAME --download
```

`--config` is mandatory and names the configuration directory. Server mode
opens the vault selected by `app.toml`. `--vault` is required for import,
export, upload, and download, and names the vault those commands act on. That
vault's `data` setting names the SQLite file containing sessions and workspace
metadata. Normal startup requires a valid schema-v2 database; a missing
database is created only by a successful import. For import, the configuration
directory must be outside the workspace source directory.

Import stores every regular workspace `.toml` and `.md` file, but explicitly
excludes `.env`, legacy root `app.toml`, and `workspace.toml`.
It follows no symlinks and stores no other file type. An included file must
therefore be in this set:
`$$(snippet.txt)` fails validation, while an appropriate stored
`$$(snippet.md)` can work. The `config` table contains only `(name, content)`;
one SQLite transaction replaces the complete small table, with no generation,
type, control, or revision metadata.

Import, export, upload, and download are mutually exclusive offline operations.
Stop CHA first; any of these commands fails immediately while runtime holds the
database lease. To edit configuration:

1. Stop CHA.
2. Export to a missing or empty private directory.
3. Edit that directory.
4. Import it back into the same database.
5. Restart CHA normally.

The source or exported directory is never consulted by normal runtime. CHA
materializes committed rows into one owner-private temporary tree. Supported
Settings edits, including model and R2 credentials, persist through SQLite
before publication.

The database, rollback journal, WAL/SHM sidecars, companion lock, private
runtime tree, workspace exports, legacy configuration-directory `.env` and
`api-keys.json`, and `openai-auth.json` must remain accessible only to their owner.
CHA enforces this for files it manages. Naively copying a live WAL database is
unsafe; the R2 commands acquire the database lease, and upload checkpoints the
WAL before transferring the main database file.

R2 transfer credentials are configured per vault under Settings > API Keys >
R2 storage. They are stored with the other vault keys and therefore travel in
exports and in the uploaded database. For migration, an empty vault can import
the following inherited or configuration-directory `.env` values once:

```text
CHA_R2_URL=https://ACCOUNT_ID.r2.cloudflarestorage.com/BUCKET
CHA_R2_ACCESS_KEY_ID=ACCESS_KEY_ID
CHA_R2_SECRET_ACCESS_KEY=SECRET_ACCESS_KEY
```

The bucket URL may optionally end with `/`.

R2 uses two object keys derived from the configured database filename. For
example, `data = "/var/lib/cha/workspace.sqlite3"` uses `workspace.sqlite3`
and `workspace.sqlite3.toml` in the configured bucket. `--upload` validates the
schema-v2 database and its vault definition, then overwrites the vault object
followed by the database object. R2 cannot atomically replace the pair; retry a
failed upload before downloading. `--download` fetches and validates both files
before replacing either, and keeps their previous versions with `.bac`
appended. Buckets written by an older database-only upload require a current
upload before they can be downloaded. R2 requests use its S3-compatible API
over HTTPS.

An invalid import does not change an existing database. Failed v1 upgrade
leaves valid v1; failed v2 replacement leaves the previous complete config. A
runtime settings failure before commit restores the materialized tree and
leaves durable and published state unchanged. In the rare case that SQLite
commits but publication fails, stop and restart CHA: startup loads the new
committed state. A failed export never changes the database but may leave an
incomplete destination; empty it before retrying.

Schema v1 is the earlier unified session database without the `config` table.
Only import upgrades it to v2, preserving all sessions. Runtime and export
reject v1 with an import instruction. Before modifying a database, import also
scans `forums/*/sessions/` and `forums/*/sessions/deleted/` for older
per-session databases. If the target is missing, use the archived
migration-capable CHA build first. If the target exists, verify that migration
and remove the legacy copies from the disposable/source tree. This build never
migrates those old files itself.

### Operator cutover from schema v1

Always rehearse this procedure on recoverable copies before changing an
installation:

1. Stop the old service. Make safe offline backups of both its configuration
   directory and v1 `workspace.sqlite3`; retain them after the new release is
   accepted until the operator chooses a retention date.
2. Inspect `forums/*/sessions/` and `forums/*/sessions/deleted/`. If either
   contains regular `.sqlite3` files, first use the archived migration-capable
   build on a disposable copy, verify the unified v1 database, and finish legacy
   cleanup there. The new import intentionally refuses both incomplete states.
3. Create one configuration directory. Put `vault`, `[web]`, and `[logging]`
   in `app.toml`, and put `vault_name` plus `data` in a vault TOML file.
4. Run the import and inspect its file-count summary:

   ```sh
   chaweb --config=/absolute/path/cha-config --vault=Personal \
          --import /absolute/path/workspace
   ```

5. Verify schema version 2, session counts/content, required config rows, and
   owner-only database, lock, and sidecar permissions.
6. Export to a new directory, then compare every accepted file byte-for-byte
   with the source:

   ```sh
   chaweb --config=/absolute/path/cha-config --vault=Personal \
          --export /absolute/path/exported-workspace
   ```

7. Move the original configuration directory aside and start only with:

   ```sh
   chaweb --config=/absolute/path/cha-config
   ```

   Open and resume a session, exercise the three narrow settings, restart, and
   export again.
8. While runtime holds the lease, confirm both import and export are rejected.
   After stopping it, import a second valid configuration and confirm all
   sessions remain unchanged.

Do not delete the old backup as part of the cutover. Backup retention and
eventual removal are operator decisions.

The Linux package contains `cha-config.example/` and `import-seed/` as source
material, not live storage. Copy the example directory to `../cha-config` and
initialize the configured database explicitly with
`chaweb --config=../cha-config --vault="Personal" --import import-seed`
before running `start-cha.sh`. The real configuration and database remain
outside the replaceable application directory; the launcher writes process
output to `chaweb.log` beside the executable.

There is no automatic migration from a single `cha.toml`. To move an existing
installation:

1. Create a configuration directory. Put selection and web/logging settings in
   `app.toml`; put data, mirror, and modify paths in a vault TOML file.
2. Adjust relative paths for the new base directory. For a root database this
   commonly changes `data = "cha.sqlite3"` to `data = "../cha.sqlite3"`; the
   database itself does not move.
3. Move `.env` and `api-keys.json` to the configuration directory when they
   contain legacy credentials. Each empty vault imports them the first time it
   is opened; the legacy files are left unchanged.
4. Move `<database>.openai-auth.json` to
   `<config-directory>/openai-auth.json`, preserving private permissions, or
   sign in again.

R2 object keys still come from database filenames; the companion vault object
adds `.toml`.

`CHA.app` performs its own setup. On first launch it creates an empty Default
vault. Add model credentials from Settings > API Keys and select one in the
provider's Credentials field. Later launches reuse the conversations and
settings already on that Mac, so replacing `CHA.app` upgrades the application
without removing them.

`chaweb` loads discovery — the roster, descriptions, and Markdown shown in
Personas, Characters, and Forums — from the database as one validated immutable
workspace. Runtime values own their parsed data eagerly; normal reads never
reopen the materialized files.

Startup validates every configured forum, not only the ones in use. A forum with
an invalid default character, member override, or prompt therefore prevents the
server from starting; the reported error names that forum and its source.

## Build and test

Native configuration uses Windows CNG on Windows and requires OpenSSL
development headers and libraries on other platforms. The Ninja preset fetches
the other vendored dependencies when needed.

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make web-check
make web-e2e
# Requires configured live-provider and R2 credentials:
make itest
```

The R2 integration case overwrites the dedicated
`cha-r2-integration-test.sqlite3` object in the configured bucket. Run only
that live round trip with:

```sh
build/ninja/itest --gtest_filter=R2Integration.*
```

Browser development is documented in
[webapp/README.md](webapp/README.md). Build a validated release with
`make package-linux VERSION=<version>` or
`make package-macos VERSION=<version>`. The Linux command writes an application
directory and `.tar.gz` archive under `packages/`; the macOS command writes
`packages/CHA.app` and `packages/CHA-macos-<version>.tar.gz`. The macOS archive
contains only `CHA.app`; the app creates its configuration and database on first
launch. Packaging requires
the Node.js version in `webapp/.node-version`, including npm and npx.

The macOS package must be built on an Apple Silicon Mac with the Xcode command
line tools. It links Homebrew's static OpenSSL, which is built for the host
macOS generation, so the application runs on the macOS generation that built it
or newer; building for an older Mac means building on one. It is signed ad hoc
rather than notarized, so a copy that arrives through a browser is quarantined:
open it once from the Finder shortcut menu, or remove the quarantine attribute.
See [src/README.md](src/README.md) for the native architecture.
