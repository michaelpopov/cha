# cha — Internals

This document describes the internal architecture of `cha`, a C++20 terminal chat
client for OpenAI-compatible chat-completion servers. It covers the main
components, how data flows between them, the exact sequences behind each
user-visible operation, and — in the [Design rationale](#design-rationale)
section — why the system is shaped this way. For user-facing behavior and
configuration, see [README.md](README.md).

- [High-level architecture](#high-level-architecture)
- [Threading model](#threading-model)
- [Component catalog](#component-catalog)
- [Data flow](#data-flow)
- [Operation sequences](#operation-sequences)
- [Persistence format](#persistence-format)
- [Key invariants](#key-invariants)
- [Design rationale](#design-rationale)
- [Testing structure](#testing-structure)

## High-level architecture

`cha` is organized in four layers. Each layer only knows about the one below it,
and the boundaries between layers are the main testing seams.

```
+--------------------------------------------------------------------+
| Startup (runs once, before the chat loop)                          |
|                                                                    |
|   Workspace    StartupSelector    SessionRepository                |
|   load_agent_definition()                                          |
+--------------------------------------------------------------------+
         |
         |  Room, session database path, AgentDefinition
         |  (main() wires these into the ChatCoordinator)
         v
+--------------------------------------------------------------------+
| UI layer (main thread)                                             |
|                                                                    |
|   run_user() event loop -- poll(stdin, agent eventfd)              |
|       |                                                            |
|       v                                                            |
|   UserSession --+--> InputEditor                                   |
|       |         +--> Tui (SessionView impl) --> Terminal (ncurses) |
+--------------------------------------------------------------------+
         |                                            ^
         |  handle_input() / receive()                |  CoordinatorUpdate
         v                                            |
+--------------------------------------------------------------------+
| Coordination layer (main thread)                                   |
|                                                                    |
|   ChatCoordinator --+--> Conversation (in-memory transcript)       |
|       |             +--> ConversationJournal (SQLite transactions) |
+--------------------------------------------------------------------+
         |                                            ^
         |  CompletionRequest channel                 |  AgentEvent channel
         |  (prompt + revision)                       |  (deltas + one terminal event)
         v                                            |
+--------------------------------------------------------------------+
| Agent layer (one worker thread per roster agent)                   |
|                                                                    |
|   AgentRegistry --> AgentWorker(s) --> CompletionClient(s)         |
+--------------------------------------------------------------------+
         |
         |  HTTP: POST /v1/chat/completions (JSON body, SSE reply),
         |        GET /v1/models (startup model discovery)
         v
   +----------------------+
   |  Inference server    |
   +----------------------+
```

Ownership follows the same shape:

- `main()` owns the `Terminal`, the startup objects, and the `ChatCoordinator`.
- `run_user()` creates the `Tui` and `UserSession` on the stack for the
  lifetime of the chat loop.
- `ChatCoordinator` owns the `Conversation`, `ConversationJournal`, and an
  `AgentRegistry`.
- `AgentRegistry` owns the fixed `AgentRoster`, one shared event channel, and
  one `AgentWorker` per persona. Each worker owns its backend, request channel,
  and worker thread. The registry closes the shared event channel only after
  every worker has stopped.

## Threading model

There is one main thread and one worker thread for each fixed roster agent:

```
+----------------+---------------------+------------------------------------------+
| Thread         | Started by          | Responsibilities                         |
+----------------+---------------------+------------------------------------------+
| Main (UI)      | main()              | Terminal input, rendering, command       |
| thread         |                     | handling, database writes, applying      |
|                |                     | agent events to the transcript           |
+----------------+---------------------+------------------------------------------+
| Worker thread  | each AgentWorker    | Blocking on its request channel, running |
|                | constructor         | one synchronous HTTP completion at a     |
|                |                     | time, publishing AgentEvent values       |
+----------------+---------------------+------------------------------------------+
```

The threads share one mutex-protected `Conversation`. The main thread is its
sole writer; the worker obtains a short-lived read view only while preparing a
request. All asynchronous communication still crosses two typed queues
(`EventChannel<T>`, `src/event_channel.h`):

- **Requests channel** (`CompletionRequestChannel`): coordinator → worker. Each
  `CompletionRequest` carries the new typed prompt, target ID, request ID, and
  the revision produced when that prompt was appended; it carries no history.
- **Events channel** (`AgentEventChannel`): worker → coordinator. Carries
  `AgentDelta`, `AgentCompleted`, `AgentCancelled`, or `AgentFailed`, each
  tagged with the `RequestId` it belongs to.

Each channel is a mutex-protected `std::deque` paired with an `eventfd`
(semaphore mode). The eventfd is the key integration point: the events
channel's descriptor is exposed as `ChatCoordinator::notification_fd()` and
polled by the UI loop **together with stdin** in a single `poll(2)` call
(`wait_for_user_events`, `src/user_events.cpp`). This is what lets the UI stay
responsive to keystrokes while a response streams in, without timers or busy
polling.

Two atomics coordinate the threads outside the channels (`src/agent_worker.h`):

- `cancellation_` — set by `cancel()` on the UI thread; checked by libcurl's
  transfer-progress callback on the worker thread, which aborts the in-flight
  HTTP transfer.
- `request_outstanding_` — a compare-and-swap gate in `submit()` that enforces
  at most one in-flight request; cleared by the worker just before it publishes
  a terminal event.

The SQLite connection owned by `ConversationJournal` never crosses the thread
boundary. Session restoration and every durable mutation run on the main
thread; the worker communicates outcomes only through `AgentEvent` values.

`Conversation` is mutex-protected. Snapshots remain the owning read API for UI
planning, inspection, and tests; persistence receives individual terminal
entries from the coordinator. `ConversationReadView` holds the mutex while
exposing a non-owning span to the worker. The worker releases the view before
network I/O or delta delivery.

## Component catalog

### Startup

```
+--------------------------------+-----------------------------------------------+
| Component                      | Role                                          |
+--------------------------------+-----------------------------------------------+
| main()                         | Composition root: loads .env, drives the      |
| src/main.cpp                   | selector, restores or creates a session,      |
|                                | builds the coordinator, runs the chat loop    |
+--------------------------------+-----------------------------------------------+
| load_dotenv                    | Reads .env NAME=value pairs without           |
| src/environment.*              | overriding variables inherited by the process |
+--------------------------------+-----------------------------------------------+
| Workspace                      | Resolves rooms/rooms.list, room directories,  |
| src/workspace.*                | and the persona directory referenced by a     |
|                                | room's personas.list (exactly one persona per |
|                                | room)                                         |
+--------------------------------+-----------------------------------------------+
| StartupSelector                | Temporary ncurses screens for choosing a      |
| src/startup_selector.*         | room, choosing a session (or New session),    |
|                                | and prompting for a session display name      |
+--------------------------------+-----------------------------------------------+
| SessionRepository              | Lists, creates, and validates self-contained  |
| src/session_repository.*       | <id>.sqlite3 session databases for one room;  |
|                                | stateless — it never retains an open session  |
+--------------------------------+-----------------------------------------------+
| load_agent_definition          | Loads personas/<p>/config.toml into a Config  |
| src/agent_definition.*         | and concatenates persona SYSTEM.md with room  |
|                                | USER.md into the effective system prompt      |
+--------------------------------+-----------------------------------------------+
| Config                         | Typed persona settings: agent id/name,        |
| src/config.*                   | host/port, mode (net/test), model, streaming, |
|                                | temperature, API key, HTTPS                   |
+--------------------------------+-----------------------------------------------+
```

### Conversation domain

```
+--------------------------------+-----------------------------------------------+
| Component                      | Role                                          |
+--------------------------------+-----------------------------------------------+
| ConversationEntry              | One typed transcript record: EntryId,         |
| src/conversation.h             | EntryKind (human/agent/notice/error),         |
|                                | participant ID, display name, text,           |
|                                | CompletionStatus, optional RequestId          |
+--------------------------------+-----------------------------------------------+
| Conversation                   | Thread-safe transcript with at most one open  |
| src/conversation.*             | (streaming) entry; supports                   |
|                                | begin/append/finish/discard for streaming,    |
|                                | plus snapshots with a revision counter and    |
|                                | history epoch for renderers                   |
+--------------------------------+-----------------------------------------------+
| write_agent_context            | Projects the typed transcript synchronously   |
| src/agent_context.*            | through an AgentContextWriter (see             |
|                                | "Context projection")                         |
+--------------------------------+-----------------------------------------------+
| ConversationJournal and        | SQLite-backed turn and entry transactions,    |
| database functions             | embedded session metadata, restoration, and   |
| src/session_database.*         | interrupted-turn detection                    |
+--------------------------------+-----------------------------------------------+
```

### Agent execution

```
+--------------------------------+-----------------------------------------------+
| Component                      | Role                                          |
+--------------------------------+-----------------------------------------------+
| agent_protocol                 | The message types crossing the thread         |
| src/agent_protocol.*           | boundary: CompletionRequest (prompt +         |
|                                | revision) and the AgentEvent variant; plus    |
|                                | request/context validation                     |
+--------------------------------+-----------------------------------------------+
| AgentWorker                    | Owns the worker thread and both channels;     |
| src/agent_worker.*             | loops on the request channel, delegates to    |
|                                | the backend, publishes deltas and exactly one |
|                                | terminal event per request                    |
+--------------------------------+-----------------------------------------------+
| CompletionBackend              | Abstract two-phase interface: prepare under a |
| src/completion_backend.h       | read view, then perform an owning payload     |
|                                | returning a CompletionOutcome                 |
+--------------------------------+-----------------------------------------------+
| CompletionClient               | The real backend: streams transcript fields   |
| src/completion_client.*        | into the JSON request body, uses a private    |
|                                | typed RAII wrapper around a reusable libcurl  |
|                                | easy handle, performs the POST to             |
|                                | /v1/chat/completions, parses SSE or           |
|                                | non-streaming responses, discovers a model    |
|                                | via /v1/models when unset. In test mode it    |
|                                | echoes the prompt without networking          |
+--------------------------------+-----------------------------------------------+
| AgentInfo                      | Immutable agent details (id, name, model,     |
| src/agent_info.h               | endpoint, streaming) exposed to the UI for    |
|                                | /info without granting access to backend      |
|                                | internals                                     |
+--------------------------------+-----------------------------------------------+
```

### Coordination

```
+--------------------------------+-----------------------------------------------+
| Component                      | Role                                          |
+--------------------------------+-----------------------------------------------+
| ChatCoordinator                | The heart of the app: owns one session's      |
| src/chat_coordinator.*         | transcript, database journal, and worker;     |
|                                | parses input                                  |
|                                | into commands or prompts; runs the turn       |
|                                | lifecycle; translates AgentEvent values into  |
|                                | transcript and database mutations             |
+--------------------------------+-----------------------------------------------+
| CoordinatorUpdate              | Value returned by every coordinator operation |
| src/chat_coordinator.h         | telling the UI what to do: re-render, clear   |
|                                | the input, show a notice, or end the session  |
+--------------------------------+-----------------------------------------------+
| Command / parse_command        | Parses /clear, /info, /stop, /exit, unknown   |
| src/command.*                  | commands, and plain text                      |
+--------------------------------+-----------------------------------------------+
```

`ChatCoordinator` tracks the single active turn in an `ActiveTurn` struct
(request ID, reserved response entry ID, accumulated response text, and whether
a streaming entry is open in the transcript). Events whose request ID does not
match the active turn are ignored — this is what makes late deltas from a
cancelled request harmless.

### User interface

```
+--------------------------------+-----------------------------------------------+
| Component                      | Role                                          |
+--------------------------------+-----------------------------------------------+
| run_user                       | The stateless top-level event loop: poll for  |
| src/user.*                     | readiness, dispatch to the session (agent     |
|                                | events before terminal input), then render    |
|                                | once per iteration                            |
+--------------------------------+-----------------------------------------------+
| UserEvents /                   | The polling boundary: one poll(2) over stdin  |
| wait_for_user_events           | and the agent notification descriptor,        |
| src/user_events.*              | translated into typed readiness flags (input  |
|                                | ready, terminal closed, agent event ready,    |
|                                | interrupted, failed)                          |
+--------------------------------+-----------------------------------------------+
| UserSession                    | Testable UI state machine: routes             |
| src/user_session.*             | SessionInput actions to the editor, viewport, |
|                                | or coordinator; applies CoordinatorUpdate     |
|                                | values; coalesces renders behind a            |
|                                | render_needed flag                            |
+--------------------------------+-----------------------------------------------+
| SessionView                    | Abstract view interface (read_input, render,  |
| src/session_view.h             | scrolling, resize) so UserSession can be      |
|                                | tested without curses                         |
+--------------------------------+-----------------------------------------------+
| Tui                            | The curses implementation of SessionView:     |
| src/tui.*                      | transcript pad, status line, input pad;       |
|                                | decodes raw keys into SessionInput values     |
+--------------------------------+-----------------------------------------------+
| InputEditor                    | Multiline wide-character input buffer with    |
| src/input_editor.*             | cursor movement and backslash line            |
|                                | continuation; converts to UTF-8 on submit     |
+--------------------------------+-----------------------------------------------+
| TranscriptRenderPlanner /      | Pure logic for incremental rendering: decides |
| TranscriptViewport             | whether the transcript pad can be reused,     |
| src/transcript_renderer.*      | appended to (for example a streaming suffix), |
|                                | or must be rebuilt; tracks scroll position    |
|                                | and whether the view follows new output       |
+--------------------------------+-----------------------------------------------+
| Terminal                       | Process-wide ncurses lifecycle; switches      |
| src/terminal.*                 | modes between the startup selector and the    |
|                                | chat screen                                   |
+--------------------------------+-----------------------------------------------+
```

### Utilities

`src/text.*` (trimming, whitespace scanning), `src/path_name.*` (validating
that names are safe single path components), `src/json_string.*` (JSON string
escaping plus strict UTF-8 validation for direct request serialization), and
`src/request_id.h` (`RequestId = std::uint64_t`).

## Data flow

The full path of one chat turn, from keystroke to pixels:

1. **Input.** `poll` reports stdin readable → `UserSession::receive_terminal_input`
   drains `Tui::read_input`, feeding characters into `InputEditor`.
2. **Submit.** Enter (without a `\` continuation) converts the buffer to UTF-8
   and calls `ChatCoordinator::handle_input`.
3. **Turn start.** For plain text, the coordinator allocates a `RequestId` and
   an `EntryId` for the prompt, commits a `started` turn row and its human entry
   in one database transaction (durably, before anything else), adds the human
   entry to the in-memory transcript, reserves the response `EntryId`, and
   pushes a `CompletionRequest` — containing the prompt and resulting revision
   — into the worker's request channel.
4. **Completion.** The worker thread wakes, validates the request, acquires a
   `ConversationReadView`, verifies that the prompt is the latest entry at the
   expected revision, and calls `CompletionBackend::prepare`. `CompletionClient`
   projects that view through `write_agent_context` directly into the final
   JSON body. The view is released before `perform` sends the HTTP request and
   waits for its response. As SSE `data:` events arrive, each content fragment
   is forwarded to the delta sink, which pushes
   `AgentDelta{request_id, text}` into the events channel. Each push writes the
   eventfd.
5. **Apply.** The eventfd makes the UI `poll` return →
   `UserSession::receive_responses` → `ChatCoordinator::receive` drains the
   channel. The first delta lazily opens a streaming agent entry in the
   transcript; later deltas append to it. A terminal event
   (completed/cancelled/failed) commits the outcome, finalizes or discards
   the entry, and clears the active turn.
6. **Render.** The `CoordinatorUpdate` sets `render_needed`; at the end of the
   loop iteration `Tui::render` asks the `TranscriptRenderPlanner` for a plan —
   usually *append the new suffix of the last message* — and repaints the pads.

### Context projection

`write_agent_context` converts the typed transcript into protocol messages.
The rules encode what the model should and should not see:

- The system prompt (persona `SYSTEM.md` + room `USER.md`) goes first.
- The currently open streaming entry is skipped.
- `notice` and `error` entries are never sent.
- A `human` entry is skipped when its request ultimately failed (an `error`
  entry with the same `RequestId` exists), so failed turns don't pollute
  context.
- An `agent` entry is sent only when `complete` and non-empty — cancelled
  partial responses stay **visible in the transcript** but are **excluded from
  model context**.
- Entries from a different agent ID are still sent as `assistant` but prefixed
  with `participant_id: ` to keep speakers distinguishable.

## Operation sequences

### Startup and session selection

1. `main()` loads `.env`, constructs the `Workspace` and the `Terminal`.
2. `StartupSelector::select_room` lists `rooms/rooms.list`; the room is loaded,
   resolving its single persona.
3. `SessionRepository` (rooted at `<room>/sessions`) lists `.sqlite3` session
   databases. Invalid databases remain visible with an error so the selector
   can report the problem without hiding healthy sessions. The user picks one,
   or **New session**:
   - **New:** prompt for a display name; `create()` picks a local-time
     `YYYY-MM-DD-HH-MM-SS-session` ID (numeric suffix on collision) and
     initializes a hidden temporary sibling with its schema, embedded metadata,
     and initial durable counters in one SQLite transaction, then atomically
     publishes `<id>.sqlite3` without replacing an existing path. A collision
     selects the next numeric suffix.
   - **Existing:** `open_database_path` revalidates the database application ID,
     schema version, embedded session ID, room, and persona;
     `load_conversation_state` selects the current history epoch and checks for
     an unfinished turn (see [Crash recovery](#crash-recovery)).
   - Any validation error is shown in the selector and the user picks again.
4. `load_agent_definition` builds the `AgentDefinition`; constructing
   `ChatCoordinator` from it constructs the `CompletionClient` (which may
   perform model discovery against `/v1/models`) and starts the worker thread.
5. `run_user` enters the chat loop.

### Submitting a prompt (streaming happy path)

```
UI thread                          SQLite               Worker                 Inference
(UserSession / ChatCoordinator)    session database     thread                 server
    |                                 |                     |                       |
    |-- transaction: insert started ->|                     |                       |
    |   turn + prompt entry           |                     |                       |
    |                                 |                     |                       |
    |  add human entry to transcript, |                     |                       |
    |  clear input, render            |                     |                       |
    |                                 |                     |                       |
    |-- CompletionRequest (prompt + revision) ------------->|                       |
    |                                 |                     |                       |
    |                                 |                     |-- POST completions -->|
    |                                 |                     |   /v1/chat/completions|
    |                                 |                     |   (stream: true)      |
    |                                 |                     |                       |
    |~~~~~~~~~~~~~~~~~~~ repeated for each SSE content fragment ~~~~~~~~~~~~~~~~~~~~|
    |                                 |                     |                       |
    |                                 |                     |<- data: {delta} ------|
    |<- AgentDelta (eventfd wakes UI poll) -----------------|                       |
    |  open/append streaming entry,   |                     |                       |
    |  render                         |                     |                       |
    |                                 |                     |                       |
    |                                 |                     |<- data: [DONE] -------|
    |<- AgentCompleted -------------------------------------|                       |
    |-- transaction: mark completed ->|                     |                       |
    |   + insert response entry       |                     |                       |
    |  finish entry (status complete),|                     |                       |
    |  clear active turn              |                     |                       |
    |                                 |                     |                       |
```

A completion with **no text** is converted into a failure by the coordinator
("Agent completed without text content"). In non-streaming mode the whole
response body arrives as a single delta before `AgentCompleted`.

### Cancellation (`/stop`, Esc, or Ctrl-C while generating)

```
UI thread                          SQLite               Worker
(UserSession / ChatCoordinator)    session database     thread
    |                                 |                     |
    |  /stop, Esc, or Ctrl-C pressed  |                     |
    |  while generating               |                     |
    |-- cancel(): set cancellation_ atomic ---------------->|
    |  notice "Stopping generation..."|                     |
    |                                 |                     |
    |                                 |                     |  curl progress callback sees
    |                                 |                     |  the flag, aborts the transfer
    |                                 |                     |
    |<- AgentCancelled -------------------------------------|
    |                                 |                     |
    |  [partial text was streamed]    |                     |
    |-- transaction: mark cancelled ->|                     |
    |   + insert partial response     |                     |
    |  finish entry (status cancelled)|                     |
    |                                 |                     |
    |  [nothing streamed yet]         |                     |
    |-- transaction: mark cancelled ->|                     |
    |   (no response row)             |                     |
    |                                 |                     |
    |  clear active turn,             |                     |
    |  notice "Generation stopped"    |                     |
    |                                 |                     |
```

The cancelled partial entry remains on screen (labeled distinctly by the
renderer) but is excluded from future model context because its status is not
`complete`.

### Failure

Transport errors (curl failures), protocol errors (non-2xx status, malformed
SSE/JSON, missing `[DONE]`, empty content), and worker exceptions all surface
as `AgentFailed{request_id, message}`. The coordinator then:

1. Atomically marks the turn `failed` and inserts a typed error entry.
2. **Discards** the open streaming entry, if any (partial text from a failed
   request is not kept, unlike cancellation).
3. Adds the error entry to the transcript and clears the active turn.

If `AgentWorker::submit` itself fails (channel closed or push throws), the same
`fail_active_turn` path runs immediately — every durably started turn is driven
to a terminal database state.

### Commands and input while generating

`handle_input` parses each submission. While a turn is active, only a bare
`/stop` is accepted; anything else leaves the input untouched and shows a
notice. While idle:

- **`/clear`** increments the database's `history_epoch` and empties the
  in-memory transcript (whose own history epoch also advances so renderers do a
  full rebuild). Old rows remain stored but are outside the selected epoch.
- **`/info`** appends a notice entry showing model, endpoint, streaming mode,
  and entry count (inserted like any standalone entry).
- **`/exit`** sets `end_session` in the update; Ctrl-C while idle does the
  same from the key handler.
- **Unknown commands** and commands with unexpected arguments produce
  notices without touching the transcript.

### Crash recovery

On opening an existing session:

1. SQLite recovers or rolls back any incomplete transaction before queries are
   served. There is no application-level torn-tail repair.
2. `load_conversation_state` validates `application_id`, `user_version`, and
   the durable singleton state, then selects entries in the current
   `history_epoch` ordered by `entry_id`.
3. A `turns` row still in state `started` is unambiguous evidence that the
   prompt transaction committed but no terminal outcome did. Restore reserves
   a synthetic error entry — "Response interrupted before completion" — as an
   `InterruptedTurn`; it is not added to the persisted-entry view yet.
4. Before allowing another journal write, `ChatCoordinator::initialize` calls
   `fail_turn` for each interrupted turn and then adds the error to the
   in-memory transcript. The transaction changes the turn to `failed`, inserts
   the error entry, and advances the durable entry counter, so the next restore
   sees a terminal turn.

`next_request_id` and `next_entry_id` are explicit durable values in the
singleton `state` row. They advance transactionally and never reset on
`/clear`, so IDs are not inferred from only the currently visible rows and are
never reused within a session.

### Shutdown

`run_user` exits its loop (via `/exit`, Ctrl-C, closed stdin, or a terminal
failure) and calls `UserSession::shutdown` → `ChatCoordinator::shutdown`:

1. If a turn is active, `cancel()` is set so the in-flight transfer aborts.
2. `AgentWorker::stop()` closes the request channel, which unblocks and ends
   the worker's `dialog` loop, then joins the thread and closes the events
   channel.
3. A final `receive()` drains any last events (e.g., the cancellation's
   terminal event) so the database gets its terminal transition.

The coordinator's destructor calls `shutdown()` defensively; `AgentWorker`'s
destructor makes a final join attempt so a `std::thread` is never destroyed
joinable.

## Persistence format

### Session database — `sessions/<id>.sqlite3`

Each session is one self-contained SQLite database. There is no authoritative
sidecar metadata file and no legacy-format migration: old `.data` / `.meta`
pairs are ignored by `SessionRepository`.

The build pins the SQLite 3.46.1 amalgamation by SHA-256. Connections enable
foreign keys and a five-second busy timeout. Writable connections use
`synchronous = FULL` with SQLite's default rollback-journal mode; WAL is not
enabled. The file header carries `application_id = 0x43484131` (`CHA1`) and
`user_version = 1`, both of which are checked before a session is used.

The schema has four strict tables:

```
+----------+------------------------------------------------------------------+
| Table    | Durable contents                                                 |
+----------+------------------------------------------------------------------+
| session  | Singleton metadata: id, room, persona, label                     |
+----------+------------------------------------------------------------------+
| state    | Singleton history_epoch, next_entry_id, next_request_id          |
+----------+------------------------------------------------------------------+
| turns    | request_id, epoch, agent_id, lifecycle state                     |
+----------+------------------------------------------------------------------+
| entries  | entry_id, epoch, optional request_id, kind, participant_id,      |
|          | display_name, text, completion status                            |
+----------+------------------------------------------------------------------+
```

`entries.request_id` references `turns.request_id`. Check constraints restrict
IDs to positive integers, lifecycle and transcript enums to known values, and
the legal kind/status combinations. A partial unique index permits at most one
`started` turn in the database. Another index serves ordered entry lookup by
history epoch.

Enums are persisted as their stable integer values:

- `EntryKind`: human `0`, agent `1`, notice `2`, error `3`.
- `CompletionStatus`: complete `0`, cancelled `2`, failed `3`. Streaming `1`
  exists only in memory and is rejected before persistence.
- Turn state: started `0`, completed `1`, cancelled `2`, failed `3`.

Every semantic write is a transaction:

- Starting a turn advances `next_request_id`, inserts the `started` turn, inserts
  its human prompt, and advances `next_entry_id`.
- Completing, cancelling, or failing conditionally changes only a currently
  `started` turn and optionally inserts its terminal entry.
- A standalone notice is inserted with the same monotonic entry-ID check.
- `/clear` increments `history_epoch` only when no turn is active. Restoration
  reads entries from the new epoch while durable ID counters keep increasing.

Schema, metadata, and initial state are created in one transaction inside a
hidden temporary sibling. Once SQLite closes successfully, a same-directory
hard link atomically publishes the final filename without replacing an
existing path. Simultaneous timestamp collisions therefore cannot adopt the
same file, and a crash during initialization cannot leave an invalid
target-named `.sqlite3`. An interrupted creator can leave only an ignored
hidden temporary file. `ConversationJournal` opens only an existing, versioned
database and never creates one implicitly.

### Workspace layout

```
workspace/
├── personas/<name>/config.toml   # Config fields (id, host, port, mode, ...)
│                 └── SYSTEM.md   # persona system prompt
└── rooms/rooms.list              # ordered room names
    └── <room>/personas.list      # exactly one persona name
            ├── USER.md           # room instructions, appended to system prompt
            └── sessions/         # one <id>.sqlite3 database per session
```

## Key invariants

- **One active turn.** The coordinator holds at most one `ActiveTurn`; the
  worker's CAS gate independently rejects a second outstanding request.
- **Exactly one terminal event per request.** The worker always publishes
  `AgentCompleted`, `AgentCancelled`, or `AgentFailed` — even on exceptions.
- **Database before transcript.** The transaction containing the `started` turn
  and prompt row commits before the prompt is shown; every start eventually gets
  a terminal state (at runtime or synthesized on restore).
- **Request-ID filtering.** Coordinator event handlers ignore any event whose
  `request_id` doesn't match the active turn, making stale events harmless.
- **Coherent shared context.** The worker prepares each payload from a locked
  read view after confirming the queued revision and latest prompt; it releases
  that view before any slow transport work.
- **At most one open entry.** `Conversation` enforces a single streaming entry
  and strictly increasing entry IDs.
- **Monotonic durable IDs.** The database advances explicit request and entry
  counters in the same transaction as their rows. `/clear` changes the history
  epoch but never resets those counters.
- **One self-contained session file.** Embedded ID, room, and persona metadata
  are revalidated on open; no `.meta` sidecar can diverge from the transcript.
- **Main-thread persistence.** The journal's SQLite connection is not shared
  with the worker; only coordinator operations mutate durable state.
- **Semantic kinds over display names.** `EntryKind` drives labeling, context
  projection, and persistence — a persona named "You" cannot impersonate the
  human, and renaming a persona never reclassifies restored history because
  the immutable `id` identifies its entries.
- **Cancelled ≠ failed.** Cancelled partial text is kept (visible, excluded
  from context); failed partial text is discarded and replaced by an error
  entry.

## Design rationale

The sections above describe *what* the code does. This section explains *why* it
is shaped that way: the constraint each decision responds to, the alternatives
that were rejected, and what the choice costs. Most of these constraints are not
visible from any single file, and breaking one usually still compiles — so this
is the section to read before changing a layer boundary, the database schema, or
the turn lifecycle.

### 1. Two threads, and one `poll` that waits on both worlds

**The constraint.** A chat client has to wait on two unrelated event sources at
the same time: the terminal, which is a file descriptor, and the model's output,
which arrives from libcurl's synchronous, blocking easy interface. Neither can
be made to wait on the other directly.

**Alternatives rejected.**

- *Single thread with curl's multi interface.* Avoids a thread, but drags
  non-blocking transport state and curl's descriptor set into the UI loop, and
  any synchronous work in the transport stalls rendering.
- *A condition variable to signal the UI thread.* A condition variable cannot be
  waited on together with stdin. The UI would need a timed wait — reintroducing
  both idle wakeups and input latency — or a third thread whose only job is
  translating one wait primitive into another.
- *Polling the queue on a timer.* Burns CPU while idle and adds latency
  proportional to the tick interval; streaming would visibly stutter.

**What was chosen.** `EventChannel<T>` pairs a mutex-protected `std::deque` with
an `eventfd`. Pushing a value writes the descriptor, so an in-process queue
becomes pollable. The UI then makes exactly one `poll(2)` call over stdin and
the events channel, with no timeout: idle costs zero CPU, and either source
wakes the loop immediately. This is the single decision that makes the input
pane stay responsive while a response streams in.

The descriptor is opened with `EFD_SEMAPHORE` so that one queued value
corresponds to exactly one readable token. Counts cannot drift out of step with
queue length, so wakeups are neither lost nor doubled, and `try_get` can
distinguish "empty" from "closed" reliably.

**Cost.** `eventfd` is Linux-specific. That is contained: only
`src/event_channel.h` and `src/user_events.cpp` know about descriptors, and a
portable build would replace those two files (a self-pipe behaves identically).
The rule this implies for future work is that any new UI event source must be
expressible as a descriptor, or it will not fit the loop.

### 2. Shared conversation, bounded lock scope

**Decision.** `ChatCoordinator` owns one `Conversation`. A request carries only
the new prompt and its expected conversation revision. `AgentWorker` validates
that request, holds a `ConversationReadView` while preparing an owning payload,
then releases the view before calling `CompletionBackend::perform`.

**Why.** This avoids repeated ownership of the transcript while retaining a
strong, explicit coherence boundary. The main thread remains the sole writer;
the worker's const access is serialized by the conversation mutex. The worker,
not a backend, owns validation and the lexical lifetime of the lock, so no
transport path can accidentally hold it through curl or delta publication.

The revision and latest-prompt check turn an unintended transcript mutation
between queueing and preparation into a visible failed turn instead of sending
the wrong context. The existing one-active-turn coordinator rule keeps this a
tripwire rather than a normal control path.

**Cost.** The UI can briefly wait while a worker serializes a request. That work
is local and bounded; it never includes DNS, upload, response waiting, or SSE
parsing. The final HTTP body is the only history-sized owned request value,
which is unavoidable for the stateless chat-completions API.

### 3. One turn at a time — and why request IDs exist anyway

**Decision.** At most one outstanding request, enforced in two independent
places: the coordinator's `std::optional<ActiveTurn>` and the worker's
`request_outstanding_` compare-and-swap.

**Why one.** The UI is a single linear transcript. Concurrent responses would
require streaming into several open entries at once, an ambiguous ordering in
the durable transcript, and a per-response cancellation affordance. Nothing in
the product asks for that today, and every one of those costs would be paid
immediately.

**Why enforce it twice.** The two checks guard different resources and neither
is redundant. The coordinator's check protects the transcript and durable turn
state (one open entry, one started database turn). The worker's CAS protects
the transport (one curl handle, one cancellation flag) and holds even when the
worker is driven by something that is not the coordinator — which is exactly
what the worker's own unit tests do.

**Why request IDs, if there is only ever one turn.** Because cancellation is
asynchronous. `cancel()` only sets a flag; the transfer aborts at the next
progress callback, so deltas already in flight can arrive after the UI has moved
on. Every coordinator handler begins with `matches(request_id)` and silently
drops anything that is not the active turn, which is what stops a late fragment
from being appended to the *next* turn's response. The ID is also the
correlation key tying a transcript entry, a database turn, and a protocol event
together — without it, durable lifecycle transitions could not be matched.

### 4. SQLite transactions for session state and turn lifecycle

**Decision.** Each session is a SQLite database containing embedded metadata,
durable counters, turn lifecycle rows, and transcript entry rows. Each semantic
operation is one explicit transaction.

**Why replace the JSONL journal.** The previous design implemented database
responsibilities in application code: `fsync` loops, torn-tail truncation,
record parsing, turn-bracket replay, two-file session creation, orphan
avoidance, and TOML metadata validation. SQLite provides atomic commit, crash
recovery, constraints, and prepared statements as one well-tested boundary.
The application now describes semantic state transitions rather than
reimplementing storage mechanics.

**Lifecycle information remains explicit.** Starting a turn commits a `turns`
row in state `started` together with its prompt. Completion, cancellation, and
failure are conditional transitions from that state. If the process disappears
after the start commits, SQLite preserves the row while rolling back any
incomplete later transaction. `WHERE state = started` therefore detects an
interrupted turn without replaying or repairing an application-level log.

**Why one database per session.** It preserves failure isolation and
portability: a corrupt database affects one session, sessions can be moved or
removed individually, and separate sessions do not contend for one database
writer lock. Listing does open each candidate database read-only to fetch its
metadata, an acceptable cost for a human-scale local session directory.

**Why metadata is embedded.** A `.meta` sidecar and message database would again
form a two-file logical object, reintroducing mismatched updates and orphan
states. The `session` row is committed with schema initialization and is always
read from the same database as the transcript.

**Why an epoch for `/clear`.** Deleting rows would work, but it would either lose
cleared history or tempt restoration to derive IDs from the remaining maximum.
Incrementing `history_epoch` makes the visible history switch atomic while
explicit counters preserve monotonic IDs. Old rows remain recoverable with
ordinary SQLite tools.

**Costs.** SQLite is a pinned native dependency and the raw C API needs a small
RAII boundary for connections, prepared statements, and transactions. The
database may create a transient rollback-journal file during a write, although
the durable session artifact at rest is one `.sqlite3` file. Schema changes
require a deliberate `user_version` change; this version intentionally has no
legacy `.data` / `.meta` importer.

### 5. Durability ordering: database before screen

**Decision.** The start transaction commits before both the in-memory transcript
mutation and the worker submit.

**Why.** It guarantees the visible state is never ahead of the durable state. If
the process dies between the two, restore reports an interrupted turn — an
honest, visible outcome. In the opposite order the user would see their prompt,
the process would die, and the prompt would simply be missing on restore with no
evidence it ever existed.

The error paths preserve the same ordering rule. If adding the prompt to the
`Conversation` throws after the start transaction commits, `submit` changes the
turn to `failed` before rethrowing. If dispatch to the worker fails (closed
channel, or `push` throws), `fail_active_turn` runs immediately. The invariant
to maintain when editing this path: **once a `started` turn is durable, no
normal code path may exit without committing a terminal state.** A process
crash is the exception handled during restoration.

### 6. Typed transcript entries instead of protocol roles

**Decision.** The transcript stores `ConversationEntry` — kind, participant ID,
display name, text, status — and never stores `{role, content}`. Roles are
produced on demand by `write_agent_context`.

**Why not store roles directly.** The transcript must represent things the
protocol has no role for: notices, errors, a response that is currently
streaming, a response that was cancelled halfway. Encoding those as roles means
either lying to the model or inventing pseudo-roles that every consumer then has
to special-case.

Three separations in the entry each do real work:

- **`kind` vs. `display_name`.** The label a user sees is cosmetic and
  configuration-controlled — a persona may legitimately be named "You" or
  "System". Labeling, filtering, and persistence all key off `EntryKind`, so a
  persona cannot impersonate the human or a system notice no matter what it is
  called. `transcript_entry_label` derives the label from the kind, not the name.
- **`participant_id` vs. `display_name`.** The ID is immutable persona identity;
  the name is presentation. Renaming a persona, or moving its directory, does
  not reclassify restored history, because persisted entries carry the ID.
- **`status` vs. `kind`.** Whether a response is streaming, complete, cancelled,
  or failed is orthogonal to who spoke, and it is precisely what context
  projection filters on.

Legal combinations are validated in one place (`validate_conversation_entry`):
an error entry must be `failed`, human and notice entries must be `complete`, an
agent entry may never be `failed`, and a complete agent entry must have text.
Illegal states are rejected at construction and again before persistence
(`require_terminal_conversation_entry`), which is why a `streaming` status can
never reach disk.

**The payoff.** There is exactly one place — `write_agent_context` — that decides
what the model sees. It emits synchronous fragments to a writer, so production
can serialize directly while tests can materialize readable messages.

### 7. Cancelled and failed differ on purpose

This is the subtlest pair of rules in the codebase, and the easiest to "simplify"
into a bug.

- A **cancelled** partial response is *kept* in the transcript and *excluded*
  from model context.
- A **failed** response has its partial text *discarded*, is replaced by an error
  entry, and the prompt that caused it is *also excluded* from context.

**Why cancelled text stays visible.** The user read it. It was genuine output,
and deleting text somebody is currently looking at is both surprising and
destroys information they may have wanted to keep.

**Why it is nevertheless not sent back.** Replaying a truncated answer as a
*complete* assistant turn misrepresents the history and teaches the model that
half-finished answers are the house style. `write_agent_context` sends agent
entries only when their status is `complete`, which is the whole reason status
lives on the entry rather than being implied by position.

**Why failed text is discarded instead.** A failure means the response is not
trustworthy — typically an HTTP error or a malformed stream, where any partial
text is a fragment of something that never became an answer. An explicit error
entry communicates more to the user than the fragment does.

**Why the failed prompt is dropped from context too.** Otherwise the context
would end with a user message that has no reply, which is a misleading
conversational shape and invites the model to respond to it twice.
`write_agent_context` collects every request ID that has an error entry and skips
the matching human entry, so a failed turn leaves *no trace* in what the model
sees while remaining fully visible on screen and fully recorded in the database.

The general principle underneath all four rules: **the transcript is a record for
the human; the context is an argument to a model.** They are deliberately not the
same object, and `write_agent_context` is the only place allowed to know the
difference.

### 8. Effects as return values

**Decision.** Coordinator operations return a `CoordinatorUpdate`
(`render_needed`, `end_session`, `clear_input`, `notice`) instead of calling into
the UI.

**Why.** The dependency direction stays one-way. `ChatCoordinator` names no UI
type in its headers and can be driven headless, which is exactly how it is
tested. Callbacks would invert that and pull view concerns into the turn
lifecycle.

**The second benefit is performance.** Because effects are values, they merge.
`receive()` drains the entire channel in one pass and `merge_update` folds every
event's effects into a single update; the loop then renders once. During a fast
stream that collapses dozens of deltas per wakeup into one repaint.
`UserSession`'s `render_needed` flag performs the same coalescing across a whole
loop iteration — input handling and agent events both merely *request* a render,
and `render_if_needed` performs at most one at the end. Rendering is never
triggered from inside event handling, which is what keeps redraw cost decoupled
from event rate.

**Ordering note.** Each iteration processes agent events *before* terminal input,
so a keystroke is always interpreted against the freshest model state — `/stop`
or Esc sees whether the turn has in fact just completed.

### 9. Seams chosen for testability, not for pluggability

**Decision.** The externally substitutable seams are `CompletionBackend` and
`SessionView`; `AgentContextWriter` is a narrow synchronous serialization sink
used by the context projector and its tests.

Both were introduced at the two places where the real implementation cannot run
in a unit test: one owns a socket, the other owns a terminal. Each replaces an
entire hard dependency with a value the test controls, and each keeps the real
class as the only production implementation. Everything else — `Conversation`,
the database journal, the render planner, the input editor, and context
projection — is concrete code tested directly, because none has such a
dependency. The
absence of further interfaces is deliberate: abstraction without a second
implementation or a test need is cost with no return.

**Note what is deliberately *not* a seam.** Test mode (`mode = "test"`, echoing
the prompt) lives inside `CompletionClient`, not behind a separate backend. It is
a user-facing feature for running without a server, not a testing hook, and
keeping it in the client means the coordinator has exactly one production path
regardless of mode.

**The result.** Everything except `main()` lives in `cha_core`, so tests link the
real code: the UI is testable without a terminal, the coordinator without a
network, and the transport without a model. `main()` stays a thin composition
root with nothing worth testing.

### 10. The backend blocks, on purpose

**Decision.** `CompletionBackend::perform` is synchronous. Its `prepare` phase
is local request construction under the worker-owned read view; streaming is
expressed as a callback into `perform`, not as an async stream, a future, or a
state machine.

The worker thread exists precisely so the transport can stay simple: one curl
easy handle, one blocking `perform`, deltas pushed as they are parsed. An
asynchronous transport would add machinery for no benefit — there is only ever
one request in flight, and the thread has nothing else to do while it waits.

`CompletionClient` hides libcurl behind a private nested `CurlEasyHandle`. It
owns and resets the reusable easy handle, checks option and status calls, and
provides type-specific setters for `long`, pointers, callbacks, header lists,
and `curl_off_t`. This contains libcurl's variadic API in one implementation
file, removes curl types from the public header, and leaves the load-bearing SSE
parser explicit rather than introducing another transport dependency.

**Cancellation is cooperative for the same reason.** The progress callback
(`CURLOPT_XFERINFOFUNCTION`) checks the atomic and returns non-zero to abort. The
alternative — closing the socket from another thread — races with libcurl's own
use of that descriptor. Cooperative abort means cancellation is always observed
at a safe point, and it can be reported as a *normal* outcome
(`CURLE_ABORTED_BY_CALLBACK` plus the flag still set becomes
`CompletionOutcome::cancelled`) rather than surfacing to the user as a spurious
transport error.

### 11. Unbounded generation, bounded discovery

**Decision.** Both requests bound *connection* setup at 10 seconds. Beyond that
they differ: the completion request sets no total timeout and no low-speed
limit, while model discovery adds a 10-second total timeout.

**Why the asymmetry.** A long generation is a legitimate, expected state — a
local model may think for minutes — and a timeout would kill valid work while
giving the user no way to tell a deadline from a hang. Instead the user gets
immediate, explicit cancellation (`/stop`, Esc, Ctrl-C) and continuous evidence
of progress (streamed text, status line), which is strictly better control than
an arbitrary deadline. TCP keepalive is enabled so a genuinely dead peer is still
detected.

Model discovery is the mirror image: a small metadata `GET` issued during
construction, before the chat UI exists. There is nothing to watch and no way to
cancel it, so a hang there is indistinguishable from a broken startup. It gets a
bound.

### 12. Strict validation, loud failure

**Decision.** Malformed input is rejected with a specific error rather than
repaired or guessed. A session database must carry the expected SQLite
`application_id` and schema `user_version`; its embedded ID, room, and persona
must match its filename and selected room.

**Why.** This client keeps a durable record of real conversations. Silently
accepting a file it does not fully understand risks corrupting that record or
attributing text to the wrong speaker — failures the user would not notice
until the history is already wrong. So errors identify the database and the
SQLite failure; invalid sessions are surfaced in the selector so the user can
pick another instead of the application dying.

Two checks in particular earn their keep:

- **Room and persona binding in metadata.** Opening a transcript under a
  different persona would silently reinterpret the participant IDs already in the
  database, blending two agents' histories into one.
- **Schema and row constraints.** Strict tables, foreign keys, enum checks,
  monotonic counter updates, and conditional turn transitions reject states
  that the old replay code had to discover procedurally.
- **No implicit database creation.** `ConversationJournal` opens read-write
  without `SQLITE_OPEN_CREATE`; only `SessionRepository::create` can initialize
  a session. A mistaken path therefore fails instead of becoming an empty
  database.

Version fields here are a tripwire, not a migration mechanism: a mismatch is a
hard, immediate failure by design.

### 13. What the shape leaves room for

The application is single-agent, but several interfaces deliberately preserve
identity. Entries carry a `participant_id` rather than an "is assistant"
boolean; requests and durable turn rows carry an `agent_id`; events are keyed
by request ID;
`write_agent_context` already prefixes another agent's text with its ID when
projecting; and `ActiveTurn` is documented in the header as the single-agent
case.

The pieces that would have to change are correspondingly easy to name:
`ActiveTurn` becomes a map keyed by request ID, `Conversation` has to tolerate
more than one open entry, the worker's single-request CAS becomes per-agent, and
the database's `one_started_turn` unique index must be relaxed or scoped to an
agent. The transcript rows and turn rows already carry the identities needed
for that evolution, but concurrency policy is intentionally encoded today.

This is an observation about the current shape, not a commitment to build it.

## Testing structure

## Multi-agent update

The earlier single-agent descriptions in this document are superseded by this
section where they conflict. A room now loads an ordered, immutable-for-the-run
`AgentRoster` from every non-comment line of `personas.list`. `AgentRegistry`
owns one worker per roster member and their shared `AgentEventChannel`; the UI
still polls exactly one notification descriptor. The coordinator remains the
sole writer of the shared `Conversation` and allows only one active turn across
the whole roster.

Human transcript entries now record `addressed_to` (the immutable agent ID) and
`addressed_to_name` (the display name at send time). The prompt target is the
only routing authority: the registry selects a worker from it and that worker
validates the target before preparing a request. SQLite schema version 2 stores
the same pair on `entries`, removes `turns.agent_id` and `session.persona`, and
recovers an interrupted turn's attribution by joining the turn to its prompt.

`@Name` at the beginning of a prompt routes to a named agent; an unmentioned
prompt uses the first roster member (or the run-local `/@Name` override).
Mention parsing preserves ordinary leading whitespace, supports `@@` as a
literal leading at-sign, and leaves rejected mentions in the input editor.
Foreign completed agent responses become attributed `user` messages in context;
human prompts addressed to another agent include a target marker. The renderer
shows `[You → Name]` when the current or restored transcript demonstrates more
than one participant.

Everything except `main()` is built into the `cha_core` library, so the
test binaries link the real implementation.

- **Unit tests** (`cha_tests`, `tests/unit_*.cpp`) exercise components through
  their seams:
  - `CompletionBackend` — coordinator and worker tests inject scripted fake
    backends instead of the HTTP client.
  - `SessionView` — `UserSession` tests inject a fake view instead of curses.
  - Pure logic and local persistence (`Conversation`, `write_agent_context`,
    SQLite restoration and transactions, `TranscriptRenderPlanner`,
    `InputEditor`, `Config`, `Workspace`, and text utilities) are tested
    directly.
  - Persistence tests cover embedded metadata, typed-entry round trips,
    transactional rollback, monotonic IDs, epoch-based clear, interrupted-turn
    detection/finalization, timestamp collisions, invalid databases, and the
    rule that a journal never creates a missing database implicitly.
- **Integration tests** (`itest`, `tests/integration_test.cpp`) run against
  the checked-in `workspace/` (pinned via `CHA_WORKSPACE_DIRECTORY`) and a
  `mock_http_server.h` that speaks real HTTP/SSE to a real
  `CompletionClient`. They are built by `make itest` but excluded from
  `make test`.

The layering shown throughout this document is what keeps the tests cheap: the
UI can be tested without a terminal, the coordinator without a network, and
the transport without a model.
