# Learning the CHA C++ codebase

This guide is a systematic route through CHA's native C++ code. Its purpose is
not merely to list files, but to build a working mental model: which objects
exist, who owns them, which thread may touch them, when data becomes durable,
and how one browser prompt turns into streamed model output.

The browser implementation under `webapp/` is outside this guide. It appears
only at the native protocol boundary, where understanding the C++ server
requires knowing what the browser sends and receives.

## 1. How to use this guide

Read the code in passes. Do not try to understand every implementation detail
on the first pass.

1. Read sections 2–5 to learn the vocabulary, dependency direction, startup,
   and ownership model.
2. Follow the focused reading path in sections 6–12. Keep the source open and
   find each named type or function as it appears here.
3. Trace the complete workflows in section 13 without diving into helpers.
4. Use sections 14–18 to consolidate concurrency, persistence, failure, and
   shutdown behavior.
5. Complete the exercises in section 21. If you can explain each answer in
   terms of concrete objects and transitions, you understand the architecture
   well enough to change it deliberately.

The subsystem `README.md` files are reference manuals. This document connects
them into a learning sequence:

- [Native architecture](../src/README.md)
- [Utility contracts](../src/util/README.md)
- [Chat model](../src/chat/README.md)
- [Character definitions and model context](../src/characters/README.md)
- [Provider execution](../src/providers/README.md)
- [Session layer](../src/session/README.md)
- [Workspace composition](../src/workspace/README.md)
- [Web runtime](../src/web/README.md)

When this tutorial and a subsystem reference differ, inspect the current
headers and tests. They are the executable contract.

## 2. The application in one page

CHA is a C++20 web server for conversations with OpenAI-compatible model
servers. The executable is `chaweb`. It serves a browser shell, exposes a JSON
HTTP API, and streams session changes with server-sent events (SSE).

The most useful high-level model is:

- `loadws()` atomically publishes the current immutable `Workspace`, which owns
  all resolved workspace-directory configuration.
- The independent process-owned `SessionRepository` lists, creates, validates,
  leases, and restores SQLite session databases. It asks the current workspace
  for forum storage paths but does not copy workspace configuration.
- `LiveSessionManager` owns the process's live-session registry.
- One `LiveSession` is an actor with one permanent owner thread for one open
  session.
- One `SessionController` owns the live transcript, persistence journal, and
  ordered generation state for that session.
- The process-owned `Providers` supervisor starts one independent request
  worker per target. The controller exposes their events in deterministic
  foreground order.
- `ProviderClient` owns provider HTTP mechanics. The Chat Completions and
  Responses API modules own their request encoding and response decoding
  without knowing about curl or HTTP.
- `SseMailbox` transfers presentation updates from the session owner thread to
  the HTTP thread writing the browser's event stream.

```mermaid
flowchart LR
    Browser["Browser client"]
    Routes["HTTP routes"]
    Manager["LiveSessionManager"]
    Actor["LiveSession owner thread"]
    Controller["SessionController"]
    Workspace["Published immutable Workspace"]
    Repository["SessionRepository"]
    Journal["SessionJournal / SQLite"]
    Providers["Process Providers supervisor"]
    Request["ProviderRequest"]
    Worker["Detached request worker"]
    Provider["ProviderClient"]
    Mailbox["SseMailbox"]

    Browser -->|"JSON commands"| Routes
    Routes -->|"getws() reads"| Workspace
    Routes --> Repository
    Routes -->|"queued WebCommand"| Actor
    Manager -->|"owns and locates"| Actor
    Actor -->|"only caller"| Controller
    Controller -->|"getws() lookups"| Workspace
    Controller --> Journal
    Controller -->|"retains while presenting"| Request
    Providers -->|"supervises while active"| Request
    Request --> Worker
    Worker --> Provider
    Request -->|"GenerationEvent queue + wake"| Actor
    Actor --> Mailbox
    Mailbox -->|"SSE snapshot or append"| Browser
```

There are three kinds of long-lived information:

- Discovery — the roster, descriptions, and Markdown the browser lists — is
  read once per `Workspace`, validated as a whole, and shared
  immutably until a successful reload publishes a replacement.
- Sessions read configuration from the published `Workspace`; disk edits are
  invisible until a successful reload publishes them.
- Conversation state is dynamic. A live controller owns an in-memory view and
  writes durable turn transitions to one SQLite database.

This split explains why creating a session appears immediately while editing
workspace configuration requires publication of a new `Workspace`.

## 3. Build graph and dependency direction

Start with [CMakeLists.txt](../CMakeLists.txt). The native build has three
important production targets:

| Target | Role |
| --- | --- |
| `cha_core` | `util`, `chat`, `characters`, `providers`, `session`, and `workspace` |
| `cha_web` | HTTP, SSE, actor runtime, routing, and protocol; links `cha_core` |
| `chaweb_app` | Small composition root in `src/web_main.cpp`; links `cha_web` |

The executable file is named `chaweb` even though the CMake target is
`chaweb_app`.

The intended dependency direction is:

```text
chaweb_app -> cha_web -> cha_core

workspace -> session -> providers -> characters -> chat
                \-----------> characters

util is used where needed without depending on the domain layers.
```

That diagram is intentionally approximate inside `cha_core`; the important
rules are simpler:

- `chat` contains presentation-neutral domain vocabulary.
- `util` contains domain-neutral mechanisms.
- `characters` owns character configuration and model-context projection.
- `providers` owns request execution, provider transport, and protocol
  decoding, but not sessions or HTTP routing.
- `session` coordinates transcripts, persistence, and generation, but not web
  routes.
- `workspace` loads the workspace and wires a session from workspace data.
- `web` adapts HTTP/SSE to the workspace and session APIs.
- Only `web_main.cpp` assembles the complete process.

