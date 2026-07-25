# Source tree — architecture

This document explains how `cha` is built: what the layers are, what each one
owns, how a message travels from a keystroke to a model and back, and which
rules must hold as the code grows. Read it before changing anything structural.

Per-directory `README.md` files describe each layer in detail; the exhaustive
rule-by-rule reference lives in [`docs/cha.md`](../docs/cha.md); the user-facing
manual is the [top-level `README.md`](../README.md).

## System overview

`cha` is a terminal chat client for OpenAI-compatible chat-completion servers.
It runs inside a workspace containing persona definitions and rooms. A persona
definition contains an identity, model configuration, and base system prompt. A
room contains an ordered list of the personas participating in chats in that
room, together with a room-specific system-prompt extension. When the room is
loaded, that extension is appended to each participating persona's base system
prompt.

During a run, the participating personas are represented as agents. Each agent
has its own identity, effective system prompt, and model connection, while all
agents use the session's shared chat transcript. The first agent in the room's
list is the default; a prompt beginning with `@Name` is sent to another agent.

A session is a persistent chat within a room, stored in a single SQLite
database. For each turn, `cha` records the user's prompt before sending it to
the chosen agent on a worker thread. Model output is streamed into the chat
transcript, and the outcome of the turn—completion, cancellation, or failure—is
recorded in the database.

## Layer map

The tree is organized by responsibility. The directories are *not* separate
libraries: CMake compiles every production translation unit except
`apps/tui_main.cpp` into one `cha_core` target, so the boundaries are
architectural, enforced by review and by the include rules below.

| Directory | Owns | Must not know about |
| --- | --- | --- |
| `util/` | Leaf helpers: text and path rules, `.env` loading, the pollable `EventChannel`. | Anything above it. |
| `transcript/` | The transcript model: entry types, validation, and the thread-safe live `Transcript`. | Storage, providers, terminals. |
| `agents/` | Persona config, the room roster, model-context projection, the execution thread, and HTTP transport. | Workspace layout, sessions, front ends. |
| `session/` | Workspace and session operations, SQLite persistence, and live chat coordination. | Terminals, command syntax, transports. |
| `ui/text/` | The textual grammar: slash commands and `@mention` addressing. | Curses, storage, backends. |
| `ui/terminal/` | Terminal lifecycle, startup selection, input editing, rendering, and the event loop. | Workspace files, session repositories, backends. |
| `apps/` | Executable composition roots and process-level error handling. | Reusable policy — it only wires. |

## Dependency direction

Dependencies run one way, from the top of this table to the bottom. A directory
may include headers only from those listed beside it.

| Directory | May include |
| --- | --- |
| `apps/` | `ui/terminal/`, `session/`, `util/` |
| `ui/terminal/` | `ui/text/`, `session/`, `transcript/`, and roster values from `agents/` |
| `ui/text/` | `session/`, `util/` |
| `session/` | `agents/`, `transcript/`, `util/` |
| `agents/` | `transcript/`, `util/` |
| `transcript/` | Nothing in the project. |
| `util/` | Nothing in the project. |

Three rules keep this direction honest:

1. **`ui/` calls `SessionController` and `Workspace`, never storage or
   transport.** A front end may render `TranscriptEntry` values and call
   `SessionController`, but it must not open a session repository, read
   workspace files, or call a completion backend. If a front end needs something
   new, add an operation to `session/`.
2. **`transcript/` depends on nothing.** It may not import SQLite, provider
   protocol, or terminal concepts. Everything else may depend on it.
3. **`agents/` owns persona loading but not workspace discovery.** Once
   `session/` has resolved which directories a room uses, `agents/` loads
   `Config` and `AgentDefinition` from them.

## Runtime structure

One process, two long-lived threads. The main thread owns all transcript and
database mutation; the agent thread performs one blocking completion at a time
and reports back through a channel.

