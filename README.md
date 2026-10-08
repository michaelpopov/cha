# CHA

CHA is a C++20 desktop application for chatting with OpenAI-compatible model
servers. The supported products are macOS (WKWebView) and Windows (WebView2).
Domain calls use a native bridge; there is no application HTTP listener.

## Start chatting

On macOS, build and launch the packaged app, or use native development with
Vite serving only frontend assets:

```sh
make package-macos VERSION=0.0.0-dev
make run-native-dev CONFIG=/path/to/cha-config
```

CHA starts in the process-local **Entrance / Welcome** conversation as **Guest**
with **Assistant**. Inspect forums, create or reopen a stored session, and
select a forum character. Which persona you speak as follows the forum you are
in and is set in that forum's configuration.

Welcome is private to the running application and is deleted on shutdown. All
stored sessions and workspace metadata remain in the SQLite database selected
by the active vault.

The chat input also accepts these controller-level commands:

| Command | Purpose |
| --- | --- |
| `/mcast` | Send one prompt to multiple forum characters. |
| `/clear` | Remove all messages and saved history from the current session. |

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

## Sessions and voice

**New session** opens a conversation immediately. After its first message, CHA
uses the built-in Assistant's provider to generate a short title. The session
appears in Recent after naming finishes, even if naming fails. Navigating away
from an unused new session discards it; submitting or renaming keeps it.
Press **Enter** to send and **Ctrl+Enter** to insert a line break. The sidebar
and composer dividers can be resized with the mouse or arrow keys.

Settings → Voices → Voice settings configures OpenAI or xAI dictation and
FishAudio output separately. The hands-free send phrase defaults to
`over to you`; saying it sends the draft and keeps dictation active. Microphone
capture pauses during speech playback. Message controls copy text, show token
usage, play audio, and remove completed errors or turns. See the
[user manual](docs/UserManual.html) for controls and audio caching.

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
startup vault and holds logging settings. An obsolete `[web]` section is
ignored with a warning. Each other `.toml` file is one vault and supplies that
vault's data paths:

```text
cha-config/
├── app.toml
└── personal.toml
```

```toml
# app.toml
vault = "Personal"
mirror = "mirror"
modify = "modify"

[logging]
file = "logs/cha.log"
level = "info"
```

```toml
# personal.toml
vault_name = "Personal"
data = "/var/lib/cha/workspace.sqlite3"
protected = false
```

The optional `app.toml` `mirror` setting names a base directory. CHA appends
the vault name and writes persistent sessions as Markdown under display-named
forum directories; omit it to disable mirroring. Protected vaults never mirror.
The base directory must already exist. Sessions are refreshed after terminal
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
files are loaded when the committed configuration is materialized. Change
connection and model settings in Settings → Providers, or use the
export/edit/import workflow below for filesystem edits. Import reloads the
workspace in the running application.

A character's chosen provider, reasoning effort, web search mode, and style can
be changed in the desktop interface:
Characters → the character → the row naming it above the description → Settings. Save writes
those settings in the character's `character.toml` (under `characters/`, including
through any grouping directories). Provider, reasoning, and search changes
reload affected live sessions; style and voice changes refresh presentation
without stopping generation. The built-in Assistant reads
`system/assistant/character.toml` and
has its own character Settings screen.

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
`character.toml` may override them with `reasoning_effort = "none"`,
`"low"`, `"medium"`, `"high"`, or `"xhigh"`, and `web_search = "off"`, `"auto"`, or `"required"`.
Omitting either character key inherits the provider value.
A provider without `reasoning_effort` uses `"none"`. The legacy `"minimal"`
value normalizes to `"low"`; unsupported provider values warn and use the
default. Direct OpenAI Responses requests omit reasoning settings for models
that do not support reasoning.

`web_search` other than `off` normally requires `api = "responses"`. OpenRouter
also supports it with `api = "chat_completions"` through its server-side web
search tool. With `web_search = "auto"`, the model may search when the prompt
and turn warrant it; `required` forces a search tool call on every generation.
Other Chat Completions hosts cannot be selected by a character that enables
web search.

These provider-hosted settings are separate from Settings → **Search API**:

- **On-demand web search** lets the answering model call CHA's `web_search`
  function through Responses or Chat Completions. It does not require Jev.
  Characters can override the workspace default with `web_search_tool = true`
  or `false`. Each answer allows up to four web tool-call attempts, or eight
  shared between search and reading when page reading is enabled.

**Page reading provider** selects Firecrawl for the model's
`web_read` tool, which returns page content as Markdown. Select the service's
saved API key in Search API, or choose Off to disable reading. Page reading
works independently of Brave/Tavily search; `web_search_tool = false` disables
both on-demand tools for a character.

