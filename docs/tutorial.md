# Learning the CHA C++ codebase

This guide follows a conversation from the native desktop window through the
application, shared session runtime, provider worker, and database. It also
explains how workspace edits and vault changes reach the React interface.

## 1. How to use this guide

Read the public headers before their implementations. Start with the object and
build maps below, then follow the domain, storage, and generation sections.
Sections 12–18 connect those pieces to the native bridge and lifecycle. Use the
tests in section 19 to check an assumption before changing code.

The [maintainer guide](MaintainerGuide.md) covers workspace files and operational
recipes. [Editing workspace entities](editing.md) covers adding an editing flow.
The README in each `src/` directory summarizes that directory's current
responsibility and dependencies; headers and tests remain authoritative for
detailed behavior.

## 2. The application in one page

CHA is a native desktop application with a C++20 backend and a React interface
inside WKWebView on macOS or WebView2 on Windows. The host and backend run in
one process. Packaged assets and media use native resource handling; application
commands and session events use a message bridge. There is no application HTTP
listener. Provider requests, OAuth, R2, and speech services still use outbound
network connections, including SSE decoding for streaming model responses.

```text
React interface (webapp/)
    -> native host message adapter
    -> BridgeRouter
    -> Application
         -> WorkspaceConfigStore -> immutable Workspace snapshot
         -> SessionRepository -> vault SQLite database
         -> LiveSessionManager -> SessionRuntime (one session thread)
                                    -> live controllers -> SessionJournal
                                    -> Providers -> ProviderRequest workers
         -> media jobs and resources

SessionRuntime -> LiveSession endpoint -> SessionOutput
    -> BridgeRouter delivery -> React projection
```

`Application` is the composition root and lifecycle boundary. It owns the
configuration store, session repository, live-session manager, provider
supervisor, keys, OAuth, mirroring, and media services. Operations needing the
active vault take the caller's explicit, nonzero context epoch. A stale epoch
cannot silently target the newly selected vault.

`WorkspaceConfigStore` owns the published workspace for that application.
`snapshot()` returns a `shared_ptr<const Workspace>`; readers hold it while
using references into the snapshot. Settings and workspace operations receive
the workspace explicitly. There is no process-global current workspace.

One `SessionRuntime` thread owns all live controllers, the session map, and the
selected session. Only that thread calls `SessionController`; other threads
submit commands or enqueue generation results. `LiveSessionManager` is the
admission facade, and `LiveSession` is a stable endpoint that can outlive its
controller. Controllers retain stable IDs and consult their injected workspace
source. Each provider request owns the resolved configuration and history with
which it started and runs on its own worker.

There are three distinct kinds of stored data:

- External configuration selects vaults, database paths, mirror/modify bases,
  and logging. It also contains the process-wide OAuth credential file.
- The selected vault database holds authoritative workspace configuration,
  saved API/R2 keys, persistent sessions, and cached audio.
- A private temporary tree materializes configuration for loading and holds
  the temporary Welcome database. Export directories and Markdown mirrors are
  projections, not runtime sources of truth.

## 3. Build graph and dependency direction

[CMakeLists.txt](../CMakeLists.txt) defines the current targets:

| Target | Role |
| --- | --- |
| `cha_lib` | Every production source: domain model, providers, storage, workspace, `Application` and its operations, live sessions, presentation, mirroring, audio, and the message protocol, routing, and operation dispatch |
| `cha_macos_runtime` | macOS shared library exposing the C runtime ABI to the Swift host |
| `cha_windows_app` | Windows native host and runtime integration |

```text
macOS host -> cha_macos_runtime -> cha_lib
Windows host -------------------> cha_lib
```

The directory `src/runtime/` holds the live-session runtime, the chat-input
grammar, and the wire DTOs. Its types live in namespace `cha`.

Core session behavior does not depend on the native host, JSON routing, or
React. The bridge adapts messages to application operations; platform hosts
own WebView integration and platform UI work.

### Build and test commands

From the repository root:

```sh
make build
make test
make web-check
make web-stage
```

`make web-check` checks generated DTO types, TypeScript, and frontend tests.
`make web-stage` builds and stages frontend assets. For a focused C++ run:

```sh
ctest --test-dir build/ninja -R '^Application' --output-on-failure
```

The CMake presets also include `asan-ubsan` and `tsan`. The credential-dependent
provider integration executable is separate from the ordinary unit suite.
See the root [README](../README.md) and [Makefile](../Makefile) for platform
prerequisites and packaging commands.

On macOS, `make run-native-dev CONFIG=/absolute/path/to/existing-config-directory`
opens the native development host with Vite assets. Vite serves frontend files;
it does not replace the native application API. An ordinary browser tab cannot
supply the injected native bridge.

## 4. Repository map

| Location | Responsibility |
| --- | --- |
| [src/app](../src/app) | Composition, lifecycle, context admission, workspace/settings/media/vault operations |
| [src/bridge](../src/bridge) | Native envelopes, flow control, subscriptions, dispatch |
| [src/chat](../src/chat) | IDs, personas, character metadata, transcript vocabulary |
| [src/util](../src/util) | Queues, templates, logging, filesystem and text helpers |
| [src/characters](../src/characters) | Character configuration and model-context projection |
| [src/providers](../src/providers) | Request workers, outbound transport, protocol decoding |
| [src/session](../src/session) | Controller state, controller opening, Markdown formatting, mirroring |
| [src/storage](../src/storage) | SQLite access, journal, database, lease, repository |
| [src/media](../src/media) | Audio downloads, transient media resources, cleanup |
| [src/workspace](../src/workspace) | Configuration store, workspace loading, built-ins |
| [src/runtime](../src/runtime) | Live-session runtime, DTOs, text-input grammar, projection |
| [webapp/src](../webapp/src) | React state, screens, native client, event projection |
| [resources/dto.yaml](../resources/dto.yaml) | DTO schema used to generate TypeScript types |
| [packaging/shared/import-seed](../packaging/shared/import-seed) | Example workspace configuration tree |
| [packaging/macos](../packaging/macos) | Swift host, shared C ABI, macOS packaging |
| [packaging/windows](../packaging/windows) | WebView2 host and Windows packaging |
| [tests](../tests) | Unit, wire-fixture, integration, and native-host tests |

## 5. The four concepts that unlock the code

### 5.1 Stable identity is not display text

IDs identify stored entities and bridge parameters. Names are presentation and
prompt text. A display-name edit must not change an entity ID or rewrite the
attribution saved in an old transcript entry.

### 5.2 One runtime thread owns every live controller

`SessionRuntime::Impl::run()` processes commands and provider events for all
live sessions. Controllers and journals are constructed, used, and destroyed
on that thread. Application callers enqueue commands; provider workers enqueue
events and wake the runtime. Thread confinement keeps controllers free of
internal locking without creating a thread for each session.

### 5.3 Workers receive copies, views stay local

`TranscriptView` and `ControllerView` borrow runtime-thread storage for immediate
projection. `ModelHistory` owns a point-in-time copy suitable for a worker.
Never retain a view into a transcript that may mutate.

