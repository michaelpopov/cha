# cha — Internal design

This document describes the current implementation of `cha`, a C++20 terminal
client for OpenAI-compatible chat-completion servers. It is an architecture and
maintenance guide; user-facing setup and command documentation lives in
[README.md](README.md).

- [Architecture](#architecture)
- [Startup and workspace loading](#startup-and-workspace-loading)
- [Multi-agent model](#multi-agent-model)
- [Conversation and context projection](#conversation-and-context-projection)
- [Turn lifecycle](#turn-lifecycle)
- [Commands and terminal behavior](#commands-and-terminal-behavior)
- [Persistence](#persistence)
- [Transport](#transport)
- [Shutdown and failure handling](#shutdown-and-failure-handling)
- [Component map](#component-map)
- [Key invariants](#key-invariants)
- [Build and testing](#build-and-testing)

## Architecture

`cha` has one main/UI thread and one worker thread per agent in the selected
room. The main thread owns all transcript and database mutation. Worker threads
prepare and perform blocking completions, then return typed events through one
shared channel.

```text
Startup
  Workspace + StartupSelector + SessionRepository + load_agent_definitions
                                      |
                                      v
Main/UI thread                  ChatCoordinator
  run_user                   +-- Conversation
  UserSession -------------->+-- ConversationJournal (SQLite)
  Tui                         +-- AgentRegistry
       ^                              |
       | CoordinatorUpdate            +-- AgentRoster
       |                              +-- shared AgentEventChannel
       |                              +-- AgentWorker[0] -- CompletionClient[0]
       |                              +-- AgentWorker[1] -- CompletionClient[1]
       |                              +-- ...
       |
       +---- poll(stdin, shared agent-event eventfd)
```

The ownership graph is:

- `main()` owns the `Terminal`, selected room/session state, and
  `ChatCoordinator`.
- `run_user()` owns a `Tui` and `UserSession` for the chat loop.
- `ChatCoordinator` owns the in-memory `Conversation`, SQLite
  `ConversationJournal`, `AgentRegistry`, current default agent, and optional
  active turn.
- `AgentRegistry` owns a non-empty ordered `AgentRoster`, a shared
  `AgentEventChannel`, and one `AgentWorker` per roster slot.
- Each `AgentWorker` owns one backend, one request channel, a worker thread, and
  its cancellation/outstanding-request atomics.
- Each production backend is a `CompletionClient` with one reusable libcurl easy
  handle and one agent-specific system prompt and configuration.

### Thread communication

`EventChannel<T>` is a mutex-protected `std::deque` paired with a Linux
`eventfd` opened with `EFD_NONBLOCK | EFD_SEMAPHORE`. There are two channel
directions:

- Each worker has a private `CompletionRequestChannel` from coordinator to
  worker.
- All workers publish `AgentEvent` values into the registry's shared
  `AgentEventChannel`.

The shared event channel exposes one descriptor to the UI. `run_user()` blocks
in one `poll(2)` call over stdin and that descriptor, so input and streamed
output wake the same event loop without timers or busy polling. When both are
ready, agent events are applied before terminal input.

The event variant contains:

- `AgentDelta { request_id, text }`
- `AgentCompleted { request_id }`
- `AgentCancelled { request_id }`
- `AgentFailed { request_id, message }`

`Conversation` is mutex-protected. The main thread is its sole writer.
`AgentWorker` obtains a short-lived `ConversationReadView` while validating and
preparing a request. That view holds the mutex and exposes a non-owning span; it
is destroyed before network I/O and before any delta is published.

## Startup and workspace loading

`main()` performs these steps:

1. Load optional `.env` values without replacing existing process variables.
2. Construct `Workspace` and `Terminal`.
3. Let `StartupSelector` choose a room from `rooms/rooms.list`.
4. Parse the room's ordered `personas.list`.
5. Resolve every listed `personas/<name>` directory and load all agent
   definitions.
6. Let the user choose or create a session in `<room>/sessions`.
7. Fully restore an existing database, if selected.
8. Construct `ChatCoordinator`; production backend construction may perform
   model discovery before worker threads start.
9. Enter `run_user()`.

The workspace shape is:

```text
workspace/
  .env                         optional
  personas/
    <persona>/
      config.toml
      SYSTEM.md
  rooms/
    rooms.list
    <room>/
      personas.list
      USER.md
      sessions/
        <session-id>.sqlite3
```

Blank lines and lines beginning with `#` are ignored in both list files. Names
must be safe single path components. `personas.list` must contain at least one
entry and cannot repeat a persona directory name. `Workspace::load_room`
parses the room and list; persona-directory existence is checked when `main()`
resolves each entry.

For each persona, `load_agent_definition()` loads `config.toml`, reads
`SYSTEM.md`, reads the selected room's `USER.md`, and joins the prompts with two
newlines. `load_agent_definitions()` preserves list order. `AgentRoster` is the
single boundary for a non-empty roster and for valid, unique configured agent
IDs and ASCII-case-insensitive names, including when tests inject backends
without workspace loading.

Agent IDs are non-empty ASCII letters, digits, underscores, or hyphens. Display
names are non-empty, contain no whitespace, cannot begin with `@` or `/`, and
cannot equal `User` under ASCII case folding.

Sessions are room-scoped, not roster-scoped. Session metadata contains the
session ID, room, and display label, but no persona or roster. A session can be
reopened after agents have been renamed, removed, or added; persisted
participant IDs, display names, and prompt targets preserve historical
attribution.

## Multi-agent model

The room's roster is ordered and fixed for one run. The first agent is the
initial default. `/@Name` changes the default in memory for the current run
only.

Only one turn may be active across the entire roster. The application does not
run simultaneous answers, even though each agent owns a separate worker
thread. This preserves one linear transcript, one streaming entry, and one
unambiguous cancellation target. Each worker also independently rejects a
second outstanding request, protecting its transport when tested or used
outside the coordinator.

### Prompt addressing

`parse_addressed_prompt()` recognizes an optional leading mention after leading
whitespace:

- `@Name prompt` addresses `Name` and stores only `prompt`.
- No mention uses the current default agent.
- `@@text` removes one `@` and submits literal `@text` to the default agent.
- Ordinary input, including its whitespace, is preserved.

Handle resolution tries, in order:

1. exact display name, ASCII-case-insensitively;
2. exact name after removing trailing `,.;:!?`;
3. a unique ASCII-case-insensitive prefix.

Unknown and ambiguous handles are rejected with a roster-aware notice. The
input is not cleared, so the user can correct it. A mention with an empty body
is also rejected without clearing the input.

The resolved target is stored on the human transcript entry as both:

- `addressed_to`: immutable agent ID, used for routing and context semantics;
- `addressed_to_name`: display name at submission time, used for restored UI
  labels and target attribution.

`AgentRegistry` keeps workers and roster entries in the same order. Submission
and cancellation linearly scan the small roster to select the corresponding
worker slot; there is no parallel index structure.

## Conversation and context projection

### Typed transcript

`ConversationEntry` is the common record used by rendering, persistence, and
model-context projection:

```text
id
kind                  human | agent | notice | error
participant_id
display_name
addressed_to          human entries only
addressed_to_name     human entries only
text
status                complete | streaming | cancelled | failed
request_id            optional correlation with a turn
```

The fixed human identity is `participant_id = "human"` and display name
`"You"`. Notices display as `"System"` and errors as `"Error"`. Agent entries
carry the producing agent's stable ID and the display name used during that
turn.

`Conversation` supports terminal insertion and a single open streaming agent
entry:

- `add_entry`
- `begin_entry`
- `append_to_entry`
- `finish_entry`
- `discard_entry`
- `clear`
- `replace_entries`

Every mutation increments a revision. `clear()` and `replace_entries()` also
advance an in-memory history epoch so incremental renderers know to rebuild.
Entry IDs must be positive and strictly increasing, but need not be contiguous.

Snapshots are owning copies used by rendering and tests. `ConversationReadView`
is the locked non-owning API used during backend preparation.

### Validation

`validate_conversation_entry()` enforces the semantic combinations:

- human and agent entries require participant IDs;
- human entries require a target ID and target display name;
- only human entries may carry targeting fields;
- human and notice entries must be complete;
- error entries must be failed;
- agent entries may be streaming, complete, or cancelled, but never failed;
- a complete agent entry must contain text.

`require_terminal_conversation_entry()` additionally rejects streaming state
before persistence.

### Per-agent model context

The transcript is a human-visible record; model context is a projection.
`project_agent_context()` materializes that projection for the agent handling
the new prompt:

1. Emit that agent's effective system prompt, if non-empty.
2. Exclude the currently open streaming entry.
3. Exclude all notices and errors.
4. Exclude a human prompt whose request has a matching error entry.
5. Include only complete, non-empty agent responses; cancelled partial output
   remains visible but is not sent back to a model.
6. Emit the target agent's own completed responses as `assistant`.
7. Emit human prompts and other agents' completed responses as `user`.

When the projected history contains cross-agent evidence, human messages are
attributed as `User: ...`; a prompt addressed elsewhere becomes
`User: [to Name] ...`. A foreign agent response becomes `Name: ...`.
Contiguous human/foreign-agent material may be coalesced into one `user`
message with blank-line separators, while adjacent ordinary human prompts
remain separate when no foreign response joins them.

This makes every agent see the same shared conversation from its own point of
view: its own prior answers are assistant messages, while other agents'
answers are attributed input.

## Turn lifecycle

### Submission

For an idle coordinator:

1. Parse commands and addressing, resolve the target agent, and reject an empty
   prompt.
2. Allocate a request ID and human entry ID.
3. Commit the `started` turn and prompt entry in one SQLite transaction.
4. Add the prompt to the in-memory conversation.
5. Reserve the response entry ID and create `ActiveTurn`.
6. Route the request to the target worker.

The database commit intentionally precedes the screen and worker. Once a
started turn is durable, normal error paths drive it to a terminal state.

The coordinator is the only conversation writer and accepts no mutating command
while a turn is active. `AgentRegistry` routes the request by its already
resolved target, so the worker does not revalidate coordinator-owned request
and conversation invariants. It prepares an owning `RequestPayload` under
`ConversationReadView`, releases the lock, and calls the synchronous backend.

### Streaming success

Each transport fragment becomes `AgentDelta`. The first non-empty delta opens
the reserved agent entry as `streaming`; later deltas append to it. Deltas are
in-memory only.

On `AgentCompleted`, the coordinator rejects completion if no response entry
was opened. Otherwise it copies the open entry into the terminal persistence
record, commits the completed response and turn transition, marks the in-memory
entry complete, clears `ActiveTurn`, and requests a render. The open
conversation entry is the sole accumulated response buffer.

For non-streaming HTTP, the backend publishes the complete response as one
delta followed by `AgentCompleted`.

### Cancellation

`/stop`, Esc, or Ctrl-C while generating sets the active worker's atomic
cancellation flag. libcurl's progress callback observes it and aborts
cooperatively.

On `AgentCancelled`:

- if text was streamed, the coordinator commits a cancelled response and
  finishes the visible entry as cancelled;
- if no text was streamed, it commits only the cancelled turn transition.

The prompt remains in model context, but cancelled agent output does not.

### Failure

Transport errors, protocol errors, worker exceptions, dispatch failure, and a
successful terminal event without response text all become a failed turn.

The coordinator commits a typed error entry, discards any open partial agent
entry, adds the error to the transcript, and clears the active turn. Context
projection excludes both the error and the matching human prompt, so a failed
turn remains visible and durable without being replayed to a model.

Events with a request ID that does not match `ActiveTurn` are ignored. Request
IDs are therefore still required despite the single-active-turn policy:
asynchronous cancellation can leave late events in flight, and persistence
needs a stable turn correlation key.

## Commands and terminal behavior

When idle, the coordinator accepts:

- `/clear`: advance the durable history epoch, empty visible history, and reset
  addressing labels based on current roster size.
- `/agents`: show a transient status notice containing the current roster; `*`
  marks the run-local default.
- `/info`: show a transient status notice containing the current transcript
  entry count followed by the same roster information.
- `/@Name`: change the run-local default agent.
- `/stop`: report that no generation is active.
- `/exit`: request session termination.

Command output is never added to the conversation or session database. Unknown
commands and commands with unexpected arguments likewise produce transient
status notices. While a turn is active, only bare `/stop` is accepted; other
submitted text remains in the editor and produces a “Generation in progress”
notice.

`UserSession` is the UI state machine. It owns the `InputEditor`, applies
`CoordinatorUpdate` values, and coalesces rendering behind `render_needed`.
`SessionView` is its test seam; `Tui` is the ncurses implementation.

The input editor stores wide characters, supports cursor movement and editing,
and converts to UTF-8 on submission. A trailing backslash enters a visual
continuation line; visual newlines are removed from the submitted value.

Key behavior:

- Page Up/Down scroll by half a viewport.
- Esc while idle clears the editor and status notice.
- Ctrl-C while idle exits.
- Esc or Ctrl-C while generating requests cancellation.
- Resize signals reconfigure curses and force rendering.
- Closed stdin ends the session.

`Tui` renders a transcript pad, reverse-video status line, and persistent input
pad. `GenerationStatus` includes the active agent's display name. Human labels
are `[You]` or `[You → Name]`; agent labels always include the display name.
Addressed human labels are enabled whenever the current roster has multiple
agents, or when restored single-agent history contains another participant or
target. `/clear` forgets historical addressing evidence but keeps addressing
enabled for a currently multi-agent room.

`TranscriptRenderPlanner` compares snapshot revision, history epoch, width,
entry count, and the former last entry. It chooses no work, suffix append, or
full rebuild. `TranscriptViewport` follows output until the user scrolls and
clamps its position when content shrinks.

## Persistence

Each session is one self-contained
`rooms/<room>/sessions/<id>.sqlite3` database. Old `.data` and `.meta` files are
ignored.

The database uses:

- SQLite `application_id = 0x43484131` (`CHA1`);
- schema `user_version = 2`;
- strict tables;
- foreign keys enabled;
- a five-second busy timeout;
- `synchronous = FULL` for writable connections;
- default rollback-journal mode, not WAL.

The build pins SQLite 3.46.1. The schema contains:

| Table | Purpose |
| --- | --- |
| `session` | Singleton session `id`, `room`, and display `label`. |
| `state` | Singleton `history_epoch`, `next_entry_id`, and `next_request_id`. |
| `turns` | `request_id`, originating epoch, and lifecycle state. |
| `entries` | Typed transcript fields, target attribution, text, and terminal status. |

Turn states are `started`, `completed`, `cancelled`, and `failed`. A partial
unique index permits only one started turn in the entire session. Another
partial unique index permits only one human prompt per request. Full validation
also verifies that every turn has exactly one prompt in the same epoch.

Streaming status is never stored. A response row exists only after completion
or after cancellation with partial text. Errors are terminal `error` entries.

### Session listing and opening

`SessionRepository::list()` examines regular `.sqlite3` files, opens them
read-only, and performs lightweight identity/metadata validation: application
ID, schema version, embedded session ID versus filename, and embedded room
versus selected room. Broken candidates remain visible with an error so one bad
file does not hide healthy sessions. Session-ID filename validation is part of
that per-candidate error boundary, so even a malformed `.sqlite3` filename
cannot abort the whole listing.

Transcript-sized validation is deferred until selection. `main()` calls
`open_database_path()` for metadata identity and immediately calls
`load_conversation_state()`, which validates durable state, the turn/prompt
invariant, and each restored entry. Before accepting writes,
`ConversationJournal` validates database identity and structure; SQLite
constraints enforce entry semantics.

### Creation

New session IDs use local time:

```text
YYYY-MM-DD-HH-MM-SS-session
YYYY-MM-DD-HH-MM-SS-session-2
...
```

Creation writes a hidden temporary sibling, initializes schema and metadata in
one transaction, then publishes the final path with a hard link that cannot
replace an existing destination. The temporary path is removed whether
publication succeeds, collides, or throws. An empty user-supplied label falls
back to the generated ID.

### Transactions and IDs

Every semantic journal operation is one explicit transaction:

- `append`: standalone notice/error entry;
- `start_turn`: advance request ID, insert started turn, insert prompt;
- `complete_turn`: transition and insert completed response;
- `cancel_turn`: transition and optionally insert cancelled response;
- `fail_turn`: transition and insert error;
- `clear`: increment history epoch after confirming no turn is active.

`next_request_id` and `next_entry_id` are durable counters. Updates reject
reuse or out-of-order IDs. They do not reset on `/clear`, and gaps are valid
when an entry ID was reserved but never persisted.

Clearing does not delete old rows. It advances `history_epoch`; restore and new
entries use only the current epoch. Old history remains inspectable with SQLite
tools.

### Crash recovery

If the process exits after `start_turn` commits but before a terminal
transaction, SQLite retains a `started` row and its prompt. On restore:

1. SQLite rolls back any incomplete transaction.
2. `load_conversation_state()` loads current-epoch terminal entries and finds
   started turns by joining them to their human prompts.
3. It reserves an `InterruptedTurn` error attributed to the prompt's target:
   “Response interrupted before completion”.
4. `ChatCoordinator::initialize()` persists each repair with `fail_turn` before
   accepting another journal mutation, then adds the error to memory.

No streamed partial response is durable before its terminal transaction, so a
crash during streaming restores the prompt plus the interruption error.

### Persistence failure policy

Persistence errors are fatal for the current run. This is deliberate: after a
turn's terminal worker event has been consumed, continuing without its SQLite
transition would either diverge the visible transcript from durable state or
leave the coordinator active with no event left to complete it. The application
therefore does not convert database failures into ordinary transcript errors,
because writing such an error depends on the same unavailable journal.

Every coordinator journal mutation adds operation context before propagating
the failure. Turn-related messages identify the request and agent, while a
failed `/clear` identifies that operation. The chat loop preserves that
original exception, destroys its curses view, explicitly restores the terminal,
stops all workers, and then lets `main()` report the contextual failure.

SQLite transaction unwinding attempts a rollback. Since journal writes precede
their corresponding in-memory mutations, the database remains the authority:
a failed turn-start write never appears in memory, while a failed terminal
write leaves the durable turn in `started`. On the next open, normal crash
recovery reports that turn as interrupted. Even potentially transient failures
such as an exceeded busy timeout end the run rather than weakening the
durability invariant.

## Transport

`CompletionBackend` is a synchronous two-phase interface:

```text
prepare(request, locked conversation view) -> owning RequestPayload
perform(payload, delta callback, cancellation atomic) -> CompletionResult
```

This boundary lets tests inject backends while ensuring the conversation lock
cannot extend into transport work.

`CompletionClient` has two modes:

- `test`: return the submitted prompt as one delta without networking;
- `net`: use libcurl against an OpenAI-compatible API.

For each agent, configuration includes stable ID/name, host, port, mode, model,
streaming flag, optional temperature, API key or environment-based key,
optional reasoning effort, and HTTP/HTTPS selection. `api_key_env` overrides
`api_key` and must resolve to a non-empty environment value.

If a net-mode model is absent, construction performs:

```text
GET <scheme>://<host>:<port>/v1/models
```

and selects `data[0].id`. Discovery has ten-second connect and total timeouts.

Completions use:

```text
POST <scheme>://<host>:<port>/v1/chat/completions
```

The JSON body is built from the per-agent context projection and includes
model, stream mode, and configured optional generation fields. nlohmann/json
owns serialization and rejects malformed UTF-8 before dispatch. Bearer
authorization is included when configured.

Completion requests have a ten-second connection timeout, TCP keepalive, and
no total or low-speed timeout. Long generations are intentionally unbounded;
the user controls them through cooperative cancellation.

Streaming mode parses SSE `data:` events, forwards content deltas, requires a
`[DONE]` marker, rejects malformed JSON/protocol shapes and empty output, and
ignores data after completion. Non-streaming mode parses
`choices[0].message.content` and also requires non-empty text. Non-2xx bodies
are surfaced as protocol errors; libcurl failures are transport errors.

## Shutdown and failure handling

After a normal loop exit, `run_user()` calls `UserSession::shutdown()`. On an
exceptional exit it first preserves the original exception and destroys the
`Tui`, then restores the terminal and calls `ChatCoordinator::shutdown()`
directly. A shutdown exception does not replace the operation failure that
caused the exceptional exit. `ChatCoordinator::shutdown()` is idempotent:

1. `AgentRegistry::stop()` cancels all workers.
2. Each worker closes its request channel, joins its thread, and becomes
   permanently stopped.
3. After all workers stop, the registry closes the shared event channel.
4. The coordinator drains remaining queued events so a final cancellation or
   completion receives its durable terminal transition.

Destructors call their shutdown paths defensively. `AgentWorker` makes a final
join attempt if ordinary shutdown throws, preventing destruction of a joinable
`std::thread`.

Database writes remain on the main thread. A journal opens existing databases
read-write without `SQLITE_OPEN_CREATE`; only session creation may initialize a
database, so a mistaken path cannot silently become an empty session.

## Component map

| Component | Responsibility |
| --- | --- |
| `main.cpp` | Composition root and startup/session-selection workflow. |
| `environment.*` | Optional `.env` loading. |
| `workspace.*` | Room/persona list parsing and safe path resolution. |
| `startup_selector.*` | Temporary ncurses room/session/name selection screens. |
| `agent_definition.*` | Config/prompt loading for an ordered room roster. |
| `agent_identity.*` | Stable ID and display-name validation. |
| `agent_roster.*` | Non-empty immutable agent metadata and handle resolution. |
| `session_repository.*` | Session enumeration, metadata checks, creation, and path resolution. |
| `session_database.*` | SQLite schema, restoration, journal transactions, and crash repair data. |
| `conversation.*` | Typed, thread-safe transcript and streaming-entry state. |
| `agent_context.*` | Per-agent projection from transcript semantics to model roles. |
| `agent_protocol.h` | Request/event values and their typed channels. |
| `agent_registry.*` | Ordered worker ownership, routing, shared events, and shutdown. |
| `agent_worker.*` | One blocking completion thread and request gate per agent. |
| `completion_backend.h` | Injectable two-phase completion boundary. |
| `completion_client.*` | Test echo mode, request serialization, libcurl, SSE/JSON parsing. |
| `chat_coordinator.*` | Commands, addressing, active turn, transcript/database lifecycle. |
| `user.*` / `user_events.*` | Main `poll` loop and descriptor readiness. |
| `user_session.*` | Testable UI state machine and render coalescing. |
| `session_view.h` / `tui.*` | View seam and ncurses implementation. |
| `input_editor.*` | Wide-character editing and UTF-8 submission. |
| `transcript_renderer.*` | Entry labels, incremental render planning, layout, viewport. |
| `terminal.*` | Process-wide ncurses lifecycle and mode changes. |
| `command.*` / `mention.*` | Slash-command and leading-address parsing. |
| `text.*` / `path_name.*` | Shared text and path utilities. |

Everything except `main.cpp` is compiled into `cha_core`.

## Key invariants

1. A roster is non-empty, ordered, and fixed for one run.
2. Agent IDs and ASCII-folded display names are unique.
3. Roster slot `i` and worker slot `i` describe the same agent.
4. At most one turn is active across the session.
5. At most one request is outstanding per worker.
6. At most one streaming conversation entry exists, and it is the last entry.
7. The main thread is the only conversation and database writer.
8. Backend preparation holds a read view; backend performance never does.
9. Registry routing sends a request to exactly one worker while the active-turn
   state prevents intervening conversation mutations.
10. Request and entry IDs are positive, strictly increasing, and never reused.
11. A durable started turn has exactly one human prompt in its originating
    epoch.
12. Streaming entries are never persisted.
13. Completion, cancellation, and failure transition only the currently
    started turn.
14. Database state is committed before the corresponding in-memory terminal
    mutation.
15. Failed turns and cancelled output are excluded from model context according
    to their distinct rules.
16. Sessions bind to a room, not to the room's current roster.
17. The shared event channel closes only after every worker has stopped.
18. A journal mutation failure ends the current run; the application never
    continues with transcript state it could not persist.

## Build and testing

CMake builds:

- `cha_sqlite3`: pinned SQLite amalgamation;
- `cha_core`: all application logic except the composition root;
- `cha`: terminal application;
- `cha_tests`: discovered unit tests;
- `itest`: separately invoked integration executable.

The build requires wide ncurses and threads. It uses an installed libcurl when
available, otherwise fetches pinned curl 8.14.1. It also fetches nlohmann/json
3.11.3, toml++ 3.4.0, SQLite 3.46.1, and GoogleTest 1.15.2 for tests. The
bundled curl enables OpenSSL when it is available.

`make test` builds and runs the unit suite through CTest. Unit tests cover
configuration, identity, roster/mention routing, registry and worker behavior,
conversation/context semantics, coordinator lifecycle, SQLite constraints and
recovery, workspace/session handling, transport parsing, event channels,
input/UI state, and incremental rendering.

`make itest` runs the separate integration binary from the checked-in
`workspace/`. Its tests cover live configured streaming, non-streaming, and
cancellation paths plus local mock-server multi-agent routing, per-agent
context, persistence, and reopening after a roster change.

The principal test seams are:

- `CompletionBackend`, for coordinator/worker tests without a network;
- `SessionView`, for `UserSession` tests without curses;
- `project_agent_context()`, whose materialized messages expose projected
  boundaries, roles, and content directly.

Persistence, conversation logic, parsing, rendering plans, and other local
components are concrete and tested directly.