Save the search or reading service key in API Keys, then select it in Search API.
Search results, page content, and tool calls stay outside transcript history;
replies show a web-source indicator. See the [maintainer guide](docs/MaintainerGuide.md#recipient-detection-and-search-api)
for exported settings and failure behavior.

Provider secrets are managed on the Settings > API Keys screen and stored in
the active vault database under `system/keys/api_key_N/config.toml`. Provider
configs contain only the selected key ID; they never read model credentials
from the process environment or `.env`. Keys are vault-specific and are
included as plaintext TOML in workspace exports. OpenAI subscription
credentials live in `openai-auth.json` in the application configuration
directory.

Native hosts load packaged frontend assets themselves. Relative `data`,
`logging.file`, and application-level `mirror` and `modify` bases resolve
against the configuration directory. CHA appends the vault name to mirror/modify
bases; obsolete per-vault values are ignored with warnings. Template includes
resolve beneath the private materialized workspace.

### Command line and configuration maintenance

The application takes one command-line option:

```text
CHA --config=CONFIG_DIR
```

`--config` is mandatory and names the configuration directory. Launch opens the
vault selected by `app.toml`. Import, export, upload, and download are
maintenance operations performed inside the running application and act on the
active vault from Settings → Vaults → the active vault. Import and export
require the application-level `modify` base to be configured. The vault's
`data` setting names the SQLite file containing sessions and workspace
metadata. The runtime requires a valid schema-v2 database; native first-run
setup and New vault create one before opening it.

Import stores every regular workspace `.toml` and `.md` file, but explicitly
excludes `.env`, legacy root `app.toml`, and `workspace.toml`.
It follows no symlinks and stores no other file type. An included file must
therefore be in this set:
`$$(snippet.txt)` fails validation, while an appropriate stored
`$$(snippet.md)` can work. The `config` table contains only `(name, content)`;
one SQLite transaction commits the configuration changes, with no generation,
type, control, or revision metadata.

Each of these operations runs as global vault maintenance: the application
closes admission, releases every live controller and its journal connection,
performs the storage operation, then reopens admission under a new context
epoch. To edit configuration:

1. Export to the vault's `modify` directory.
2. Edit that directory.
3. Import it back into the same database.

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

R2 transfer credentials are configured per vault under Settings → API Keys →
R2 storage: bucket URL, access-key ID, and secret key. They travel in exports
and uploaded databases. R2 credentials are not read from `.env` or the process
environment. The bucket URL may optionally end with `/`.

R2 uses two object keys derived from the configured database filename. For
example, `data = "/var/lib/cha/workspace.sqlite3"` uses `workspace.sqlite3`
and `workspace.sqlite3.toml` in the configured bucket. Upload validates the
schema-v2 database and its vault definition, then writes the database object
followed by its companion TOML. The companion uses a portable database filename.
CHA records the returned R2 ETag and checks the remote version before later
uploads; an unknown or changed version requires overwrite confirmation.
R2 cannot atomically replace the pair; retry a failed upload before downloading. Download fetches and validates both files
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

### Native setup and older installations

Packaged macOS and Windows hosts initialize their configuration directory with
an empty Default vault and the shared seed configuration. Connect ChatGPT in
Settings → OpenAI, or add model credentials in Settings → API Keys and choose
them in a provider. Later launches reuse the existing database.

The seed and example configuration live under `packaging/shared/`. Existing
schema-v1 and legacy per-session databases need a separately planned migration;
the desktop executable accepts only `--config`, not old `--import`, `--export`,
or `--vault` maintenance flags. Use the running application's vault actions for
current configuration changes. See the [maintainer guide](docs/MaintainerGuide.md)
for path layout, import validation, and legacy migration constraints.

`CHA` loads discovery — the roster, descriptions, and Markdown shown in
Personas, Characters, and Forums — from the database as one validated immutable
workspace. Runtime values own their parsed data eagerly; normal reads never
reopen the materialized files.

Startup validates every configured forum, not only the ones in use. A forum with
an invalid default character, member override, or prompt therefore prevents the
application from starting; the reported error names that forum and its source.

## Build and test

Native configuration requires OpenSSL development headers and libraries on all
platforms. Windows needs static OpenSSL built for the matching MSVC runtime;
the package uses the static CRT. macOS uses bundled curl 8.22.0 with OpenSSL,
WebSocket support, and Apple SecTrust certificate verification; other platforms
use curl 8.14 or newer, or the bundled fallback.
CMake fetches other vendored dependencies when needed.

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make web-check
make itest-local
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
`make package-macos VERSION=<version>` on macOS or
`make package-windows VERSION=<version>` on Windows. On Linux,
`make package-linux VERSION=<version>` (or `make package=linux VERSION=<version>`)
creates `packages/cha-linux-<version>.tar.gz` with `cha-daemon`, the ChaWeb
browser files, an example vault, and deployment scripts for nginx and systemd
(see [packaging/linux/README.md](packaging/linux/README.md)). The macOS command writes
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