### 5.4 Snapshots are authoritative

An append describes text growth at an exact transcript entry or reasoning
request. Structural changes need a snapshot. When coalescing cannot preserve a
valid append, publication falls back to a full snapshot. Delivery acknowledgments
bound outstanding work; the UI validates sequence and target before applying
an append.

## 6. First reading pass: domain vocabulary

Begin with [chat/ids.h](../src/chat/ids.h),
[chat/persona.h](../src/chat/persona.h), and
[chat/character_metadata.h](../src/chat/character_metadata.h).

The important split in character data is:

- `CharacterMetadata` is public, discovery-safe information: ID, display name,
  description, tags, and appearance.
- `CharacterDefinition`, declared in
  [characters/character.h](../src/characters/character.h), combines public metadata with
  private backend configuration and a completed system prompt.

Application discovery operations may expose metadata. They must not expose the full definition, which can
contain provider configuration and private prompt material.

### 6.1 The transcript is the shared conversation language

Read [chat/transcript.h](../src/chat/transcript.h) completely before
[chat/transcript.cpp](../src/chat/transcript.cpp).

`TranscriptEntry` is used by rendering, persistence, and model-context
projection. Its fields answer different questions:

| Field | Meaning |
| --- | --- |
| `id` | Monotonically increasing entry identity within the session |
| `kind` | Human, character, notice, or error semantics |
| `participant_id` / `display_name` | Who authored the entry |
| `addressed_to` / `addressed_to_name` | Target of a human prompt |
| `text` | Visible content |
| `status` | Complete, streaming, cancelled, or failed |
| `request_id` | Turn/generation correlation when applicable |

The `Transcript` class enforces several central invariants:

- Entry IDs increase strictly.
- At most one character entry is open for streaming.
- Human and notice entries are immediately complete.
- A completed or cancelled character entry has answer text.
- An error entry has failed status.
- A live streaming entry cannot be stored as a terminal record.

`Transcript::delete_turn()` accepts the entry ID of a saved character response
and removes every transcript entry with that request ID: the response and its
matching human prompt. It refuses streaming or unidentified entries. If the
deletion leaves no non-marker content below the active cover boundary, it also
clears that now-orphaned boundary.

The factory functions (`make_human_entry`, `make_character_entry`, and so on)
make valid intent visible at call sites. Validation still exists at boundaries;
factories are not a reason to trust arbitrary loaded data.

### 6.2 Revision and cover state

Two transcript concepts are easy to conflate:

- `revision` changes on presentation mutations.
- `covered_until` is an optional entry-ID boundary; earlier entries are omitted
  from model context.

`Transcript::cover()` adds a transient marker and sets the boundary immediately
after the selected entry, so the conversation through that entry is omitted
from later model requests. Selecting a different entry replaces the boundary;
it may move forward or backward. `Transcript::uncover()` adds its own marker and
clears the boundary, restoring the full conversation to model context. The
markers and boundary are not durable session history.

The version-2 SQLite schema still stores `history_epoch` so databases cleared
by older CHA builds restore the correct active history. The current application
does not expose transcript clearing and never advances the stored epoch.

Checkpoint: explain why `Transcript::model_history()` returns an owning value
while `Transcript::view()` returns a borrowed span.

## 7. Second reading pass: utility mechanisms

Read utilities as mechanisms with specific shutdown contracts, not as a bag of
helpers.

### 7.1 `ConcurrentQueue<T>`

[util/concurrent_queue.h](../src/util/concurrent_queue.h) is used for generation
events. Closing the queue stops new values but preserves accepted values for
draining. `close_with()` installs one final terminal value. That terminal
guarantee is how each generation execution reports exactly one completion,
cancellation, or failure even on its exception path.

### 7.2 `WakeNotifier`

[util/wake_notifier.h](../src/util/wake_notifier.h) is the tiny seam by which a
generation worker tells the shared session runtime, “new events may be
available.” Every live controller receives the same `OwnerWakeSignal`. It
remembers a wake that arrives before the runtime waits, so work cannot be
stranded between checking queues and sleeping. Waking does not carry events;
queues remain the source of work.

### 7.3 Configuration helpers

The other utilities support important boundaries:

- `path_name.*` keeps UTF-8 filesystem paths and identifiers explicit.
- `public_name.*` centralizes visible-name validation.
- `text_template.*` expands `$$(relative/file)` includes and `$${variable}`
  substitutions with containment and cycle/resource limits.
- `logging.*` owns the process logging lifetime.

Checkpoint: locate one caller of each utility and state whether it is a domain
policy or a reusable mechanism.

### 7.4 Configuration and native startup

The native host supplies a configuration directory and asset root to
`cha_runtime_create()`. The shared configuration parser bootstraps an empty
configuration directory with a Default vault; a nonempty directory must already
contain valid configuration. There is no shipped `chaweb` server executable.
Some command-parser maintenance options remain in the code, but the supported
desktop maintenance workflow uses the Database menu.

`app.toml` selects the startup vault and supplies optional directory bases and
logging settings:

```toml
vault = "Personal"
mirror = "mirror"
modify = "modify"

[logging]
file = "logs/cha.log"
level = "info"
```

Each other vault TOML file has its own name and database path:

```toml
vault_name = "Personal"
data = "personal.sqlite3"
protected = false
```

Relative paths resolve from the configuration directory. Mirror and modify
paths append the vault display name: the example uses `mirror/Personal` and
`modify/Personal`. Obsolete `[web]` listener settings and old per-vault
mirror/modify fields are ignored with warnings. Session runtime bounds and
deadlines live in [RuntimeSettings](../src/runtime/runtime_settings.h), separate
from this external configuration.

Vault-backed keys are included as plaintext in workspace exports. OAuth
credentials remain in `openai-auth.json` outside the vault database. A protected
vault uses SQLCipher; its password is supplied at launch or switch and retained
only in memory. Protected vaults are not mirrored. Enabling protection does not
delete previously written Markdown files.

See the [maintainer guide](MaintainerGuide.md) for path constraints, migration
inputs, backup considerations, and import/export recipes.

## 8. Third reading pass: startup and the immutable workspace

Read [application.h](../src/app/application.h),
[application.cpp](../src/app/application.cpp), and the shared
[runtime bridge](../packaging/macos/runtime_bridge.cpp).

Startup connects these owners:

1. The host locates configuration and assets and obtains a password if needed.
2. `Application::open()` opens `WorkspaceConfigStore`, which holds the database
   lease, materializes committed rows, and loads its immutable workspace.
3. Vault-backed keys and process-wide OAuth support supply provider credentials.
4. `SessionRepository` receives explicit database/private paths, password, and
   a workspace-source callback. It creates the temporary Welcome database.
5. Optional mirroring is initialized; a mirror failure is logged without
   preventing use of the vault.
6. `LiveSessionManager` starts the shared `SessionRuntime` with an opener bound
   to this application's store, repository, and provider supervisor. Audio
   services use the same repository.
7. The native host runtime creates `BridgeRouter` and its processing threads.
   The host creates a document connection and installs the frontend bridge.