If a proposed change makes `chat` include a web header, or makes `session`
depend on `httplib`, it is crossing an important boundary.

### Build and test commands

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

There are also `asan-ubsan` and `tsan` CMake presets. Browser checks and the
credential-dependent provider integration test are separate from the core C++
unit suite; see the root [README](../README.md).

The current native build requires OpenSSL on every platform because prompt-cache
keys use its SHA-256 implementation, independently of curl's TLS backend.

## 4. Repository map

| Location | What to learn there |
| --- | --- |
| [src/web_main.cpp](../src/web_main.cpp) | Process composition and destruction order |
| [src/chat](../src/chat) | IDs, personas, character metadata, transcript vocabulary |
| [src/util](../src/util) | Queues, template expansion, logging, path/text helpers |
| [src/characters](../src/characters) | Character configuration, definitions, and model context |
| [src/providers](../src/providers) | Request workers, provider transport, and response decoding |
| [src/session](../src/session) | Controller, transcript orchestration, SQLite journal, leases, repository |
| [src/workspace](../src/workspace) | Workspace loading, built-ins, and session construction |
| [src/web](../src/web) | Native protocol, routes, actor, mailbox, lifecycle, shutdown |
| [tests](../tests) | Behavioral examples grouped by the same subsystem boundaries |
| [workspace](../workspace) | A real workspace to compare against the loaders |

Headers in this project are unusually valuable: most ownership and thread
contracts are written next to the types. Read a subsystem's public headers
before its `.cpp` files.

## 5. The four concepts that unlock the code

### 5.1 Stable identity is not display text

`ForumId`, `CharacterId`, and `SessionId` are string aliases declared in
[chat/ids.h](../src/chat/ids.h). A participant also has a stable ID and a
display name. IDs are used for storage, URLs, lookup, and model attribution;
display names are used in the interface and prompt text.

The code deliberately stores both. Renaming a display label should not silently
change the identity of old entries. When reading a field named `character_id`,
`participant_id`, or `addressed_to`, do not mentally replace it with the visible
name beside it.

### 5.2 There is one mutable owner per live session

The core session model is not generally thread-safe, and it does not need to
be. `LiveSession::owner_main()` is the sole caller of its `SessionController`.
HTTP workers enqueue commands; generation workers enqueue events; the owner
thread drains both and performs all mutations.

This is actor-style confinement implemented with ordinary C++ objects, a
thread, queues, and wakeups—not an actor framework.

### 5.3 Workers receive copies, views stay local

`TranscriptView` and `ControllerView` borrow owner-thread storage and are valid
only for immediate synchronous projection. In contrast, `ModelHistory` owns a
point-in-time copy and can be shared immutably with generation workers.

The distinction prevents two common errors:

- retaining a `std::span` into a transcript that will mutate;
- allowing a worker to observe a transcript halfway through a state change.

### 5.4 State is published as a snapshot unless append safety is proven

A controller update is either no state change, a required full snapshot, or a
text append to a precise entry/reasoning target. The append is an optimization,
not an alternate source of truth. If the actor or mailbox cannot prove that the
browser has the correct base and target, it publishes a new snapshot.

This lets the mutable controller remain authoritative while keeping token
streaming efficient.

## 6. First reading pass: domain vocabulary

Begin with [chat/ids.h](../src/chat/ids.h),
[chat/persona.h](../src/chat/persona.h), and
[chat/character.h](../src/chat/character.h).

The important split in character data is:

- `CharacterMetadata` is public, discovery-safe information: ID, display name,
  description, tags, and appearance.
- `CharacterDefinition`, declared in
  [characters/character.h](../src/characters/character.h), combines public metadata with
  private backend configuration and a completed system prompt.

Routes may expose metadata. They must not expose the full definition, which can
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

The factory functions (`make_human_entry`, `make_character_entry`, and so on)
make valid intent visible at call sites. Validation still exists at boundaries;
factories are not a reason to trust arbitrary loaded data.

### 6.2 Revision, history epoch, and off-record state

Three transcript concepts are easy to conflate:

- `revision` changes on presentation mutations.
- `history_epoch` changes when the visible logical history is replaced or
  cleared.
- `OffrecordSpan` is a half-open range of entry IDs omitted from model context.

The `/hide-on`, `/hide`, and `/hide-off` operations add visible transient marker
entries and change the runtime span. Those markers and span boundaries are not
durable session history. `/clear` does not delete old SQLite rows; it advances
the durable history epoch so restoration selects only the current epoch.

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
generation worker tells the session actor, “new events may be available.” The
web implementation is `OwnerWakeSignal`. Waking does not carry state; queues
remain the source of work.

### 7.3 Configuration helpers

The other utilities support important boundaries:

- `environment.*` loads `.env` without making secrets part of character data.
- `path_name.*` and `utf8_path.*` keep filesystem and URL identifiers explicit.
- `public_name.*` centralizes visible-name validation.
- `text_template.*` expands `$$(relative/file)` includes and `$${variable}`
  substitutions with containment and cycle/resource limits.
- `logging.*` owns the process logging lifetime.

Checkpoint: locate one caller of each utility and state whether it is a domain
policy or a reusable mechanism.

## 8. Third reading pass: startup and the immutable workspace

Read [web_main.cpp](../src/web_main.cpp) once from top to bottom. It is the
composition root, so most lines construct or connect an owner.

Startup proceeds in this order:

1. `load_application_config()` reads `app.toml` and command-line overrides.
2. `load_dotenv()` loads `workspace/.env`.
3. `loadws()` loads, validates, and publishes the complete `Workspace`.
4. File logging is initialized from the published workspace settings.
5. The process-owned `SessionRepository` creates its temporary Entrance/Welcome
   database.