```mermaid
flowchart LR
    subgraph mainthread["Main thread — UI, state, persistence"]
        loop["run_user<br/>poll on stdin + eventfd"]
        usession["UserSession<br/>+ InputEditor"]
        tui["Tui — curses"]
        controller["SessionController"]
        conv["Transcript"]
        journal["SessionJournal<br/>SQLite"]
        registry["AgentRegistry<br/>main-thread handle"]
    end

    subgraph agentthread["Agent thread — one request at a time"]
        dialog["AgentRegistry::dialog"]
        backend["CompletionBackend<br/>CompletionClient + libcurl"]
    end

    provider[("Model server<br/>/v1/chat/completions")]

    loop --> usession
    usession --> tui
    usession --> controller
    controller --> conv
    controller --> journal
    controller --> registry
    registry -->|"WorkItem queue"| dialog
    dialog --> backend
    backend <-->|"HTTP or SSE"| provider
    dialog -->|"AgentEvent channel"| loop
    dialog -.->|"short-lived read view"| conv
```

Ownership is a strict tree, and destruction order matters:

- `main()` owns the `Workspace`, the process-wide `Terminal`, and the selected
  `SessionController`.
- `run_user()` owns the `Tui` and the `UserSession` for the chat loop.
- `SessionController` owns the `Transcript`, the `SessionJournal`, the
  `AgentRegistry`, the current default agent, and the state of the in-flight
  turn. The transcript is declared *before* the registry so it outlives the
  thread that reads it.
- `AgentRegistry` owns the roster, every backend, the request channel, the event
  channel, the worker thread, and the cancellation and outstanding-request
  atomics.

### How the two threads talk

`EventChannel<T>` (see [`util/`](util/README.md)) is a mutex-protected queue
paired with a Linux `eventfd`. There are two channels:

- a private `EventChannel<WorkItem>` carrying routed requests **to** the agent
  thread;
- the shared `AgentEventChannel` carrying `AgentEvent` values **back**. Its
  descriptor is what `run_user()` polls next to stdin, so streamed output and
  keystrokes wake the same loop with no timers and no busy-waiting.

Submission claims a single atomic gate, so at most one request is outstanding
across the whole roster; a second submission is refused even for a different
agent. The agent thread clears the gate immediately *before* publishing a
terminal event, so the main thread can start the next turn as soon as it sees
one. Cancellation is a shared atomic flag checked before `prepare()`, and by the
libcurl progress callback during transfer.

## Lifecycle: startup

```mermaid
sequenceDiagram
    autonumber
    participant main as tui_main
    participant ws as Workspace
    participant sel as StartupSelector
    participant repo as SessionsRepository
    participant db as Session database
    participant controller as SessionController

    main->>main: load_dotenv
    main->>ws: construct, require personas/ and rooms/
    main->>ws: rooms
    ws-->>main: room names from rooms/rooms.list
    main->>sel: select_room
    sel-->>main: chosen room
    main->>ws: sessions of room
    ws->>repo: list
    repo->>db: read metadata, validate id and room
    repo-->>ws: Session rows
    ws-->>main: SessionSummary rows
    main->>sel: select_session
    alt New session
        sel-->>main: empty id
        main->>sel: prompt_session_name
        main->>ws: create_session
        ws->>repo: create, temp file then link
    else Existing session
        sel-->>main: session id
        main->>ws: open_session
        ws->>db: load_session_state
        db-->>ws: SessionRestore
    end
    ws->>controller: build with AgentDefinitions and database path
    controller->>controller: restore entries, repair interrupted turns
    ws-->>main: SessionController
    main->>controller: run_user
```

Persona loading happens here, on the main thread: each `CompletionClient` is
constructed — including optional `/v1/models` discovery when `model` is unset —
*before* the agent thread starts. After that point only the agent thread touches
a backend, which is why the clients need no internal locking.

## Lifecycle: one turn

This is the path every prompt takes.

```mermaid
sequenceDiagram
    autonumber
    participant U as UserSession
    participant T as handle_text_input
    participant C as SessionController
    participant J as SessionJournal
    participant V as Transcript
    participant G as AgentRegistry
    participant W as Agent thread
    participant P as Model server

    U->>T: submitted line
    T->>T: parse_command, then parse_addressed_prompt
    T->>C: submit_prompt text and handle
    C->>C: reject if a turn is active
    C->>C: resolve handle against AgentRoster
    C->>J: start_turn, SQLite transaction
    C->>V: add human entry
    C->>G: submit CompletionRequest
    G->>W: WorkItem via request channel
    C-->>U: SessionUpdate, render and clear input

    W->>V: short-lived read view
    W->>W: backend.prepare, project context and build body
    W->>P: POST /v1/chat/completions
    loop streamed fragments
        P-->>W: SSE delta
        W->>G: AgentDelta on event channel
        G-->>U: eventfd wakes poll
        U->>C: receive
        C->>V: begin or append streaming entry
        U->>U: render
    end
    P-->>W: DONE marker
    W->>G: AgentCompleted
    U->>C: receive
    C->>J: complete_turn, SQLite transaction
    C->>V: finish entry as complete
    C-->>U: SessionUpdate, render
```