8. The frontend checks `bridge.info`, then obtains bootstrap state, capabilities,
   roster, and context epoch through `app.bootstrap`.

The host owns the runtime. The application owns the database-related services;
live controllers must release their journals before the store releases its lease
and temporary tree.

### 8.1 What `Workspace::load()` builds

[workspace.h](../src/workspace/workspace.h) and
[workspace.cpp](../src/workspace/workspace.cpp) define a fully loaded,
validated configuration snapshot. The loader reads personas, characters,
forums, providers, styles, voices, prompts, and references, then adds built-in
Guest, Assistant, and Entrance data. It resolves all forum members, including
forums that have not been opened.

Normal readers use eagerly owned values, not the original import directory.
A settings edit creates a new candidate workspace. Previously returned
snapshots remain valid until their readers release them.

### 8.2 Database authority and publication

[WorkspaceConfigStore](../src/workspace/workspace_config_store.h) owns the
published `shared_ptr<const Workspace>`. Hold one `snapshot()` result for an
operation and pass `const Workspace&` into the settings/workspace helpers that
need it. Repositories and controllers receive a workspace-source callback tied
to their application; constructing a test workspace does not publish global
process state.

The schema-v2 `config` table stores `name` and `content`. Import collects
regular `.toml` and `.md` workspace files without following symlinks. It
excludes root process-configuration files and every other extension. Template
includes must therefore be part of the accepted row set.

An ordinary configuration mutation edits the private tree, loads a complete
candidate, collects rows, commits them in one SQLite transaction, and only then
publishes the candidate. Failure before commit retains the old workspace.
Runtime entity editors and explicit Database Import use this storage boundary;
editing an exported directory alone changes nothing in the running application.

### 8.3 Provider selection

Every connection and model setting lives in
`system/providers/<id>/config.toml`. Each character selects exactly one of
those configs with `provider = "<id>"` in its own `character.toml`. Workspace,
forum-default, and member provider keys are ignored, so there is no provider
inheritance or override chain. The `[prompt]` scope still merges across the
character, forum-default, and member layers.

Read the provider loader in
[workspace/workspace.cpp](../src/workspace/workspace.cpp).
`WorkspaceProvider` stores the fully resolved `ModelBackendConfig` directly;
`Workspace::character_definition()` copies it into the request-owned value
passed to provider code.

The provider screen is deliberately not a generic editor for every
`ModelBackendConfig` field. It shows the editable display name, Model, Base URL,
API format, and Credentials. Base URL is split back into `host` and
`base_path` while retaining its scheme and non-default port. Saves force
network mode and streaming, clear provider-level `reasoning_effort`, and set
`web_search = "off"`. Characters own the user-facing reasoning and web-search
choices.

Creating a provider starts either from the small default OpenAI configuration
or from a selected existing provider. Copying carries every provider setting,
including the selected saved-key ID, and changes only the display name and new
stable provider ID. The user can review and test the result before assigning it
to a character.

Credentials is an explicit choice. A normal provider refers to a secret in the
active vault's `ApiKeyStore`; the value never enters a frontend response but is
included as plaintext in workspace exports. `OpenAI OAuth` selects
`openai_subscription` authentication
and therefore requires the Responses API and the exact
`https://chatgpt.com/backend-api/codex` base URL. `No credentials` clears both
forms of authentication. The legacy field spelling `api_key_env = "Name"` is
accepted only as a lookup by exact display name among the active vault's saved
model keys; the process environment is never consulted for model access. A
missing or ambiguous name
fails when that provider is used. A resolvable legacy name appears as the
matching saved key in the editor and is written as the normal opaque `api_key`
ID when the provider is saved.

The provider `Test` action also stays narrow. The frontend sends the current
candidate as the same `ProviderUpdate` used by Save, and the application calls
`ProviderClient` directly with one synthetic empty-history request asking for
`OK`. It does not persist the candidate, reload live sessions, start a live
session, create a transcript, or involve the `Providers` supervisor. The probe
forces the real network path, turns web search off, uses fixed 10-second overall
and idle timeouts, and treats a completed provider response as success.

### 8.4 Prompt construction

`Workspace::load()` combines:

- the character definition prompt;
- forum prompt/context;
- the forum's default persona and its `PERSONA.md` prompt;
- standard generated context;
- effective model backend settings.

`FORUM.md` has two audiences. It is the forum prompt above, and
`Workspace` also reads it verbatim and serves it through
`forum.get` as the forum's description. Write it for readers as
well as for the characters. This differs from `CHARACTER.md`, which publishes
only its `<character_profile>` section: a forum publishes the whole file. The
raw template source is served, so `$${character.display_name}` and `$$(...)`
includes appear literally — a description belongs to the forum rather than to
any one member, so there is nothing to expand it against.

The built-in Assistant and Entrance data are assembled by `Workspace::load()`.
The generated workspace guide is combined with public workspace inventory
data. The Entrance/Welcome session is a normal session at the controller level;
its specialness is in how the application constructs and stores it.

Checkpoint: starting with `packaging/shared/import-seed/forums/stoics`, identify the files that
contribute to one member's final definition and the order in which values win.

## 9. Fourth reading pass: session storage and opening

Read [session_repository.h](../src/storage/session_repository.h),
[session_database.h](../src/storage/session_database.h),
[workspace_session_database.h](../src/storage/workspace_session_database.h),
and [session_open.cpp](../src/session/session_open.cpp).

### 9.1 Public identity and storage identity

`(forum_id, session_id)` is the durable public identity carried by the bridge.
Session IDs are unique within a forum. `session_key` is the internal SQLite
integer used to scope every journal and restore query. Request IDs and entry
IDs repeat across sessions, so omitting `session_key` from a query is a data
isolation defect.

`StoredSession` describes a listing. `PreparedSession` is validated construction
input: identity, label, database path/password, internal key, and restored
transcript/counters. A listing can become stale; `prepare()` must validate again
when the runtime opens the session.

### 9.2 Lease, connections, and transactions

`WorkspaceConfigStore` holds a non-blocking lease on the database's companion
`.cha-lock` file. The file remains stable while database files are replaced or
WAL sidecars come and go. Its existence alone does not mean the database is
busy; the held operating-system lock does.

The repository uses short-lived connections. Each live controller owns a
journal connection confined to the shared session runtime thread. Connections
enable foreign keys; write transactions use `BEGIN IMMEDIATE`. Reading under
a deferred transaction and later upgrading it can fail with
`SQLITE_BUSY_SNAPSHOT` even with a busy
timeout. Read-only restoration uses a consistent read snapshot.

Normal runtime requires schema v2. The import implementation still contains
schema-v1 upgrade and legacy per-session-database detection. These storage
helpers do not imply that the removed command-line executable is available;
plan any old-database migration separately before attempting a native launch.

### 9.3 Listing, deletion, and Welcome

Listing is an indexed database query, filtered by the current workspace's
forums. Creation validates the forum and inserts a session transactionally.
Session deletion archives its row; archived sessions disappear from ordinary
lists and cannot reuse their public identity. Repository maintenance may purge
archived rows.