6. The process-owned `Providers` supervisor is constructed.
7. `LiveSessionManager` is given an opener lambda that calls `open_session()`.
8. The HTTP server, asset handler, lobby routes, and session routes are
   installed.
9. The socket is bound, the server begins listening, and shutdown coordination
   waits for a process signal.
10. Shutdown stops new work and tears down live actors, then
    `Providers::shutdown()` waits for request workers before logging stops.

Local declaration order and function scopes matter: destructors run in reverse
order, and log users must be destroyed before logging itself.

### 8.1 What `Workspace::load()` builds

Read [workspace/workspace.h](../src/workspace/workspace.h), then the helpers and
`Workspace::load()` in
[workspace/workspace.cpp](../src/workspace/workspace.cpp).

The loader treats the workspace as one configuration unit:

- It requires `characters/`, `forums/`, and `personas/` in the expected form.
- It validates character IDs and public-name uniqueness.
- It loads persona metadata and optional `PERSONA.md` prompts.
- It validates every forum's members, default character, and default persona,
  the last of which becomes the starting author for sessions in that forum.
- It validates every forum member's character, provider, and style references.
- It builds indexes and fully resolved forum-member prompt values.
- It adds the built-in Guest persona, Assistant character, and Entrance forum.

All forums are resolved when a workspace is loaded, including unused ones.
This turns an invalid member override or prompt into a deterministic load error
rather than a delayed failure when someone opens that forum.

The public methods expose information from one published workspace. Production
sessions keep their identity and runtime state, then query the current
workspace when they need forum, persona, character, provider, or style data.
Provider requests alone own the exact resolved input they are already running.
Disk edits are invisible until the next successful `loadws()`.

There is no `WorkspaceRuntime`, `WorkspaceGeneration`, or copied forum-roster
object. A candidate reload is simply a temporary `Workspace`. Once fully loaded
and validated, `loadws()` atomically makes it current. Callers use one `getws()`
result for the duration of an operation, so references into that immutable
snapshot remain valid even if another thread publishes its replacement.

### 8.2 Publication and reload

Read `getws()` and `loadws()` in
[workspace/workspace.cpp](../src/workspace/workspace.cpp). A caller holds the
returned `shared_ptr` while it reads references from the workspace. The
`SessionRepository` is separate process state and is not replaced on reload.

`POST /api/v1/workspace/reload` blocks new session opens, stops existing live
actors, and writes a workspace archive before loading one replacement
`Workspace`. Reload validates a complete candidate outside the publication
mutex and swaps it in atomically. Failure retains the published workspace, but
the actors were already stopped before filesystem work began.

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

### 8.4 Prompt construction

`Workspace::load()` combines:

- the character definition prompt;
- forum prompt/context;
- the forum's default persona and its `PERSONA.md` prompt;
- standard generated context;
- effective model backend settings.

`FORUM.md` has two audiences. It is the forum prompt above, and
`Workspace` also reads it verbatim and serves it through
`/api/v1/forums/{forum}` as the forum's description. Write it for readers as
well as for the characters. This differs from `CHARACTER.md`, which publishes
only its `<character_profile>` section: a forum publishes the whole file. The
raw template source is served, so `$${character.display_name}` and `$$(...)`
includes appear literally — a description belongs to the forum rather than to
any one member, so there is nothing to expand it against.

The built-in Assistant and Entrance data are assembled by `Workspace::load()`.
The generated workspace guide is combined with public workspace inventory
data. The Entrance/Welcome session is a normal session at the controller level;
its specialness is in how the application constructs and stores it.

Checkpoint: starting with `workspace/forums/stoics`, identify the files that
contribute to one member's final definition and the order in which values win.

## 9. Fourth reading pass: session storage and opening

Read these in order:

1. [chat/session_identity.h](../src/chat/session_identity.h)
2. [session/stored_session.h](../src/session/stored_session.h)
3. [session/session_catalog.h](../src/session/session_catalog.h)
4. [session/session_lease.h](../src/session/session_lease.h)
5. [session/session_database.h](../src/session/session_database.h)
6. [session/session_repository.h](../src/session/session_repository.h)
7. [workspace/session_open.cpp](../src/workspace/session_open.cpp)

### 9.1 Observation versus authority

`StoredSession` is listing data. It says what was observed on disk, not that the
session can be opened or is unchanged.

`PreparedSession` is authoritative for construction because it contains:

- validated identity and label;
- the database path;
- an active `SessionLease`;
- restored transcript/counter state.

`SessionRepository::prepare()` first establishes that the database file exists,
then acquires the cross-process companion-file lease, then performs the
authoritative load and validation behind that lease. Nothing observed before
the lease is trusted for controller construction.

The lease is moved into `SessionController`. Consequently, “the controller is
alive” and “this process holds the session lease” have the same lifetime.

### 9.2 Listing and creation

`SessionRepository` asks the current `Workspace` for a forum's directory and
constructs a `SessionCatalog` per operation. It does not cache workspace
configuration or session listings.

`SessionCatalog::list()` is tolerant: a recognizable database with invalid
metadata remains visible with an error label. Selecting/opening is strict.

Creation chooses a timestamp-based ID, acquires the candidate lease, creates a
temporary SQLite database, and publishes it without overwriting an existing
destination. Concurrency is settled per candidate; no global sessions-directory
lock is needed.

The repository also creates a private temporary database for Welcome. It uses
the same catalog/database/controller machinery and removes that private
directory when the repository is destroyed.

### 9.3 `open_session()` is the bridge

`workspace/session_open.cpp` performs a short but crucial composition:

1. acquire the current `Workspace` and find the immutable forum;
2. prepare and lease the stored session;
3. retain the session label for the live web actor;
4. construct a workspace-backed `SessionController`, transferring the database
   path, restore state, lease, configured default IDs, and wake notifier.