Two ordering rules are load-bearing:

- **Durable before visible.** The prompt is written to the journal before it is
  added to the in-memory transcript, and a response is committed before its
  entry is finalized. A crash can therefore lose a turn, but can never show one
  that was not recorded.
- **One writer.** Every mutation above happens on the main thread. The agent
  thread only reads the transcript, and only through a short-lived locked
  view taken before any network I/O.

## Turn states

There are two kinds of state, with different purposes:

- The database records the durable state of a turn. A turn is first recorded as
  `started` and then receives exactly one final outcome: `completed`,
  `cancelled`, or `failed`.
- While a response is in progress, the application also tracks a live
  `ResponsePhase`: `waiting`, `reasoning`, or `answering`. This phase drives
  `GenerationStatus` and the status-line text (`generating`, `reasoning`, or
  `responding`). It is not stored as part of the turn.

The response phase changes as output arrives. A new request begins in
`waiting`. The first reasoning fragment changes the phase to `reasoning`, but
reasoning is temporary UI state: it is not added to the chat transcript or
saved in the database. The first answer text changes the phase to `answering`
and opens a streaming answer entry in the transcript. Later reasoning does not
move the phase back from `answering`.

The final outcome is handled as follows:

| Event | Transcript result | Durable turn outcome |
| --- | --- | --- |
| The agent completes after producing answer text | The answer entry is finalized | `completed` |
| The agent reports completion without producing answer text | An error entry is added | `failed` |
| The user cancels before answer text arrives | No response entry is added | `cancelled` |
| The user cancels after some answer text arrives | The partial answer is retained and marked as cancelled | `cancelled` |
| The agent fails | Any partial answer is removed and an error entry is added | `failed` |
| The process stops before recording a final outcome | On the next open, an interruption error is added | Repaired from `started` to `failed` |

Error entries and cancelled partial answers remain visible, but are excluded
from the context sent to agents on later turns. The prompt from a failed turn
is also excluded; the prompt from a cancelled turn remains part of the chat
context.

## What the model sees

The transcript is not sent verbatim. `project_agent_context()` in `agents/`
rebuilds a per-agent view of it for each request:

- the agent's own effective system prompt comes first;
- notices, errors, and any still-open streaming entry are dropped;
- human prompts belonging to a failed turn are dropped with it;
- only `complete` agent entries with non-empty text survive; cancelled and
  failed answers stay on screen but never become history;
- the requesting agent's own answers become `assistant` messages; another
  agent's answers become `user` messages prefixed with that agent's name;
- when the transcript involves more than the requesting agent, human turns are
  prefixed `User:` and marked `[to Name]` when they were addressed elsewhere;
- consecutive `user` messages produced by this attribution are coalesced.

Reasoning text is never projected and never stored. It exists only in the
active response while its turn is running and never enters the transcript.

## What is stored

One session is one self-contained SQLite file under
`rooms/<room>/sessions/<id>.sqlite3`, carrying its own identity so it can be
validated before use, and a `history_epoch` so `/clear` can start a fresh
visible history without deleting anything.

```mermaid
erDiagram
    session {
        int singleton PK
        text id
        text room
        text label
    }
    state {
        int singleton PK
        int history_epoch
        int next_entry_id
        int next_request_id
    }
    turns {
        int request_id PK
        int epoch
        int state "0 started, 1 completed, 2 cancelled, 3 failed"
    }
    entries {
        int entry_id PK
        int epoch
        int request_id FK
        int kind "0 human, 1 agent, 2 notice, 3 error"
        text participant_id
        text display_name
        text addressed_to
        text text
        int status
    }
    turns ||--o{ entries : "prompt and response"
```

Schema `CHECK` constraints re-state the entry-model rules in SQL, a partial
unique index allows only one `started` turn at a time, and another allows only
one prompt per turn. Streaming entries are rejected by construction; reasoning
never enters a transcript entry. See [`session/README.md`](session/README.md) for the full
persistence contract.