Forum synchronization only inserts missing forum rows. Explicit configuration
import also prunes sessions for removed forums. A forum removal is therefore
not a reversible way to hide its conversations. Review forum IDs before an
import; see the maintainer guide's normalization rules.

Welcome uses a private temporary database with the same journal and restore
machinery as persistent sessions. It disappears with the application's private
runtime tree. Markdown mirroring excludes it.

### 9.4 Constructing the controller

`open_session()` takes the repository, identity, provider supervisor, wake
notifier, and configuration store explicitly. It holds a store snapshot while
finding the forum, prepares stored history, and constructs a controller with
the store's workspace-source callback. It also supplies callbacks for default
character persistence and cached-audio lookup.

The application installs the mirror callback and gives this opener to the
live-session manager. Only the shared session runtime thread constructs, uses,
and destroys the controller and journal.

## 10. Fifth reading pass: the generation pipeline

Read the interfaces before the implementations:

1. [providers/generation_event.h](../src/providers/generation_event.h)
2. [characters/model_context.h](../src/characters/model_context.h)
3. [providers/model_backend.h](../src/providers/model_backend.h)
4. [providers/providers.h](../src/providers/providers.h)
5. [providers/chat_completions_api.h](../src/providers/chat_completions_api.h)
6. [providers/responses_api.h](../src/providers/responses_api.h)
7. [providers/provider_client.h](../src/providers/provider_client.h)

### 10.1 The backend seam

`ModelBackend` has a two-phase contract:

- `prepare(const GenerationRequest&)` projects context and creates owned request
  bytes.
- `perform(RequestPayload, delta sink, cancellation flag)` performs synchronous
  generation and returns a classified terminal result.

The split lets request preparation fail on a worker while keeping the session
controller provider-agnostic. Tests inject fake backends through the same seam.

`GenerationDelta` distinguishes reasoning from answer text.
`GenerationResult` classifies completion, cancellation, protocol error, or
transport error. Worker-facing `GenerationEvent` adds the request ID and
converts the result into deltas followed by one terminal event.

### 10.2 Context projection

[characters/model_context.cpp](../src/characters/model_context.cpp) translates a
presentation-neutral `ModelHistory` into provider message roles.

Projection omits:

- notices and errors;
- the current open streaming entry;
- entries before the optional cover boundary;
- failed prompts;
- incomplete/cancelled character history.

For the target character, its own completed output becomes `assistant` history
and directly addressed human prompts become `user`-role history. Other
participants' entries are grouped into explicit shared-history JSONL so the
model can see the multi-party conversation without being told it authored
someone else's words. The system prompt names the same JSONL heading and tells
the character to use the block as earlier forum conversation context. The new
prompt is appended last with its persona display name as the current message to
answer.

### 10.3 Provider requests and ordered generation

`Providers` is a process-owned supervisor, not a scheduler or cache. Each call
to `make_request()` creates a `ProviderRequest`, registers it, and launches one
detached worker. The request owns immutable character and history input, a
cancellation flag, an event queue, and a shared wake notifier. Its worker
creates a fresh backend and curl easy handle for that request.

Registration makes the detached work supervised: `Providers` retains the
request until its terminal event is published and its transport resources are
gone. Process shutdown closes admission, cancels active requests, and waits for
the registry and final diagnostic tails to quiesce. A session may release a
request without waiting for its provider I/O.

For multicast, the controller creates an ordered vector of independent request
handles that share one immutable history snapshot. Every request starts as soon
as it is admitted, but the controller drains only `foreground_index`. A later
request may finish first; its output remains buffered in its private queue until
the controller durably activates that target. This preserves concurrent
provider latency and deterministic transcript order without a provider-layer
batch abstraction.

Stopping cancels every request, discards non-foreground handles, and retains
only the current foreground request long enough to persist its terminal event.

### 10.4 Provider transport versus protocol semantics

`ProviderClient` owns:

- protocol selection and `/v1/chat/completions` or `/v1/responses` dispatch;
- headers/authentication, curl handles, status/content-type checks, logging,
  byte counts, and cancellation through curl's progress callback;
- test mode, which emits the prompt as answer text.

`chat_completions_api.*` and `responses_api.*` each own request encoding and
response meaning for one protocol. Read
[providers/chat_completions_api.cpp](../src/providers/chat_completions_api.cpp)
beside [providers/responses_api.cpp](../src/providers/responses_api.cpp):

- incremental SSE framing for streaming responses;
- JSON message extraction;
- reasoning-format interpretation;
- reasoning and answer delta emission;
- end marker, malformed response, and missing-answer classification.

The controller derives a stable prompt-cache key from forum, session, and
character identity. When caching is enabled, the protocol encoders send that
key as `prompt_cache_key` to the direct OpenAI host and as `session_id` to
OpenRouter. For GPT-5.6 and later Responses, long retention also requests
implicit caching with the longest currently supported minimum TTL, 30 minutes.
Other compatible hosts receive no cache-affinity field.

The protocol modules intentionally know nothing about curl, HTTP status, or
cancellation. `ProviderClient::perform()` decides the final outcome after the
transport completes and may add HTTP metadata to a decoder error.

Streaming needs state across arbitrary network chunks. One
`ChatCompletionsStreamDecoder` or `ResponsesStreamDecoder` is therefore created
per streaming request. It retains incomplete SSE framing, protocol completion,
token usage, and whether reasoning or answer text was received, then is
destroyed with that request. The common `StreamingResponseDecoder` interface
lets `ProviderClient` select the protocol without owning either protocol's
parsing state.

Checkpoint: describe what happens if a streaming response contains reasoning
but no answer, and identify which layer detects it and which layer converts it
into a transcript error.

## 11. Sixth reading pass: `SessionController`

The controller is the heart of the application. Read
[session/session_controller.h](../src/session/session_controller.h) first, then
read [session/session_controller.cpp](../src/session/session_controller.cpp) in
four groups:

1. construction, restoration, and `view()`;
2. prompt resolution and `start_generation()`;
3. command methods such as cover, uncover, delete turn, multicast, and stop;
4. generation-event `apply()` overloads and shutdown.

### 11.1 What the controller owns

One controller owns:

- one `SessionJournal` connection, scoped to a single `session_key`;
- the runtime-thread-confined `Transcript`;
- a borrowed reference to the process-owned `Providers` supervisor;
- stable forum/persona IDs used to look up the current `Workspace`;
- default-character and current-persona selection;
- next request and entry IDs;
- at most one active foreground response;
- at most one `ActiveGeneration`, containing ordered request handles.

The controller has no mutex. Its public mutation/view methods belong to the
shared session runtime thread. Thread-safe communication is isolated inside
request event queues, cancellation flags, and the wake mechanism.

`Workspace` is the forum roster and handle resolver. It centralizes exact,
normalized, and prefix matching plus ambiguity diagnostics, so prompt
submission, multicast, and default-character changes use the same rules.
`SessionController` keeps no copied roster; its style-override map stores only
style IDs and overlays the selected appearance when metadata is copied for a
request or presentation. The smaller `generation_status.h`,
`controller_view.h`, and `opened_session.h` headers are
boundary value types: they let application and bridge code observe or transfer
session state without gaining access to controller internals.