The workspace layer knows both the static workspace and dynamic repository;
neither needs to know about HTTP.

Checkpoint: explain why route code calls `validate()` before starting an actor,
but the actor's `open_session()` must still call `prepare()`.

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
- entries inside the closed off-record span;
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
3. command methods such as clear, hide, multicast, default character, and stop;
4. generation-event `apply()` overloads and shutdown.

### 11.1 What the controller owns

One controller owns:

- the session lease and `SessionJournal`;
- the owner-thread-confined `Transcript`;
- a borrowed reference to the process-owned `Providers` supervisor;
- stable forum/persona IDs used to look up the current `Workspace`;
- default-character and current-persona selection;
- next request and entry IDs;
- at most one active foreground response;
- at most one `ActiveGeneration`, containing ordered request handles.

The controller has no mutex. Its public mutation/view methods belong to the
owner thread. Thread-safe communication is isolated inside request event
queues, cancellation flags, and the wake mechanism.

`Workspace` is the forum roster and handle resolver. It centralizes exact,
normalized, and prefix matching plus ambiguity diagnostics, so prompt
submission, multicast, and default-character changes use the same rules.
`SessionController` keeps no copied roster; its style-override map stores only
style IDs and overlays the selected appearance when metadata is copied for a
request or presentation. The smaller `generation_status.h`,
`controller_view.h`, and `opened_session.h` headers are
boundary value types: they let application and web code observe or transfer
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

`request_stop()` only sets cancellation and returns. It does not block the actor
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
describe what changed without depending on SSE.

Checkpoint: for first answer chunk, second answer chunk, completion, and
provider failure, state the transcript mutation, journal operation, and update
classification.

## 12. Seventh reading pass: web actor and protocol

Read the web layer in this order:

1. [web/protocol.h](../src/web/protocol.h)
2. [web/session_projection.cpp](../src/web/session_projection.cpp)
3. [web/command_queue.h](../src/web/command_queue.h)
4. [web/sse_mailbox.h](../src/web/sse_mailbox.h)
5. [web/live_session.h](../src/web/live_session.h)
6. [web/live_session.cpp](../src/web/live_session.cpp)
7. [web/live_session_manager.cpp](../src/web/live_session_manager.cpp)
8. [web/lobby_routes.cpp](../src/web/lobby_routes.cpp)
9. [web/session_routes.cpp](../src/web/session_routes.cpp)

### 12.1 Protocol values are owning DTOs

`ControllerView` borrows controller storage. `to_snapshot()` immediately copies
it into an owning `SessionSnapshot` suitable for JSON or cross-thread
publication. The protocol also defines `AppendEvent`, command results, errors,
bootstrap data, and lifecycle/shutdown values.

This projection is the seam where core state becomes web presentation. Core
types do not acquire JSON annotations just because the browser needs them.

### 12.2 Command ingress

An HTTP handler obtains a `shared_ptr<LiveSession>` from the manager and calls
`submit()`; it never calls the controller.

`CommandQueue` has two lanes:

- bounded commands with `CommandReply` objects, so load cannot grow queued
  mutations without limit;
- unbounded, lossless owner notifications for SSE disconnect cleanup.

`submit()` enqueues under the lifecycle boundary, wakes the owner if necessary,
and waits only for the caller's deadline. A timeout abandons the reply but does
not undo a command that may already be executing. That is why the HTTP error
says the outcome is unknown.

### 12.3 Text parsing is a web adapter

The browser sends `RawCommand { text }`; the author is not the browser's to
choose, so `LiveSession` supplies the session's current persona ID. The actor passes it to
`handle_text_input()` in [web/text_input.cpp](../src/web/text_input.cpp).
Parsing is divided among:

- `text_mention.*` for leading character mentions;
- `text_command.*` for slash commands;
- `text_multicast.*` for `/mcast` syntax;
- `text_input.*` for dispatch to typed controller methods.

The parser belongs in `web` because it adapts one input protocol. The
controller exposes typed actions and remains usable without slash-command
syntax. `/style` is a presentation-only command: `set_session_style()` resolves
the name in the current `Workspace` and stores only the selected style ID as
session state. The next snapshot overlays that style on Workspace character
data. It has no backend or busy guard, and its mutating forms carry a snapshot.
It is runtime-only.

### 12.4 The owner loop

`LiveSession::owner_loop()` repeatedly:

1. drains a bounded batch of commands/notifications;
2. checks for shutdown;
3. drains a bounded batch of foreground generation events;
4. applies notice state and publishes an append or snapshot;
5. checks browser-disconnect/idle deadlines;
6. continues immediately if a batch was full, otherwise waits for a wake or
   deadline.

Bounding both drains prevents an endless command stream from starving model
events and prevents a hot model stream from starving commands such as `/stop`.

### 12.5 SSE mailbox

`SseMailbox` is the one cross-thread presentation channel from owner to HTTP
stream writer. It holds at most one in-flight payload and one replaceable
pending payload.

Snapshots replace stale pending state. Compatible appends can be combined only
when their sequence base and text target are exact. A pending snapshot plus a
later append is not assumed compatible; the mailbox rejects the append and the
owner projects a current snapshot instead.

The result is bounded memory under a slow browser. Intermediate presentation
updates may collapse, but the latest complete state is recoverable. A newly
connected SSE stream always begins with a snapshot.

A live session holds one browser SSE connection: the most recent one. CHA has
a single reader who may carry a conversation from desktop to laptop to tablet,
so a new connection takes the session over at once rather than waiting for the
previous device's stream to end, and the stream it displaces is closed with a
final `superseded` record. Connection
state also drives the actor's idle/orphan deadline; generation receives the
longer protection appropriate to active work.

### 12.6 Manager and routes