## Invariants

These hold across the whole tree. Breaking one is a design change, not a bug fix.

| Invariant | Enforced by |
| --- | --- |
| A roster is non-empty, with unique IDs and unique case-folded names. | `AgentRoster` constructor |
| At most one turn is in flight, process-wide. | `AgentRegistry` outstanding-request gate |
| Every accepted request yields exactly one terminal `AgentEvent`. | `AgentRegistry::dialog` and shutdown order |
| Only the main thread mutates `Transcript` or the journal. | `SessionController` |
| At most one streaming entry is open at a time. | `Transcript` |
| Entry and request IDs are positive and strictly increasing. | `Transcript::require_next_id`, `state` table |
| Durable writes precede visible ones. | `SessionController` |
| Reasoning exists only while a response is active; it never enters the transcript, persistence, or projection. | `ActiveResponse`, `GenerationStatus`, `TranscriptEntry` shape |
| Front ends never open storage or call backends. | Include rules above |

## Failure policy

- **Persistence failures are fatal to the session.** A failed journal write is
  rethrown with context; the session ends rather than continuing with a
  transcript the database does not agree with.
- **Provider failures are ordinary events.** A transport or protocol error
  becomes `AgentFailed`, an error entry, and a notice; the session continues.
- **Startup failures abort.** A malformed workspace, room, persona, or session
  database throws before any chat UI is drawn.
- **Terminal restoration wins.** `run_user()` restores the terminal before
  rethrowing, so a stack trace never lands on a screen still in curses mode.
- **Error messages never carry model output.** Streaming protocol errors report
  sanitized status, content type, and byte counts only.

## Extending the system

| Goal | Where the work goes |
| --- | --- |
| New provider or protocol | A new `CompletionBackend` in `agents/`; nothing above it changes. |
| New slash command | `ui/text/command.*` for the grammar, plus an operation on `SessionController` if it touches session state. |
| New front end, e.g. HTTP | A new `ui/http/` front end plus a new `apps/` entry point, reusing `Workspace` and `SessionController` unchanged. |
| New persisted field | `transcript/` entry model, its validators, the SQLite schema and its `CHECK` constraints, and the restore path — in that order. |
| New rendering behavior | `ui/terminal/transcript_renderer.*`, testable through `TranscriptSurface` without curses. |

## Build and test map

| Target | Contents |
| --- | --- |
| `cha_core` | Every production source except `apps/tui_main.cpp`. |
| `cha` | The terminal application: `cha_core` plus `apps/tui_main.cpp`. |
| `cha_tests` | Unit and component tests under `tests/`, mirroring this tree. Run with `make test`. |
| `itest` | Live integration tests driving the real stack against the checked-in `workspace/`. Run with `make itest`; not part of `make test`. |

Third-party dependencies are libcurl, nlohmann/json, toml++, SQLite
(amalgamation), wide ncurses, and GoogleTest — vendored through CMake
`FetchContent` when not already installed.

## Source conventions

- Project headers are included from the `src/` include root, for example
  `"session/session_controller.h"`.
- Headers include what their own declarations need; no reliance on transitive
  includes.
- Every class and struct carries a comment saying why it exists, what it does,
  and which project types it collaborates with.
- Public data types live with the behavior that owns their meaning: agent
  protocol and roster types in `agents/`, session and journal types in
  `session/`, transcript semantics in `transcript/`.
- Tests mirror this tree under `tests/`; cross-layer scenarios go in
  `tests/integration/`.

## Where to read next

| Layer | Document |
| --- | --- |
| Transcript model | [`transcript/README.md`](transcript/README.md) |
| Agent runtime and transport | [`agents/README.md`](agents/README.md) |
| Operations and persistence | [`session/README.md`](session/README.md) |
| UI contract | [`ui/README.md`](ui/README.md) |
| Command and mention grammar | [`ui/text/README.md`](ui/text/README.md) |
| Terminal front end | [`ui/terminal/README.md`](ui/terminal/README.md) |
| Entry points | [`apps/README.md`](apps/README.md) |
| Shared helpers | [`util/README.md`](util/README.md) |
| Exhaustive design rules | [`../docs/cha.md`](../docs/cha.md) |