### 11.2 Starting a prompt

`submit_prompt()` resolves a character handle or uses the current default,
resolves the author ID against the workspace persona roster, copies current
`ModelHistory`, and delegates to `start_generation()`.

`start_generation()` is ordered carefully:

1. Allocate one request ID per target and build all owning
   `ProviderRequestInput` values from one shared history snapshot.
2. Install `ActiveGeneration` and reserve its ordered request list.
3. `activate_run()` persists the first started turn and human prompt
   transactionally.
4. Add that prompt to the transcript, install `ActiveResponse`, and require a
   presentation snapshot.
5. Call `Providers::make_request()` for every target. Each admitted request
   starts immediately and returns a handle whose queue ends in one terminal
   event.

This is the key commit boundary: provider work starts only after the foreground
prompt is durable and the controller has installed state capable of receiving
its result. Later multicast targets run immediately but are not made durable or
visible until they become foreground.

### 11.3 Response phases

`ActiveResponse::phase` moves through:

```text
waiting -> reasoning -> answering -> terminal
    \-----------------> answering -> terminal
```

Reasoning may continue after answer text has begun; the phase records the most
important visible structure, while reasoning text remains separate ephemeral
state.

- The first reasoning delta changes structure and requires a snapshot.
- Later reasoning growth can be a targeted append.
- The first answer delta creates the streaming transcript entry and requires a
  snapshot.
- Later answer growth can be a targeted append.
- Completion, cancellation, failure, target changes, and notices require a
  snapshot.

Reasoning is never written to the transcript or SQLite journal.

### 11.4 Terminal outcomes

On completion with answer text, the controller transactionally completes the
turn, marks the streaming entry complete, clears `active_`, and advances or
finishes the active generation.

Completion without answer text is treated as failure. On failure, an open
streaming response is discarded and a durable error entry replaces it.

Cancellation has two forms:

- If answer text exists, a cancelled character entry is stored with the partial
  text.
- If no answer exists, the turn is cancelled without a response entry; the
  prompt remains.

`request_stop()` only sets cancellation and returns. It does not block the runtime
waiting for provider threads. Later event-loop passes drain terminal events and
finish cleanup.

### 11.5 `ControllerUpdate` is an effect description

Read [session/controller_update.h](../src/session/controller_update.h) and
[session/controller_update.cpp](../src/session/controller_update.cpp).

A command/event returns both semantic results (notice, input consumed, session
ended) and a presentation-state effect:

- `NoStateUpdate`
- `SnapshotRequired`
- `TextAppend` to an entry or reasoning request

Merging is conservative. A structural change dominates an append, and
incompatible appends become a snapshot. This type lets the session layer
describe what changed without depending on bridge delivery.

Checkpoint: for first answer chunk, second answer chunk, completion, and
provider failure, state the transcript mutation, journal operation, and update
classification.

## 12. Seventh reading pass: application and native bridge

Read these boundaries in order:

1. [Application](../src/app/application.h)
2. [Application operations](../src/app/workspace_operations.h) and
   [settings operations](../src/app/settings_operations.h)
3. [Bridge protocol](../src/bridge/bridge_protocol.h)
4. [Bridge router](../src/bridge/bridge_router.cpp)
5. [Workspace dispatch](../src/bridge/workspace_dispatch.cpp) and
   [settings dispatch](../src/bridge/settings_dispatch.cpp)
6. [Session output](../src/runtime/session_output.h)
7. [Session runtime and manager](../src/runtime/live_session_manager.cpp) and
   [live session endpoint](../src/runtime/live_session.cpp)
8. [Frontend native bridge](../webapp/src/api/nativeBridge.ts) and
   [event projection](../webapp/src/api/nativeEvents.ts)

### 12.1 Application admission and context

`Application` serializes lifecycle-sensitive operations and delegates domain
work to workspace, settings, vault, and media operations. Context-bound methods
require an explicit epoch. Zero and stale epochs are rejected; zero is not a
request to use whichever vault is current.

`admit_locked()` is the shared decision, `require_admitted()` converts denial to
an exception for throwing methods, and `check_context()` exposes the same
check to other paths. They are different result forms, not different policies.
The underlying lifecycle mutex, active password, and unusable flag are private.

Application state is `running`, `maintenance`, `stopping`, or `unavailable`.
Bootstrap publishes the current epoch and capabilities. A context transition
invalidates pending work associated with the previous vault. Native file saves
also revalidate the captured epoch after a platform save dialog returns.

### 12.2 Request and delivery ownership

The host creates a connection identity for a particular native document. JSON
that merely names a connection does not create one. Requests carry that
identity, a request ID, context epoch, method, and parameters. The router checks
the trusted host identity and envelope before dispatching.

`BridgeRouter` owns connections, request accounting, deadlines, reply queues,
subscriptions, delivery IDs, and acknowledgments. Ordinary work and control
work have separate admission capacity. Slow generation or provider testing
must not consume the path needed to stop or clean up work.

Those bridge admission pools are separate from the session runtime's single
command queue. A session reply's ready callback only queues a router
completion; it must not synchronously call back into the session runtime.
Subscription replies carry the endpoint needed to obtain output, avoiding a
blocking manager lookup from that callback.

Synchronous workspace and settings operations live in noun-specific dispatcher
files. Each dispatcher returns an optional JSON result: no value means it did
not handle the method, while a JSON null is a handled empty result. Session,
media, and lifecycle work that needs asynchronous router state stays in the
router. Shared parameter parsing lives in `request_params.h`.

Only one delivery batch per connection is outstanding until acknowledged. A
reply timeout abandons observation of the result; it does not roll back a
mutation already running. Connection teardown releases subscriptions and
connection-owned resources.

### 12.3 Runtime commands, selection, and output

`SessionRuntime` owns one bounded queue containing web commands and manager
control operations. A web command captures its context epoch, full session
identity, and controller instance. Execution rechecks them: an old epoch fails
with `vault_changed`, and a retired or replaced instance fails with
`session_not_live`. A command cannot silently target a newly opened controller
with the same public identity. `OperationReply` and `CommandReply` support
asynchronous completion and ignore late completion after abandonment.

Each loop handles a bounded command batch, then a bounded event batch for each
running controller, then retirement. Event processing also runs for idle
controllers so terminal events such as `session_ended` are consumed. A full
batch causes another pass before sleeping on the shared wake signal.

The selected session stays live while idle. Selecting another session marks
the previous one for retirement once its entire generation, including
multicast, finishes and durable state is written. Selecting it again cancels
that retirement. Unsubscribing only detaches delivery; it does not cancel
generation or release a controller. Explicit Stop cancels generation, while
explicit session close requests controller shutdown.

Selection opens a candidate before committing the switch, preserving the
previous selection if opening fails or the request is abandoned before that
commit. Completed background sessions are cleaned up before checking capacity.
At the limit, an idle selected session can be replaced; a generating session
cannot be evicted just to make room.