`LiveSessionManager` is the liveness authority and join authority. Its map is
keyed by `FullSessionId`.

- Concurrent opens of the same identity share one starting actor and result.
- A running actor is reattached rather than duplicated.
- Waiting for startup happens outside the manager mutex.
- Finished actors are removed under the mutex and joined outside it.
- A caller timing out while an actor starts does not cancel shared startup.

Lobby routes provide health, workspace reload, public discovery and character
settings, plus stored-session listing, creation, download, rename, deletion,
and open. Session routes operate only on a live actor:

```text
GET  /health
GET  /api/v1/bootstrap
POST /api/v1/workspace/reload
GET  /api/v1/characters/{character}
PATCH /api/v1/characters/{character}
GET  /api/v1/personas/{persona}
GET  /api/v1/forums/{forum}
GET  /api/v1/forums/{forum}/sessions
POST /api/v1/forums/{forum}/sessions
GET  /api/v1/forums/{forum}/sessions/{session}/download
PATCH /api/v1/forums/{forum}/sessions/{session}
DELETE /api/v1/forums/{forum}/sessions/{session}
POST /api/v1/forums/{forum}/sessions/{session}/open

GET  /s/{forum}/{session}/
GET  /s/{forum}/{session}/api/v1/session
GET  /s/{forum}/{session}/api/v1/events
POST /s/{forum}/{session}/api/v1/input
POST /s/{forum}/{session}/api/v1/actions/stop
POST /s/{forum}/{session}/api/v1/actions/default-character
```

`default-agent` remains a compatibility alias for `default-character`; new
code uses character vocabulary.

The route files should mostly validate HTTP input, map errors/statuses, look up
an actor, and serialize protocol values. Session behavior belongs below them.

Checkpoint: explain why a `LiveSessionHandle` may safely outlive the manager's
map entry after the owner thread has finished.

### 12.7 Web support modules

After the main actor path makes sense, scan the smaller adapters:

| Files | Responsibility |
| --- | --- |
| `application_config.*` | Resolve the application root/config file and apply CLI host/port/workspace overrides |
| `asset_handler.*` | Serve the browser shell and staged static assets without owning session behavior |
| `http_server.*` | Apply server-wide request, Host/Origin, timeout, and size policy |
| `http_response.*`, `json.*`, `route_support.*` | Consistent JSON parsing, response bodies, route components, and mutation validation |
| `browser_connection_state.*` | Hand the session to the newest browser stream and calculate disconnect/idle deadlines |
| `owner_wake_signal.*` | Coalesce cross-thread wakeups for the actor loop |
| `sse_stream.*` | Serialize mailbox payloads and heartbeats into `httplib::DataSink` |
| `server_shutdown.*` | Bridge process signals to coordinated, bounded manager shutdown |

These modules keep `LiveSession` and the route files from accumulating generic
HTTP, filesystem, and signal-handling details.

## 13. End-to-end workflow traces

Use these traces as navigation exercises. Open each function in sequence.

### 13.1 Creating and opening a stored session

1. `LobbyRoutes::install()` handles session creation.
2. `SessionRepository::create()` selects the forum catalog.
3. `SessionCatalog::create()` chooses an ID, leases the candidate, creates the
   database, and returns `StoredSession`.
4. The browser later posts to the open route.
5. The route first tries to reattach, then validates storage, then calls
   `LiveSessionManager::open()`.
6. The manager inserts a starting actor before starting its owner thread.
7. `LiveSession::owner_main()` calls the supplied opener.
8. `open_session()` reads the current forum defaults and obtains a
   `PreparedSession`.
9. `SessionController` restores transcript state and repairs any interrupted
   started turn.
10. The actor commits `running`, publishes a snapshot, and enters its loop.

The pre-route `validate()` improves error mapping but does not grant ownership.
The lease-and-load in `prepare()` remains authoritative.

### 13.2 One ordinary prompt

1. `SessionRoutes` parses JSON into `RawCommand` and enqueues it.
2. `LiveSession::execute()` calls `handle_text_input()` on the owner thread.
3. Text parsing selects `SessionController::submit_prompt()`.
4. The controller resolves the current persona and target, copies
   `ModelHistory`, persists the started turn, adds the prompt, installs active
   state, and asks `Providers` to start one request.
5. The actor publishes a snapshot showing the prompt and active generation.
6. The request worker creates a fresh `ProviderClient` and calls `prepare()` to
   project/model-encode context.
7. It calls `perform()`, whose decoder emits reasoning or answer deltas.
8. The request queues each delta and wakes the owner.
9. The actor drains the event; the controller updates reasoning or transcript.
10. Structural changes become snapshots; proven text growth becomes appends.
11. The SSE writer serializes the mailbox payload to the browser.
12. The terminal event makes the controller persist completion, cancellation,
    or failure and publish final state.

### 13.3 Multicast

1. `/mcast` syntax resolves an ordered, duplicate-free target list.
2. The controller captures one shared history for all children.
3. The controller makes the first foreground prompt durable, then starts one
   independent request per target. The first can begin slightly before the
   final request is admitted.
4. Workers perform concurrently and buffer per-request events.
5. Only request 0 mutates live state.
6. On its terminal event, request 1 becomes foreground and its prompt is then
   persisted/added; already-buffered output can be drained immediately.
7. The process repeats in requested target order.

The transcript therefore reads as a deterministic sequence of individual
turns, not interleaved token streams.

### 13.4 Stop

1. The stop route enqueues `StopCommand`.
2. The owner calls `SessionController::request_stop()`.
3. The controller sets every request's atomic cancellation flag and releases
   all non-foreground handles.
4. `ProviderClient` observes cancellation in curl's progress callback; a fake
   backend is expected to observe the same flag.