Journal writes, history restore, snapshot construction, and mirroring remain
synchronous on this shared thread. Slow work there delays other sessions.
Provider workers continue independently, and their event queues remain
unbounded; the bounded runtime queue is not provider backpressure.

`ControllerView` borrows storage. `to_snapshot()` copies it immediately into an
owning `SessionSnapshot`. `SessionOutput` coalesces pending output and uses a
full snapshot when it cannot retain a correct append. No HTTP thread, SSE
mailbox, heartbeat, or browser takeover is involved.

The frontend projection checks connection, epoch, subscription, and session
identity before processing an event. A snapshot establishes state and the next
sequence. An append must match both that sequence and an existing target.
Duplicates are ignored; invalid current-scope data or a sequence gap fails the
projection and invokes recovery through a new subscription/snapshot. Late events
from a previous scope cannot corrupt the current session.

### 12.4 Text input

The frontend submits raw text; `LiveSession` supplies the current persona ID.
`text_mention.*`, `text_command.*`, `text_multicast.*`, and `text_input.*` resolve
mentions and `/mcast` into typed controller actions. Stop, cover/uncover,
default-character selection, and deleting a turn use typed operations.

This keeps text syntax outside the controller. The controller accepts domain
intent regardless of how a screen or text parser obtained it.

### 12.5 DTOs and validation

[resources/dto.yaml](../resources/dto.yaml) declares the DTO shapes used by
`schema.d.ts`. It is used for type generation, not as an HTTP route catalog.
Run `npm run api-types` in `webapp/` after changing it and keep generated output
in sync.

`bridge.info` checks the exact protocol version before bootstrap. It is a
mismatch check, not compatibility negotiation. It is useful during development
when Vite assets and a previously built runtime can differ.

TypeScript types do not validate JSON. The frontend retains runtime guards at
the bridge/result and event boundaries, shares repeated roster/appearance/forum
checks, and validates snapshot fields before rendering or append projection.
Avoid adding duplicate guards for unused shapes or regenerating a second
schema by hand. Guards accept supported values such as idle generation IDs and
nullable timestamps; additive response properties are allowed.

The C++ and TypeScript method policies are still declared separately. The
[method-policy fixture](../tests/fixtures/wire/native-method-policies.json)
and tests check control classification, epoch requirements, and context changes
across the boundary. A new method must update both policies and their fixture.

### 12.6 Frontend state and loading

[state/view.ts](../webapp/src/state/view.ts) keeps per-entity inspection records,
with shared shapes where appropriate. Remembered selections, the currently
browsed forum, and the active conversation have different lifetimes. Do not
collapse them into one global selection merely to reduce field count.

[state/entityUpdates.ts](../webapp/src/state/entityUpdates.ts) centralizes the
summary and visible-reference updates after entity edits. A persona rename can
update forum summaries and the current session's forum summary without
rewriting names recorded in historical transcript entries. Character settings
writability is separate from editing the character's definition, including for
built-in Assistant.

[useLoad.ts](../webapp/src/useLoad.ts) handles simple list loads: data, load error,
retry, and suppression of stale completion after a client change or unmount.
Pass a stable loader function. Screens retain mutation errors separately, and
complex detail/edit/subscription effects retain their specific behavior.

Detail screens own their loaded data and mutations. `EditableTitle` and
`DetailActions` in [DetailActions.tsx](../webapp/src/components/DetailActions.tsx)
provide rename controls and edit/upload/delete dialogs inside those screens.
`TopBar` only handles the sidebar toggle and navigation title; it does not load
or mutate entity details. Pending or failed session operations select the chat
view so their progress or error is visible.

[state/route.ts](../webapp/src/state/route.ts) always uses hash routes inside the
native document. After maintenance requires a reload, `reloadApplication()`
reloads the document; changing only its hash would leave the old bridge alive.

### 12.7 Markdown, audio, and native resources

`session_markdown()` renders both explicit session exports and continuous
mirrors. It omits transient cover markers and duplicate multicast prompts.
Live-session export gets a session-runtime snapshot; closed-session export
restores stored history. The native host selects a destination, then writes
through the context-checked runtime save operation off the UI thread.

`SessionMirror` writes private, atomic Markdown files under display-named forum
and session paths. It updates after terminal transcript changes or renames.
Failures are logged without undoing durable chat state. It is disabled for
protected vaults and excludes Welcome. Existing files, including archived
conversation copies, may remain after their source stops appearing in CHA.

`AudioDownloadManager` runs background synthesis jobs independently of screen
selection. Admission captures the relevant settings and entry identity;
repository checks prevent a deleted/changed entry or replaced database from
receiving stale audio. Cached clips are stored in `entry_audio` and survive
restart for persistent sessions. Job queues do not survive restart.

The frontend polls accepted jobs through `audioDownloads.ts`. Retrying an
operation must distinguish retryable transport/service failures from terminal
errors. Clearing the audio cache cancels session jobs and removes clips without
changing transcript text. Uncached voice previews use separate native speech
operations with bounded admission.

Media resources are scoped to the native connection and context. The hosts
serve native resource handles to the WebView; the frontend releases handles
when done. Model and speech credentials stay in native code rather than being
returned to the frontend.

## 13. End-to-end workflow traces

### 13.1 Opening and submitting to a session

1. The frontend creates or lists sessions through the typed native client.
2. `session.open` passes the captured epoch to `Application`.
3. The manager queues selection of the full session identity on the runtime.
4. The runtime reuses a live controller or prepares stored history and constructs
   one, then commits selection.
5. A subscription delivers an initial snapshot.
6. `session.submit` queues text for the shared session runtime thread.
7. The controller commits the foreground prompt, starts provider requests, and
   applies their events in order.
8. Owning snapshots/appends cross `SessionOutput`, router delivery, and the
   native adapter; the frontend acknowledges and projects them.
9. Terminal generation state is committed and the optional mirror is refreshed.
   An unselected session awaiting retirement can now release its controller.

### 13.2 Editing an entity

The screen submits a typed operation with its context epoch. `Application`
checks admission and passes explicit dependencies to the operation helper.
`WorkspaceConfigStore` validates and commits the candidate before publishing it.
The operation returns canonical data; the reducer updates summaries, inspection
state, and current visible references. Presentation-only changes refresh
affected live sessions. Changes requiring new generation configuration stop
all affected live controllers with `reloading`, including background sessions.
Existing provider requests retain their captured inputs until cancellation;
they are not mutated in place.

### 13.3 Switching or maintaining a vault

Vault switching validates the target and acquires its lease before disturbing
the current database. Application maintenance fences new work, coordinates
media, drains live sessions, reserves the configuration store and repository,
and releases handles needed for database work. The active lease remains held
during maintenance of that database.

Session deletion and global maintenance use one absolute reservation deadline
for queue admission, queue wait, and controller/journal release. Releasing or
timing out a reservation cancels its shared token and wakes the runtime; it
does not wait for queue space. Expired queued reservations cannot start later.
Context epoch publication is atomic and does not require a runtime round trip,
so timeout recovery can invalidate old queued work even while the runtime is
still blocked. A timeout does not undo shutdown already requested for a session.

Import replaces configuration from the derived modify directory; Export writes
that directory. Upload and Download transfer the database and companion vault
TOML through R2. Download validates staged files before replacing the local
pair, keeping `.bac` backups. Upload cannot atomically replace both remote
objects and must be retried after a partial failure.

After a successful cutover, storage is reopened, capabilities are refreshed,
and context publication invalidates old work. The shared session runtime stays
alive; sessions reopen on demand. The frontend refreshes or reloads for the new
context. A pre-commit validation failure leaves the old vault usable.
Failure to restore a usable database marks the application unavailable and
requires restart; it must not resume with missing storage.

Settings → Vaults can also create, rename, protect, remove definitions, download
an inactive vault, and merge configuration. Merge overlays configuration and
keys, not source sessions, and validates the combined workspace before commit.
See the maintainer guide for each operation's file and backup behavior.

## 14. State machines to keep in your head

### 14.1 Turn persistence

```text
               +-> completed + response
started prompt +-> cancelled + optional partial response
               +-> failed    + error entry
```

SQLite enforces at most one started turn and one prompt per turn. Every journal
transition is transactional. On startup, a leftover started turn means the
previous process was interrupted; restore creates an `InterruptedTurn`, and the
controller repairs it to failed before normal operation.

Deleting a saved response is transactional too: `SessionJournal::delete_turn()`
removes the prompt and response entries before removing their turn row. The
session's next request and entry counters keep advancing, so deleted IDs are
never reused.

### 14.2 Live controller and endpoint lifecycle

```text
starting -> running -> stopping -> finished
    \---- open failed/not found ----> finished
```

For an attached output, the runtime constructs and publishes a terminal
snapshot before closing output. It then shuts down and destroys the controller,
releases its journal, and publishes `finished`. The endpoint and its owning
output can outlive that cleanup for in-flight delivery. No per-session thread
remains to join, and retirement does not wait for a renderer acknowledgment.

Shutdown reasons have increasing precedence: ordinary close/retirement,
`reloading`, `session_failed`, `session_deleted`, then `server_stopping`.
Equal-priority requests preserve the existing reason. A stronger reason raised
during terminal snapshot construction is included; the reason is frozen only
when that snapshot is ready to publish. Closing output preserves accepted
terminal data, including when an earlier payload is still in flight.

Provider requests are cancelled and released without waiting, so process-owned
supervision may still be winding down their transport. An ordinary session
failure retires that controller without stopping unrelated sessions.

### 14.3 Presentation delivery

```text
controller mutation
    -> no update
    -> exact append -> bounded output -> acknowledged delivery -> projection
    -> structural change or unsafe append -> current snapshot
```

Snapshot/event sequence is session presentation state. Delivery ID is native
transport acknowledgment state. Context epoch identifies the application vault
context. They solve different problems and must not be used interchangeably.

## 15. Concurrency and ownership map

| Object | Owner/lifetime | Thread rule |
| --- | --- | --- |
| `Workspace` | Store-published immutable snapshot | Hold `snapshot()` while borrowing references |
| `WorkspaceConfigStore` | Application | Serializes edits; publishes after commit |
| `Application` | Native runtime | Lifecycle admission fences context-sensitive work |
| `SessionRepository` | Application | Short-lived database connections; maintenance fences operations |
| `LiveSessionManager` | Application | Facade for runtime admission, maintenance, and shutdown |
| `SessionRuntime` | Manager | One thread owns the live map, selection, controllers, and retirement |
| `LiveSession` | Runtime plus admitted callers/subscriptions | Stable endpoint; private controller state is runtime-thread-only |
| `SessionController`, `Transcript`, `SessionJournal` | One runtime entry | Shared session runtime thread only |
| `Providers` | Application | Supervises request workers and their shutdown |
| `ProviderRequest` | Supervisor, worker, controller handle | Worker produces events; session runtime consumes; cancellation is synchronized |
| `SessionOutput` | Endpoint/subscription | Owning data crosses threads; bounded/coalesced publication |
| `BridgeRouter` | Native host runtime | Coordinates request tasks, subscriptions, and delivery acknowledgment |
| Native WebView | Platform host | Platform UI access stays on its UI thread |

The C ABI's delivery callback runs on the runtime pump thread. Its JSON pointer
is valid only during that call. A host posting delivery to its UI thread must
copy the bytes first. Resource reads and native save operations must also obey
their connection/context lifetime contracts.

## 16. Persistence model

The one authoritative schema and its validators are in
[storage/workspace_session_database.cpp](../src/storage/workspace_session_database.cpp);
restore and journal SQL are in
[storage/session_database.cpp](../src/storage/session_database.cpp). Read the
schema first, then validation/restore, then `SessionJournal` methods.

Schema v2 uses these `STRICT` tables:

| Table | Purpose |
| --- | --- |
| `config` | Complete durable configuration as only `(name, content)` rows |
| `forums` | Durable forum IDs referenced by session rows; published database configuration supplies names, members, prompts, and defaults |
| `sessions` | `session_key`, owning forum, public session ID, label, `updated_at`, `archived_at`, history epoch, and next ID counters |
| `turns` | Request ID, epoch, and started/completed/cancelled/failed state |
| `entries` | Typed prompt/response/error records linked to turns |
| `entry_audio` | Cached audio bytes and content type for a session entry |

The counters and history epoch are columns on the session row rather than a
separate singleton table: they are session state, so a one-to-one table would
buy nothing.

Because one file now holds every session, each constraint that used to be
implicit — one database per session — is explicitly scoped by `session_key`:
`PRIMARY KEY (session_key, request_id)` and `(session_key, entry_id)`, the
partial unique index `one_started_turn_per_session`, and
`one_prompt_per_session_turn`. Two sessions may therefore have a started turn
at the same time, and their request and entry IDs advance independently.

Database constraints mirror transcript/controller invariants rather than
accepting any arbitrary row combination. Opening validates the database's
application ID and schema version before trusting contents, then restores
terminal entries in the current epoch and the next ID counters for that
`session_key` alone.

What is durable:

- human prompts attached to started turns;
- completed and partially cancelled character answers;
- generation error entries;
- turn state and ID counters;
- history epoch, label, `updated_at`, and `archived_at`;
- cached audio bytes and content type for saved entries.

What is deliberately not durable:

- reasoning text;
- live streaming status;
- cover runtime markers/boundary;
- notice presentation state;
- background multicast output before it becomes foreground.

Persistence failures are session-fatal because continuing would let in-memory
state diverge from the journal. Provider failures are ordinary turn outcomes
and become durable error entries.

Mirrored Markdown is not part of this persistence model. SQLite remains the
authority, the application never imports mirror files, and archived or
otherwise stale mirror files may remain. The mirror is a convenient
human-readable projection whose filenames are based on mutable display labels,
not durable identity.

## 17. Error boundaries

Provider failures are ordinary turn outcomes and become transcript errors.
Persistence failure is session-fatal: continuing would let the transcript
claim state the journal did not commit. Configuration validation failure leaves
the previous candidate active; a failure to reopen storage after maintenance
can make the entire application unavailable.