5. The command returns “Stopping generation...” without joining workers.
6. Terminal events are drained later. The active turn is persisted as cancelled
   with or without partial answer text.
7. Once the foreground terminal is handled, the controller releases its final
   request and the UI receives terminal state. Cancelled background workers may
   still be unregistering from `Providers`.

### 13.5 Changing a character's provider or style

From the browser:

1. Open Characters, select a workspace character, and follow the top-right
   chevron into Settings. Assistant has no chevron because its system config is
   not writable from the browser.
2. Choose a required provider and an optional style. No style erases only the
   style key. The sample line uses the selected style's appearance.
3. Save writes both values. The screen warns that sessions using the character
   will restart and that an answer being generated is lost.

On the server:

4. `PATCH /api/v1/characters/{id}` validates the names through the current
   `Workspace`, atomically writes `character.toml`, publishes a freshly loaded
   workspace, and asks sessions in every forum containing the character to shut
   down with `reloading`.
5. The stream drops. The browser's existing recovery ladder probes, sees
   `session_not_live`, opens again, and reattaches. It reports `session-snapshot`,
   so the settings screen stays in view. The chat shows "Applying character
   settings…" and no Retry buttons.
6. The reopened session reads the newly published provider and style.

Do not add an explicit `openConversation()` for `reloading`: that dispatch
would force the main view back to Chat.

### 13.6 Clear

1. `/clear` is rejected while the controller is busy.
2. `SessionJournal::clear()` verifies no turn is started and increments the
   durable `history_epoch` in a transaction.
3. `Transcript::clear()` removes current in-memory entries, resets off-record
   state, and increments its local epoch/revision.
4. A snapshot replaces browser state.

Old rows remain in the database as history from an earlier epoch. Restoration
loads only the current epoch.

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

### 14.2 Live actor lifecycle

```text
starting -> running -> stopping -> finished
    \---- open failed/busy/not found ----> finished
```

`finished` is published only after blocking actor teardown work, controller
destruction, journal release, and lease release. Provider requests are
cancelled and released without waiting, so process-owned supervision may still
be winding down their transport. A post-finished actor join is nevertheless
bounded, and the manager may reap the old actor.

### 14.3 Presentation delivery

```text
controller mutation
    -> no update
    -> exact append -> mailbox accepts -> AppendEvent
                    -> mailbox rejects -> current SnapshotEvent
    -> structural change -------------> current SnapshotEvent
```

Treat snapshots as truth and appends as a verified compression of truth.

## 15. Concurrency and ownership map

| Object | Lifetime/owner | Thread rule |
| --- | --- | --- |
| `Workspace` | Atomically published immutable snapshot; replaced snapshots live until their readers release them | Concurrent reads; callers hold one `getws()` shared pointer per operation |
| `SessionRepository` | Process, independent session-storage owner | Concurrent const operations; leases arbitrate sessions |
| `LiveSessionManager` | Process web runtime | Internal mutex protects registry/lifecycle coordination |
| `LiveSession` | Manager entry plus transient route handles | Owner thread mutates session; lifecycle methods synchronize |
| `SessionController` | One `LiveSession` | Owner thread only |
| `Transcript` / `SessionJournal` | One controller | Owner thread only |
| `Providers` registry | Process | Mutex protects admission and active requests; shutdown waits for transport quiescence |
| `ProviderRequest` | Session handle plus provider registry/worker | Worker produces queued events; owner consumes; cancellation is atomic |
| `CommandQueue` | One actor | HTTP producers, actor consumer |
| `SseMailbox` | One actor/stream pair | Actor producer, HTTP SSE consumer |

The application has three relevant thread roles:

1. HTTP library threads parse requests, wait on command replies, and write SSE.
2. One owner thread per live session performs all core state transitions.
3. One detached worker per active provider request performs blocking provider
   work and may briefly outlive its creating session.

When debugging a race, first classify every access by those roles. Most state
should belong entirely to one row of this table; shared mechanisms are small and
explicit.

## 16. Persistence model

The SQLite implementation is in
[session/session_database.cpp](../src/session/session_database.cpp). Read the
schema creation first, then validation/restore, then `SessionJournal` methods.

The schema has four conceptual tables:

| Table | Purpose |
| --- | --- |
| `session` | Stable session ID, forum ID, and label |
| `state` | Current history epoch and next ID counters |
| `turns` | Request ID, epoch, and started/completed/cancelled/failed state |
| `entries` | Typed prompt/response/error records linked to turns |

Database constraints mirror transcript/controller invariants rather than
accepting any arbitrary row combination. Opening validates identity before
trusting contents, then restores terminal entries in the current epoch and next
ID counters.

What is durable:

- human prompts attached to started turns;
- completed and partially cancelled character answers;
- generation error entries;
- turn state and ID counters;
- history epoch and session metadata.

What is deliberately not durable:

- reasoning text;
- live streaming status;
- off-record runtime markers/span;
- notice presentation state;
- background multicast output before it becomes foreground.

Persistence failures are session-fatal because continuing would let in-memory
state diverge from the journal. Provider failures are ordinary turn outcomes
and become durable error entries.

## 17. Error boundaries

Errors are handled at the narrowest layer that can give them meaning:

- Loaders throw contextual configuration errors; `web_main` treats startup
  failure as process failure.
- Provider response code classifies malformed model output as protocol error.
- `ProviderClient` classifies transport/HTTP failure and cancellation.
- `ProviderRequest` workers convert exceptions/results into one terminal
  generation event.
- `SessionController` turns a generation failure into a failed durable turn and
  transcript error.
- A journal failure escapes the controller and is contained as a fatal live
  session failure.
- `LiveSession` maps storage busy/not-found on startup and contains session-local
  failures during the owner loop.
- Routes map known application outcomes to JSON error codes and HTTP statuses.
- `std::bad_alloc` is generally not disguised as successful local cleanup; key
  actor boundaries terminate instead.

Avoid broad `catch (...)` additions without identifying the state boundary they
protect. A catch that lets processing continue after a journal mutation failed
can be worse than allowing the actor to terminate.

## 18. Shutdown and destruction

Shutdown is part of the design, not cleanup after the design.

At process level, the shutdown coordinator stops HTTP acceptance, asks the
manager to stop all actors, and gives the entire process one grace deadline.
The deadline is shared, not reset for every session. If owners cannot finish in
time, the process takes the immediate-exit path rather than running destructors
that could block indefinitely.

Within a `LiveSession`, teardown:

1. marks the actor stopping and publishes a final snapshot when possible;
2. performs a bounded final SSE drain unless the shutdown reason skips it;
3. closes the mailbox;
4. resolves/rejects queued command replies;
5. calls `SessionController::shutdown()`;
6. destroys the controller, releasing request handles, journal, and lease;
7. publishes `finished` so the manager may join and erase the actor.

Within the controller, shutdown cancels all requests, synchronously closes the
currently durable turn as cancelled, and releases its handles without waiting
for provider I/O. Each request owns a shared wake notifier, so a late wake does
not borrow a destroyed session object.

After all live sessions have stopped, the composition root calls
`Providers::shutdown()`. It closes admission, cancels a snapshot of active
requests, and waits for every request to unregister and finish its final
diagnostic after transport resources are gone. Diagnostic logging is shut down
only after that wait.

When changing member declaration order, constructor order, or scopes in
`web_main.cpp`, re-evaluate this destruction chain.

## 19. Tests as executable documentation

The tests mirror production boundaries. Use them after reading a type's header
and before reading all of its implementation.

| Area | Best starting tests |
| --- | --- |
| Transcript invariants | [tests/chat/unit_transcript.cpp](../tests/chat/unit_transcript.cpp) |
| Workspace loading and config validation | [tests/application/unit_workspace.cpp](../tests/application/unit_workspace.cpp) |
| Context rules | [tests/agents/unit_model_context.cpp](../tests/agents/unit_model_context.cpp) |
| Provider request lifecycle | [tests/providers/unit_providers.cpp](../tests/providers/unit_providers.cpp) |
| Provider protocols | [tests/providers/unit_chat_completions_api.cpp](../tests/providers/unit_chat_completions_api.cpp), [unit_responses_api.cpp](../tests/providers/unit_responses_api.cpp) |
| Provider HTTP integration | [tests/providers/unit_provider_client.cpp](../tests/providers/unit_provider_client.cpp) |
| Controller transitions | [tests/session/unit_session_controller.cpp](../tests/session/unit_session_controller.cpp) |
| SQLite catalog/repository | [tests/session/unit_session_catalog.cpp](../tests/session/unit_session_catalog.cpp), [unit_session_repository.cpp](../tests/session/unit_session_repository.cpp) |
| Cross-process leases | [tests/session/unit_session_lease.cpp](../tests/session/unit_session_lease.cpp) |
| Workspace publication | [tests/application/unit_workspace.cpp](../tests/application/unit_workspace.cpp) |
| Actor behavior | [tests/web/unit_live_session.cpp](../tests/web/unit_live_session.cpp) |
| Registry races/lifecycle | [tests/web/unit_live_session_manager.cpp](../tests/web/unit_live_session_manager.cpp) |
| Snapshot/append collapse | [tests/web/unit_sse_mailbox.cpp](../tests/web/unit_sse_mailbox.cpp) |
| Route protocol | [tests/web/unit_lobby_routes.cpp](../tests/web/unit_lobby_routes.cpp), [unit_session_routes.cpp](../tests/web/unit_session_routes.cpp) |
| Whole-process behavior | [tests/web/process_web_server.cpp](../tests/web/process_web_server.cpp) |

Useful test support types include fake model backends, deterministic notifiers,
temporary workspace builders, controller fixtures, live-session graphs, and a
mock provider HTTP server under [tests/support](../tests/support).

The CMake executables also reveal test scope:

- `cha_tests` covers utilities and the non-web core.
- `cha_web_tests` covers deterministic web components and routes.
- `cha_web_stress_tests` exercises concurrent live-session load.
- `cha_web_process_tests` starts the real `chaweb` process on supported
  platforms.
- `itest` uses a configured live provider and is not part of ordinary offline
  unit verification.

### A productive test-reading method

For one behavior, read in this order:

1. test name and setup;
2. public call being exercised;
3. asserted state and side effects;
4. implementation of that one call;
5. nearby negative tests that reveal the invariant.

This is faster than reading a 900-line implementation sequentially with no
question in mind.

## 20. How to approach changes safely

### Adding a controller command

1. Decide whether it is a typed core action or only web text syntax.
2. Add/adjust the `SessionController` method and `ControllerUpdate` behavior.
3. Add focused controller tests, including busy-state behavior.
4. Add parser syntax in `web/text_*` if needed.
5. Dispatch it from `handle_text_input()` or a typed `WebCommand`.
6. Verify command result, notice, snapshot/append classification, and input
   clearing in web tests.

### Adding transcript data

Trace all consumers before editing the struct:

```text
factory/validation
    -> Transcript
    -> SessionDatabase schema + read/write validation
    -> model_context projection
    -> ControllerView / SessionSnapshot
    -> protocol JSON
    -> tests and browser contract
```

Not every new field belongs in every consumer, but the decision should be
explicit.

### Adding provider response behavior

- Put curl/HTTP/cancellation in `ProviderClient`.
- Put Chat Completions encoding/decoding in `chat_completions_api.*` and
  Responses encoding/decoding in `responses_api.*`.