Bridge parse and admission errors use the native error envelope. Request-size
limits still matter, so `body_too_large` remains a live error code. The removed
HTTP `bad_request` and `forbidden_origin` codes are not part of the current
contract. Stale context reports `vault_changed`; stopped/unusable application
state reports `application_unavailable`.

A timeout limits how long a caller waits. It does not prove the requested work
was never performed. Do not automatically repeat non-idempotent mutations just
because their reply was abandoned.

Frontend runtime validation rejects malformed data before it enters state.
That validation complements generated DTO types and fixture tests; it does not
negotiate support for arbitrary older runtimes.

## 18. Shutdown and destruction

The native host requests shutdown, then joins runtime owners off the platform
UI thread with one bounded grace interval. It must not destroy a runtime whose
owners still access it. The C ABI exposes `cha_runtime_request_shutdown()` and
`cha_runtime_join_shutdown()` for this separation.

Shutdown stops admission, closes bridge connections/subscriptions, cancels
background work, and stops the session runtime. The stop flag and wake do not
depend on command queue capacity. The runtime checks stopping before dispatch
and between commands, rejects queued web mutations with `server_stopping`, and
finalizes each live controller before its one thread exits. An operation already
executing may finish. Controller and journal release precede `finished`; no
renderer acknowledgment is required to complete shutdown.

Controller shutdown cancels requests and closes the current durable turn. It
does not wait for provider transport on the session runtime thread. Shared
request/wake state keeps late worker completion from borrowing destroyed
controller storage.
The application coordinates provider and media shutdown within the shutdown
budget. The host's bounded failure path handles owners that cannot finish;
normal destruction must not introduce an unbounded UI-thread wait.

When changing runtime member order or callbacks, recheck that controllers,
repositories, and workers stop using storage before the configuration store
releases its lease and private root. A late reply callback must not extend the
advertised shutdown deadline.

## 19. Tests as executable documentation

| Area | Tests |
| --- | --- |
| Transcript and controller invariants | `tests/chat/`, `tests/session/` |
| Workspace ownership, edits, import/export | `tests/workspace/` |
| Application admission, settings, vaults, media, shutdown | `tests/app/` |
| Protocol, session runtime, projection, chat-input grammar | `tests/runtime/` |
| Native envelopes, method policy, routing and flow control | `tests/bridge/` |
| Shared C ABI and real native hosts | `tests/native/` |
| C++-produced wire values and method policies | `tests/fixtures/wire/` |
| Client validation, projection, reducers, loading, screens | Tests beside sources in `webapp/src/` |

CTest registers the `cha_tests`, `cha_app_tests`, `cha_bridge_tests`, and
`cha_native_runtime_tests` executables. Workspace tests live under
`tests/workspace/`, separate from application tests.

For a contract change, read both the C++ producer tests and TypeScript consumer
tests. Positive wire fixtures prove acceptance of emitted data; malformed-value
tests prove that unsafe data is rejected before rendering. Method-policy tests
detect drift even when both languages still compile.

For shared-runtime behavior, start with
[unit_live_session_manager.cpp](../tests/runtime/unit_live_session_manager.cpp),
[unit_live_session.cpp](../tests/runtime/unit_live_session.cpp), and
[unit_vault_maintenance.cpp](../tests/app/unit_vault_maintenance.cpp). They cover
selection and background retirement, full-queue maintenance deadlines and
nonblocking timeout recovery, stale queued work, shutdown admission, and reason
escalation while constructing a terminal snapshot.

On macOS, after building the runtime and frontend assets, the native parity
probe exercises the shared interface in a real WKWebView:

```sh
tests/native/macos/run.sh parity --timeout-ms 60000
```

Native dialogs, resource handlers, reload, renderer failure, and shutdown need
native-host checks; DOM tests alone cannot prove platform integration. Use the
Windows host checks when changing WebView2 behavior.

## 20. How to approach changes safely

For a workspace editor, follow [editing.md](editing.md). For any new native
operation, keep these pieces synchronized:

1. Method enum/name and control/context policy in the bridge.
2. Application operation with explicit context admission and dependencies.
3. Relevant dispatcher, or asynchronous router path when it owns completion.
4. DTO schema and generated TypeScript types.
5. Typed client mapping and the checks the consumer actually needs.
6. Wire/policy fixtures and focused behavior tests.

There is no single generated operation registry today. Do not assume updating
the DTO schema also adds dispatch or a frontend method.

For controller changes, first decide the journal transition and whether the
presentation effect is structural or safely appendable. For provider changes,
keep HTTP mechanics in `ProviderClient` and response meaning in the protocol
decoder. For lifecycle changes, trace the runtime, endpoint, reply, resource,
and database owners through cancellation and teardown.

Run checks appropriate to the changed boundary. A pure screen edit does not
need a new concurrency framework; a shutdown change needs more than a screen
snapshot. Keep changes small enough that their invariants remain readable.

## 21. Learning exercises

1. Trace one visible token from provider decoding through `GenerationEvent`,
   controller update, session output, native delivery, and React rendering.
2. Explain which objects retain the old workspace after a settings commit and
   why references into it remain valid.
3. Submit a request with an old context epoch and locate the admission error.
4. Explain the difference between a missing event sequence and an unacknowledged
   delivery batch.
5. Restore a database with a started turn and trace its repair.
6. Rename a persona and find every current summary that changes, then explain
   why saved transcript attribution does not change.
7. Follow an abandoned reply and prove that later completion cannot invoke a
   stale screen callback or block shutdown forever.

## 22. A practical reading plan

Start with domain headers and one transcript test. Continue through workspace
loading and store publication, then repository preparation and journal
transactions. Next, follow one prompt through the controller and provider
worker. Finish with application admission, bridge delivery, frontend projection,
and native-host lifecycle. At each boundary, read one success test and one
failure or cancellation test before adding code.

## 23. Glossary

| Term | Meaning |
| --- | --- |
| Vault | Named database selected by external configuration |
| Workspace | Immutable, validated configuration snapshot owned by a store |
| Context epoch | Application context identity captured when work is requested |
| Full session identity | Public `(forum_id, session_id)` pair |
| Session key | Internal SQLite identity scoping journal rows |
| Session runtime | Single thread owning all live controllers and session selection |
| Live session | Stable endpoint for one controller instance and its output |
| Controller | Runtime-thread conversation and generation state machine |
| Snapshot | Complete owning presentation state |
| Append | Text growth for a verified existing target |
| Subscription | Document/context/session-scoped event observation |
| Delivery ID | Identifier acknowledged by the frontend to release a transport batch |
| Resource handle | Native-owned media exposed to one connection/context |
| Mirror | Optional Markdown projection, never authoritative storage |

## 24. Final mental checklist

Before changing a path, identify its mutable owner, thread, captured context,
workspace snapshot, durable commit, and published result. Follow failure,
cancellation, and teardown through the same objects. If those answers are
clear, the code usually needs a local change rather than another abstraction.