- Emit only generic generation deltas/results across the backend seam.
- Test response chunk boundaries independently from HTTP.
- Add a provider-client test only when transport integration matters.

### Changing session lifetime

Audit the manager map lifetime, actor raw-`this` thread capture, route
`shared_ptr` handles, owner state transitions, controller/lease destruction,
and bounded process shutdown together. These contracts are coupled even though
they live in several files.

## 21. Learning exercises

Work through these in order. Write the answer with symbol names, not only prose.

### Exercise 1: draw the object graph

Starting at `main()`, draw who owns `Workspace`, `SessionRepository`,
`LiveSessionManager`, `Providers`, routes, a `LiveSession`, its controller,
journal, active requests, request-local backends, and mailbox. Mark
`shared_ptr`, `unique_ptr`, value, and borrowed references.

### Exercise 2: follow one visible token

Set a breakpoint or add temporary tracing at:

- provider decoder delta emission;
- the delta callback in `ProviderRequest::execute()`;
- `SessionController::apply(GenerationEventDelta)`;
- `LiveSession::publish_update()`;
- `SseMailbox::publish_append()`;
- `SseStreamWriter::write()`.

Explain why the first answer token follows a snapshot path while a later token
usually follows an append path.

### Exercise 3: reconstruct after a crash

Create a session, submit a prompt, and inspect the database-related code as if
the process stopped immediately after `start_turn()`. Follow
`build_restore()`, `InterruptedTurn`, and `SessionController::initialize()` to
explain the next startup transcript.

### Exercise 4: compare single-target and multicast

Trace `submit_prompt()` and `start_multicast()` until they converge. Identify
the one history copy, request IDs, target order, each request's admission point,
and the moment each target becomes durable.

### Exercise 5: prove owner-thread confinement

Find every production call to a mutating `SessionController` method. Verify that
it originates in `LiveSession` owner code, not directly in an HTTP callback or
generation worker.

### Exercise 6: force the snapshot fallback

Read the `ControllerUpdate` and `SseMailbox` tests that combine incompatible
appends or a pending snapshot and append. Explain what browser corruption could
occur if the code sent the append anyway.

### Exercise 7: trace a public name

Choose one character in `workspace/`. Follow its directory ID, public display
name, forum membership, `CharacterMetadata`, backend definition, transcript
participant identity, model-context role, snapshot JSON, and mention
resolution.

### Exercise 8: classify failures

For malformed provider JSON, HTTP 500, missing session database, held session
lease, SQLite write failure, command timeout, and disconnected SSE stream,
identify:

- the first layer that detects it;
- whether it is a turn, session, request, or process failure;
- whether a durable record is written;
- what the browser can observe.

## 22. A practical two-week reading plan

Use this as a suggested pace, not a process requirement.

| Day | Focus | Concrete result |
| --- | --- | --- |
| 1 | Build graph, `web_main`, root/native READMEs | Draw the process object graph |
| 2 | `chat` and transcript tests | Write transcript invariants from memory |
| 3 | `util` queue/template tests | Explain shutdown semantics of each mechanism |
| 4 | workspace model and sample workspace | Trace one effective character definition |
| 5 | repository, catalog, lease | Explain observation vs prepared authority |
| 6 | database schema and restore | Trace complete, cancelled, failed, interrupted turns |
| 7 | model context | Hand-project a sample transcript into model messages |
| 8 | provider supervisor/requests | Diagram admission, foreground order, cancellation |
| 9 | provider response/client | Separate semantic decode from transport outcomes |
| 10 | controller | Trace prompt and every terminal outcome |
| 11 | protocol/projection/parsers | Map typed core actions to web DTOs |
| 12 | actor/manager | Explain thread confinement and lifecycle races |
| 13 | mailbox/routes/shutdown | Trace browser delivery and teardown |
| 14 | tests and exercises | Make one small change with tests and update this guide |

## 23. Glossary

**Actor:** One `LiveSession` plus its permanent owner thread and queues. It serializes all
session state changes.

**Active response:** The controller's foreground request currently allowed to affect transcript,
journal, and presentation.

**Backend:** A request-local `ModelBackend`. Production creates a fresh
`ProviderClient` per request; tests can inject fresh facades over shared
observation state.

**Active generation:** The controller's ordered request handles and one
deterministic foreground index for a prompt or multicast.

**Controller view:** A short-lived borrowed view of controller state, consumed synchronously to make
an owning web snapshot.

**History epoch:** A durable generation of logical transcript history. `/clear` advances it rather
than deleting older rows.

**Lease:** Cross-process exclusive ownership of a session database, held for the
controller lifetime.

**Model history:** An owning immutable transcript snapshot shared with workers for context
projection.

**Notice:** Presentation state associated with command/session feedback. It is not model
history or durable transcript state.

**Prepared session:** A session validated and restored while its lease is held, ready to transfer
into a controller.

**Snapshot:** An owning complete web presentation of current controller and actor state.

**Text append:** A compact update proven to extend one exact reasoning or transcript target on a
known snapshot base.

**Turn:** A request-correlated durable state machine beginning with one human prompt and
ending completed, cancelled, or failed.

## 24. Final mental checklist

Before saying you understand a code path, answer these questions:

1. What is the stable identity, and what is only a display name?
2. Which object owns the state?
3. Which thread may mutate it?
4. Is the value borrowed, copied, shared immutably, or moved?
5. What is the durable commit point?
6. What invariant prevents partial or ambiguous state?
7. Does the browser need a snapshot, or is an exact append safe?
8. How is cancellation observed?
9. Which layer classifies a failure, and is it recoverable at that layer?
10. In what order must the objects be destroyed?

Those questions capture the design logic repeated throughout CHA. Once their
answers are automatic, the project stops looking like a collection of files and
starts looking like one coherent system.
