# cha — Internal design

This document describes the current implementation of `cha`, a C++20 terminal
client for OpenAI-compatible chat-completion servers. It is the exhaustive
rule-by-rule maintenance reference.

- For a diagrammed overview of the layers, threads, and the life of a turn,
  start with [`src/README.md`](../src/README.md); each source directory has its
  own README beneath it.
- For user-facing setup and commands, see [README.md](../README.md).

- [Architecture](#architecture)
- [Source organization and dependency rules](#source-organization-and-dependency-rules)
- [Startup and workspace loading](#startup-and-workspace-loading)
- [Multi-agent model](#multi-agent-model)
- [Transcript and context projection](#transcript-and-context-projection)
- [Turn lifecycle](#turn-lifecycle)
- [Commands and terminal behavior](#commands-and-terminal-behavior)
- [Persistence](#persistence)
- [Transport](#transport)
- [Shutdown and failure handling](#shutdown-and-failure-handling)
- [Component map](#component-map)
- [Key invariants](#key-invariants)
- [Build and testing](#build-and-testing)

## Architecture

Both frontends use two long-lived application threads: the main/UI thread and
one shared agent-execution thread. The main thread owns all transcript and
database mutation. The execution thread prepares and performs one blocking
completion at a time, then returns typed events through one shared channel.

```text
Startup
  Workspace + TUI selector or console CLI
                                      |
                                      v
Main/UI thread                  SessionController
  UserSession or             +-- Transcript
  ConsoleSession ----------->+-- SessionJournal (SQLite)
  Tui or console stream       +-- ForumPersonas
       ^                      +-- AgentRegistry
       | SessionUpdate        |       |
       |                      |       +-- AgentRuntimeInfo
       |                      |       +-- WorkItem request queue
       |                      |       +-- shared AgentEvent queue
       |                      |       +-- agent-execution thread
       |                      |               |
       |                      |               +-- CompletionClient[0]
       |                      |               +-- CompletionClient[1]
       |                      |               +-- ...
       |                      |
       |                      +-- ActiveResponse (optional)
       |
       +---- libuv loop (stdin, signals, async agent wake)
```

The ownership graph is:

- Each entry point owns the `Workspace` and selected `SessionController`. The
  TUI owns `Terminal` and `StartupSelector`; the console owns `SystemConsole`,
  and `TranscriptEmitter`.
- `run_user()` owns a `Tui` and `UserSession`; `ConsoleSession::run()` owns the
  line queue and console lifecycle.
- `SessionController` owns the in-memory `Transcript`, SQLite
  `SessionJournal`, `AgentRegistry`, `ForumPersonas`, the current default agent
  ID, the next entry and request IDs seeded from durable state, and the
  optional `ActiveResponse` describing the turn in flight.
- `AgentRegistry` owns public runtime information and a backend for each forum
  persona, one request queue, one shared agent-event queue, one execution
  thread, and shared cancellation/outstanding-request atomics.
- Each production backend is a `CompletionClient` with one reusable libcurl easy
  handle and one agent-specific system prompt and configuration.

The registry holds a non-owning reference to `Transcript`. The transcript
must outlive the registry and its joined execution thread. `SessionController`
satisfies this through member declaration order: its transcript is declared
before its registry and therefore destroyed afterward.

Backend construction and optional model discovery happen on the main thread
before the execution thread starts. After startup, only the execution thread
calls backend `prepare()` or `perform()`; the clients therefore need no
internal synchronization.

### Thread communication

`ConcurrentQueue<T>` is a portable mutex-protected `std::deque`. The two queue
directions are:

- One private `ConcurrentQueue<WorkItem>` carries routed requests from the main
  thread to the execution thread.
- The execution thread publishes `AgentEvent` values into the registry's
  shared `ConcurrentQueue<AgentEvent>`.

`submit()`, `cancel()`, and `stop()` are externally serialized main-thread
control operations. Submission resolves a backend slot before atomically
claiming one global outstanding-request gate, resets the shared cancellation
flag, and enqueues an owning `WorkItem {backend_index, request}`. A second
submission is rejected while that gate remains claimed, even when it targets a
different backend.

The execution thread blocks on the request queue, resolves the backend vector
slot once, and performs one request to completion. The gate means the otherwise
unbounded request queue has a logical capacity of one. The execution thread
clears the gate immediately before publishing a terminal event, so once the
main thread observes completion it can submit the next request without racing
the previous turn's cleanup.

After each successful event push, the registry calls an injected
`WakeNotifier`. Both terminal frontends own a `UvEventLoop`; the registry wakes
its `uv_async_t` while frontend-owned stdin and signal handles share the same
loop. Input and streamed output therefore wake one event loop without timers or
busy polling. The console may pause pipe input for queue backpressure.

The transport maps output into
`CompletionDelta { kind = reasoning | answer, text }`. The registry attaches
request identity without merging or reordering fragments. The event variant
contains:

- `AgentDelta { request_id, kind, text }`
- `AgentCompleted { request_id }`
- `AgentCancelled { request_id }`
- `AgentFailed { request_id, message }`

`Transcript` is mutex-protected. The main thread is its sole writer.
The registry's execution loop obtains a short-lived `TranscriptReadView`
while preparing a request. That view holds the mutex and exposes a non-owning
span; it is destroyed before network I/O and before any delta is published.

## Source organization and dependency rules

The source tree is organized by responsibility. Reusable and console sources
are linked into static `cha_core`; curses-specific sources are linked into
static `cha_tui`. This makes the console and integration targets independent of
ncurses.

```text
src/
  util/                  shared low-level helpers
  transcript/            transcript and turn value model
  agents/                agent metadata, execution, and provider transport
  session/               reusable operations, transcript coordination, and session persistence
  ui/
    text/                 slash-command and leading-mention grammar
    render/               shared transcript labels and writing
    tui/                  ncurses frontend and screen flow
    console/              line-oriented frontend and append-only stream
  apps/                   executable composition roots
```

Project includes are qualified from the `src/` include root, for example
`"transcript/transcript.h"` and `"session/session_controller.h"`.
The intended dependency direction is below; an arrow means “depends on”:

```text
apps
  |--> ui/tui -----+
  |                +--> ui/render
  |--> ui/console -+--> ui/text --> session
  |                +--> session
  |                +--> transcript
  |
  `--> session ------+--> agents -------> transcript
                     `--> transcript

agents, ui/text, session, and apps may also depend on util.
```

More precisely:

- either frontend, and any future HTTP frontend, may call `session/` and
  read presentation-safe values from `transcript/`;
- `ui/tui/` and `ui/console/` may share `ui/render/` and `ui/text/`, but may
  not include one another;
- a front end does not access agent execution or session catalogs directly;
- `session/` coordinates `agents/` and `transcript/`, and owns session persistence;
- `agents/` may read transcript state but does not depend on `session/` or `ui/`;
- `transcript/` and `util/` do not depend on higher layers.

When the web front end is added, a `ui/http/` front end can translate
HTTP requests and responses around the same session-layer operations.
It should not load workspace files, open catalogs, or invoke completion
backends directly.

The renderers consume `TranscriptSnapshot`, `TranscriptEntry`,
`GenerationStatus`, and `ForumPersonas` directly. Mirroring these into
presentation types would add no useful isolation. `ForumPersonas` tells it when
transcript labels should name the addressed agent. Storage-specific values do
not cross the session-layer boundary:
`Workspace` converts catalog `Session` records to `SessionSummary` before a
selector or future HTTP front end sees them.

The tests mirror the source organization. Cross-layer behavior belongs in
`tests/integration/`; focused front-end behavior such as textual dispatch
belongs under `tests/ui/`, while controller tests call its session-layer methods
directly.

## Startup and workspace loading

The TUI entry point and `Workspace` perform these steps:

1. Load optional `.env` values without replacing existing process variables.
2. Construct `Workspace`, which requires `forums/` to exist, and
   `Terminal`.
3. Let `StartupSelector` choose a forum from `Workspace::forums()`.
4. Let the selector choose an existing session or **New session** from
   `Workspace::sessions(forum)`; a chosen row carrying a validation error aborts.
5. Call `Workspace::create_session()` with a prompted label, or
   `Workspace::open_session()` with the chosen ID. Either call resolves the
   forum, loads its ordered agent definitions, and resolves the database path
   through `SessionCatalog`. Creation returns `CreatedSession`, containing the
   controller and the exact ID assigned during collision-safe publication; the
   TUI uses the controller and discards the ID.
6. `open_session()` additionally calls `load_session_state()` to fully
   restore the database before construction.
7. Construct `SessionController` from the definitions, database path, and
   restore result; production backend construction may perform model discovery
   before the execution thread starts.
8. Enter `run_user()`.

Cancelling either selector is an error rather than a silent exit: `main()`
throws, and the failure is reported after terminal restoration.

The console entry point replaces steps 2–4 with command-line parsing. It
supports forum and session listings, performs read-only forum validation with
`--forum ID --check`, opens `--session ID`, or creates a session for
`--new LABEL` (and creates one with a default label when neither selection
option is present). A check loads the effective persona configurations, expands
all prompts, and validates persona identity and uniqueness, but does not inspect
stored sessions, create a session, resolve `api_key_env`, initialize providers,
discover a model, or access the network. `--list-forums` returns before all
selection validation; `--list-sessions` returns before session selection but
conflicts with `--check`. For an interactive run, the ready banner reports the
resolved ID from `CreatedSession`, so a new session can be reopened later. The
entry point then creates a `SystemConsole` with its libuv loop, a
`TranscriptEmitter`, and a `ConsoleSession`. Usage failures return 2; workspace
and runtime failures return 1.

The workspace shape is:

```text
workspace/
  .env                         optional
  forums/
    <forum>/                   distribution unit and template containment root
      config.toml              required display_name; optional [prompt]
      USER.md                  template-expanded forum prompt extension
      personas/
        base_config.toml       optional forum persona configuration + [prompt]
        shared prompt files    optional includes (e.g. character-voice.md)
        <persona>/
          config.toml
          SYSTEM.md            template-expanded persona prompt
      sessions/                 optional until a session is created
        <session-id>.sqlite3
```

Each immediate `forums/` subdirectory is a forum, and they are listed in
lexicographic name order. Each forum's `personas/` subdirectories are its
personas and are likewise loaded in lexicographic name order. Each forum must
contain at least one persona directory. `Workspace::load_forum` resolves those
directories and reads the required `display_name` from `config.toml`; persona configuration is checked when
`Workspace` resolves each entry.

`Config` and `AgentDefinition` are agent-owned value types. Their disk loaders
live under `agents/`. `Workspace` resolves the selected forum's optional
`personas/base_config.toml` and passes it explicitly to
`load_agent_definitions()`; the agent layer does not infer the workspace
layout. `load_config()` parses the base and persona files as typed partial
configurations, overlays them, and validates the effective result. Precedence
is built-in defaults, then the base file, then the persona file. In the current
format, the persona directory supplies the stable ID and its config defines
`display_name`; persona identity is forbidden in the base file. For backward
compatibility, a persona file may use `name` when `display_name` is absent and
may use `id` to override the directory-derived ID. Omitting any other persona
field inherits the base or built-in value; there is no separate syntax for
clearing an inherited optional value.

`load_agent_definitions()` expands each persona's `SYSTEM.md` and the selected
forum's `USER.md` through the prompt template engine (`util/text_template.*`),
preserves persona-list order, and appends a generated forum-context section to
each effective system prompt. Expansion supports `$$(path)` includes (contained
under the forum directory) and `$${name}` variables from reserved loader names
and `[prompt]` tables. Initial prompt-variable precedence is base then persona.
For each expanded file, `[prompt]` in an adjacent `config.toml` overlays the
inherited scope for that file and its descendants; reserved values cannot be
shadowed. Because expansion is per persona, `USER.md` may differ across
personas when it references `persona.*` or `[prompt]` values. Expansion
enforces forum containment, include-cycle, depth, count, and output-size
limits. The generated forum-context section identifies the current agent,
lists the other current personas, and defines the JSON Lines representation
used for shared history.

`Workspace` is the only entry point into workspace layout. It is a thin value
over the root path and holds no cached forum or persona state, so every operation
resolves what it needs when it is called:

- `forums()` enumerates and sorts the `forums/` subdirectories;
- `load_forum()` resolves one forum directory and its ordered persona directories;
- `check_forum()` loads every effective definition and validates the resulting
  persona set without creating a session or provider;
- `sessions()` builds a `SessionCatalog` for the forum and maps its `Session`
  records to `SessionSummary` values;
- `create_session()` and `open_session()` load the forum's complete ordered agent
  definitions, resolve the database path, and construct the
  `SessionController`; creation returns it with the assigned ID in
  `CreatedSession`, while opening returns the controller directly.

Persona files are therefore read once per session create/open or explicit
forum check rather than once per selection, and catalog paths and `Session`
records never leave the session-layer boundary — `StartupSelector` sees only
forum names and `SessionSummary` values.

`ForumPersonas` is the session-layer boundary for a non-empty ordered group and
for lookup and handle resolution. `AgentRegistry` separately validates the
identity in backend-provided `AgentRuntimeInfo`, including when tests inject
backends without workspace loading.

Agent IDs are non-empty ASCII letters, digits, underscores, or hyphens. Display
names are non-empty, cannot start or end with whitespace, cannot begin with `@`
or `/`, and cannot equal `User` under ASCII case folding. Internal whitespace
is allowed, and handle lookup can resolve a unique word or word prefix within a
multi-word display name.

Sessions are forum-scoped and do not capture the forum's current personas.
Session metadata contains the session ID, forum, and display label, but no
persona collection. A session can be reopened after personas have been renamed,
removed, or added; persisted participant IDs, display names, and prompt targets
preserve historical attribution.

## Multi-agent model

`ForumPersonas` is ordered and fixed for one run. Its first persona supplies the
initial default agent. `/@Name` changes the default in memory for the current
run only.

Only one turn may be active across all forum personas. The application does not
run simultaneous answers. This preserves one linear transcript, one streaming
entry, and one unambiguous cancellation target. A global registry gate also
rejects a second outstanding request when the registry is tested or used
outside the controller.

### Prompt addressing

The shared text grammar's `parse_addressed_prompt()` recognizes an optional
leading mention after leading whitespace:

- `@Name prompt` addresses `Name` and stores only `prompt`.
- No mention uses the current default agent.
- `@@text` removes one `@` and submits literal `@text` to the default agent.
- Ordinary input, including its whitespace, is preserved.

Handle resolution tries, in order:

1. exact display name, ASCII-case-insensitively;
2. exact name after removing trailing `,.;:!?`;
3. a unique ASCII-case-insensitive prefix.

The text grammar passes prompt text and the optional handle to
`SessionController::submit_prompt()`. A future HTTP front end can provide those
fields directly without parsing terminal syntax. Unknown and ambiguous handles
are rejected with a notice that lists the forum's personas. The input is not
cleared, so the user can correct it. A mention with an empty body is also
rejected without clearing the input.

The resolved target is stored on the human transcript entry as both:

- `addressed_to`: immutable agent ID, used for routing and context semantics;
- `addressed_to_name`: display name at submission time, used for restored UI
  labels and target attribution.

`AgentRegistry` keeps backends and their `AgentRuntimeInfo` values in the same
order. Submission linearly scans the small runtime-information collection to
select the corresponding backend slot; there is no parallel index structure.
Cancellation needs no target because only one request can be queued or
executing, and the cancellation flag cannot be reset until that request
releases the global gate.

## Transcript and context projection

### Typed transcript

`TranscriptEntry` is the common record used by rendering, persistence, and
model-context projection:

```text
id
kind                  human | agent | notice | error
participant_id
display_name
addressed_to          human entries only
addressed_to_name     human entries only
text                   answer text for agents
status                complete | streaming | cancelled | failed
request_id            optional correlation with a turn
```

The fixed human identity is `participant_id = "human"` and display name
`"You"`. Notices display as `"System"` and errors as `"Error"`. Agent entries
carry the producing agent's stable ID and the display name used during that
turn.

`Transcript` supports terminal insertion and a single open streaming agent
entry:

- `add_entry`
- `begin_entry`
- `append_answer`
- `finish_entry`
- `discard_entry`
- `clear`
- `replace_entries`

Every mutation increments a revision. `clear()` and `replace_entries()` also
advance an in-memory history epoch so incremental renderers know to rebuild.
Entry IDs must be positive and strictly increasing, but need not be contiguous.

Snapshots are owning copies used by rendering and tests. `TranscriptReadView`
is the locked non-owning API used during backend preparation.

### Validation

`validate_transcript_entry()` enforces the semantic combinations:

- human and agent entries require participant IDs;
- human entries require a target ID and target display name;
- only human entries may carry targeting fields;
- human and notice entries must be complete;
- error entries must be failed;
- agent entries may be streaming, complete, or cancelled, but never failed;
- a complete agent entry must contain answer text;
- a cancelled agent entry must contain answer text.

`require_terminal_transcript_entry()` additionally rejects streaming state
before persistence. `require_storable_transcript_entry()` names the same
terminal-entry contract at the database boundary. The journal calls this guard
before binding any entry fields.

### Per-agent model context

The transcript is a human-visible record; model context is a projection.
`project_agent_context()` materializes that projection for the agent handling
the new prompt:

1. Emit that agent's effective system prompt, if non-empty. Its generated
   forum-context section names the current agent, lists the forum's other current
   personas, and defines the shared-history encoding.
2. Exclude the currently open streaming entry.
3. Exclude all notices and errors.
4. Exclude a human prompt whose request has a matching error entry.
5. Include only complete, non-empty agent responses; cancelled partial output
   remains visible but is not sent back to a model.
6. Emit the target agent's own completed responses as `assistant`.
7. Emit human prompts addressed to the target agent as ordinary `user`
   messages.
8. Group contiguous human prompts addressed elsewhere and other agents'
   completed responses into a separate `user` message headed
   `Shared chat history (JSONL):`.

Reasoning never enters `TranscriptEntry`, so projection can observe only agent
answer text.

Every line after the shared-history heading is an independently escaped JSON
object. A human entry carries `kind`, `speaker`, `addressed_to`, and `text`; an
agent entry carries `kind`, `speaker`, and `text`. JSON escaping prevents
newlines, quotes, or transcript-like text inside an entry from becoming false
speaker boundaries. The block is closed before the next prompt addressed to the
target agent, so quoted history and the question being answered never occupy
the same message.

This makes every agent see the same shared chat transcript from its own point of
view: its own prior answers are assistant messages, while exchanges involving
other agents are explicitly described as quoted input. The generated
forum-context section is present even in a single-agent forum because restored
history may still contain a persona that has since left the forum.

## Turn lifecycle

### Submission

For an idle controller, `SessionController::submit_prompt()` accepts prompt text
and an optional handle from a front end, resolves the target agent
through `ForumPersonas`, and rejects an empty prompt. It then starts the turn:

1. allocates a request ID and human entry ID;
2. commits the `started` turn and prompt entry in one SQLite transaction;
3. adds the prompt to the in-memory transcript;
4. reserves the response entry ID and creates `ActiveResponse`;
5. routes the request to the target backend through the shared request queue.

The database commit intentionally precedes the screen and execution request.
Once a started turn is durable, normal error paths drive it to a terminal
state. If the transcript rejects the prompt, or the registry refuses the
request, the controller fails that durable turn before reporting the problem.

`SessionController` is the only transcript writer and rejects new mutations
while a turn is active. `AgentRegistry` routes the request by its already
resolved target, so
the execution thread does not revalidate session-controller-owned request and
transcript invariants. It prepares an owning `RequestPayload` under
`TranscriptReadView`, releases the lock, and calls the synchronous backend.

### Streaming success

Each transport fragment becomes a typed `AgentDelta`. Reasoning appends to the
ephemeral buffer in `ActiveResponse` and never opens or modifies a transcript
entry. The first answer delta opens the reserved agent entry as `streaming`;
later answer deltas call `Transcript::append_answer()`.

`ActiveResponse` and `GenerationStatus` share one monotonic `ResponsePhase`:
`waiting`, `reasoning`, or `answering`. An answer advances the phase to
`answering`; late reasoning remains visible without moving it backward.

On `AgentCompleted`, the response controller requires the `answering` phase. It
constructs a fresh answer entry, commits that response and turn transition,
marks the live entry complete, clears `ActiveResponse` and its reasoning, and
requests a render.

For non-streaming HTTP, the backend uses the same event lifecycle: it publishes
one reasoning delta when selected and present, then one answer delta, followed
by `AgentCompleted`. A content-only response still produces only the answer
delta.

### Cancellation

`/stop`, Esc, or Ctrl-C while generating sets the registry's shared atomic
cancellation flag. The execution loop checks it before preparation, and
libcurl's progress callback observes it during transport and aborts
cooperatively.

If cancellation is set while an accepted request is waiting to prepare, the
execution thread publishes `AgentCancelled` without calling either backend
method. Once `perform()` begins, cancellation remains cooperative through the
same atomic flag.

On `AgentCancelled`:

- while waiting, the controller commits only the cancelled turn transition;
- after reasoning only, it commits the cancelled transition and clears the
  ephemeral reasoning without creating a response entry;
- after answer output, it stores a fresh answer-only cancelled response and
  finishes the live transcript entry as cancelled.

The prompt remains in model context, but cancelled agent output does not.

### Failure

Transport errors, protocol errors, backend execution exceptions, dispatch
failure, and a successful terminal event without answer content all become a
failed turn. In an explicitly configured reasoning format, a present non-null
reasoning field with the wrong JSON type is a protocol error even if the same
response also supplied valid answer text. This strict contract is intentional;
`auto` mode ignores malformed optional reasoning extensions.

The response controller commits a typed error entry, discards any open partial
agent entry, adds the error to the transcript, and clears the active response.
Context projection excludes both the error and the matching human prompt, so a
failed turn remains visible and durable without being replayed to a model.

Events with a request ID that does not match `ActiveResponse` are ignored. Request
IDs are therefore still required despite the single-active-turn policy:
asynchronous cancellation can leave late events in flight, and persistence
needs a stable turn correlation key.

## Commands and terminal behavior

When idle, the shared text grammar translates:

- `/clear`: advance the durable history epoch, empty visible history, and reset
  addressing labels based on the number of forum personas.
- `/agents`: show a transient status notice containing the forum personas and
  their runtime details; `*` marks the run-local default.
- `/info`: show a transient status notice containing the current transcript
  entry count followed by the same persona and runtime information.
- `/@Name`: change the run-local default agent.
- `/stop`: report that no generation is active.
- `/exit`: request session termination.

Command output is never added to the transcript or session database. Unknown
commands and commands with unexpected arguments likewise produce transient
status notices. While a turn is active, only bare `/stop` is accepted; other
submitted text remains in the editor and produces a “Generation in progress”
notice. This ordering intentionally preserves `/stop value` as a draft during
generation, while the same input when idle is rejected as an unexpected
argument and cleared.

`handle_text_input()` owns this textual dispatch policy. It combines
`parse_command()` and `parse_addressed_prompt()`, then calls `SessionController`
operations. `/exit` belongs entirely to the front end; it never reaches the
controller. The controller does not depend on terminal or text-grammar headers,
so a future HTTP front end can invoke the same operations directly. The shared
generation-in-progress notice is defined with `GenerationStatus` so the session
layer and front-end responses cannot drift.

`UserSession` is the UI state machine. It owns the `InputEditor`, applies
`SessionUpdate` values, and coalesces rendering behind `render_needed`.
`SessionView` is its test seam; `Tui` is the ncurses implementation.

`ConsoleSession` reuses the same grammar but has different arrival semantics:
complete lines received during generation are queued and dispatched one at a
time rather than refused. Bare `/stop` and `/exit` act while enqueueing; the
forms with arguments remain in FIFO order and are later rejected by
`handle_text_input()` under the shared command rules. Immediate `/exit` shuts
down the controller and cancels an active turn rather than draining its answer.
Piped stdin receives backpressure at the queue limit. EOF means “no more
submissions”; its libuv watcher is then stopped so a closed source cannot spin
the loop, while the active turn and queue continue to drain. SIGINT cancels an
active turn and exits while idle.

The input editor stores wide characters, supports cursor movement and editing,
and converts to UTF-8 on submission. A trailing backslash enters a visual
continuation line; visual newlines are removed from the submitted value.
Console `LineReader` applies the same continuation rule to byte input and
removes one trailing carriage return from each physical line, including the
final unterminated line, normalizing CRLF sources.

Key behavior:

- Page Up/Down scroll by half a viewport.
- Esc while idle clears the editor and status notice.
- Ctrl-C while idle exits.
- Esc or Ctrl-C while generating requests cancellation.
- Resize signals reconfigure curses and force rendering.
- Closed stdin ends the session.

`Tui` renders a transcript pad, reverse-video status line, and persistent input
pad. `GenerationStatus` includes the active agent's display name, shared
response phase, and ephemeral reasoning buffer. Status text is `generating`,
`reasoning`, or `responding`.
Human labels are `[You]` or `[You → Name]`; agent labels always include the
display name.
Addressed human labels are enabled whenever `ForumPersonas` contains multiple
personas, or when restored single-agent history contains another participant
or target. `/clear` forgets historical addressing evidence but keeps
addressing enabled for a currently multi-agent forum.

The console writes an append-only transcript log to stdout and notices to
stderr. While idle with interactive stdin, stderr also carries a bold
`@DefaultAgentName> ` prompt. `ConsoleSession` resolves the name from
`SessionController::default_agent_id()` each time it arms the prompt, so a
successful `/@Name` command changes the next marker as well as routing later
unaddressed submissions. No prompt is printed while a response is active.

`TranscriptEmitter` tracks entries by ID and streaming suffix length, and
advances its watermark only after stdout flushes successfully. A history clear
produces a marker rather than retracting bytes. If a failed turn discards a
partial answer from the stored transcript, already-written partial text remains
in the log and the error follows it. `SystemConsole` owns separate attributed
`ConsoleSurface` instances for transcript stdout and prompt stderr. Automatic
styling follows each stream's own TTY status; forced color mode applies to
both. `ConsoleSurface` neutralizes C0, DEL, and C1 terminal controls in model,
transcript, and prompt text. Its sanitizer carries a possible UTF-8 C1 lead
byte across non-empty writes; an empty write does not resolve that state. At
session end, `ConsolePort::finish_transcript()` emits an incomplete trailing
lead byte and performs a checked final stdout flush before the process chooses
its exit status. Destructors never emit transcript bytes.

Transcript entries use the compact `[Name] Answer` form. While a turn is
active and reasoning is present, the TUI combines the ephemeral reasoning
buffer with any open answer entry for presentation: agent label, bold/dim
`[Reasoning]` label, dim reasoning text, then normal answer text. When the turn
ends, a rebuild removes the reasoning block. Every complete-entry,
incremental-suffix, and input-pane path explicitly restores normal attributes.

`TranscriptRenderPlanner` compares snapshot revision, history epoch, width,
entry count, and the former last entry. It chooses no work, suffix append, or
full rebuild. Answer suffixes append incrementally. A change to
`GenerationStatus`, including streamed reasoning or a phase transition, forces
a rebuild of the presentation so ephemeral reasoning never becomes transcript
state. A recording `TranscriptSurface` test seam verifies the `[Reasoning]`
label, dim reasoning, normal answers, and attribute restoration.
`TranscriptViewport` follows output until the user scrolls and clamps its
position when content shrinks.

## Persistence

Each session is one self-contained
`forums/<forum>/sessions/<id>.sqlite3` database. Listing considers only regular
files with that extension.

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
| `session` | Singleton session `id`, `forum`, and display `label`. |
| `state` | Singleton `history_epoch`, `next_entry_id`, and `next_request_id`. |
| `turns` | `request_id`, originating epoch, and lifecycle state. |
| `entries` | Typed transcript fields, target attribution, text, and terminal status. |

The schema remains version 2 and contains no reasoning column. Reasoning exists
only in `ActiveResponse` and `GenerationStatus`, never in `TranscriptEntry`.
Completed responses and answer-bearing cancellations are persisted through
answer-only entries.

Turn states are `started`, `completed`, `cancelled`, and `failed`. A partial
unique index permits only one started turn in the entire session. Another
partial unique index permits only one human prompt per request. Full validation
also verifies that every turn has exactly one prompt in the same epoch.

Streaming status is never stored. A response row exists only after completion
with a non-empty answer or after cancellation with partial answer text.
Reasoning-only cancellation creates no transcript or database response entry.
A mixed cancelled response retains only its answer. Errors are terminal
`error` entries.

### Session listing and opening

`SessionCatalog::list()` examines regular `.sqlite3` files, opens them
read-only, and performs lightweight identity/metadata validation: application
ID, schema version, embedded session ID versus filename, and embedded forum
versus selected forum. Broken candidates remain visible with an error so one bad
file does not hide healthy sessions. Session-ID filename validation is part of
that per-candidate error boundary, so even a malformed `.sqlite3` filename
cannot abort the whole listing.

Transcript-sized validation is deferred until selection. `main()` calls
`Workspace::open_session()`, which calls `open_database_path()` for metadata
identity and immediately calls `load_session_state()`. Loading validates
durable state, the turn/prompt invariant, and each restored entry. Before
accepting writes,
`SessionJournal` validates database identity and structure; SQLite
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
back to the generated ID. `Workspace::create_session()` loads the forum's agent
definitions, creates the database through `SessionCatalog::create()`, and
constructs a controller over the fresh file. It returns that controller together
with the published ID as `CreatedSession`.

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
2. `load_session_state()` loads current-epoch terminal entries and finds
   started turns by joining them to their human prompts.
3. It reserves an `InterruptedTurn` error attributed to the prompt's target:
   “Response interrupted before completion”.
4. `SessionController::initialize()` installs the entries, adopts the durable
   ID counters, and persists each repair with `fail_turn` before any other
   journal mutation is accepted, adding each error to memory as it goes.

No streamed partial response is durable before its terminal transaction, so a
crash during streaming restores the prompt plus the interruption error.

### Persistence failure policy

Persistence errors are fatal for the current run. This is deliberate: after a
turn's terminal execution event has been consumed, continuing without its SQLite
transition would either diverge the visible transcript from durable state or
leave the controller active with no event left to complete it. The application
therefore does not convert database failures into ordinary transcript errors,
because writing such an error depends on the same unavailable journal.

Every journal mutation adds operation context before propagating the failure.
Turn-related messages identify the request and agent—for example, “Failed to
persist completion of request 7 for @Name”—while a failed `/clear` identifies
that operation. The chat loop preserves that original exception, destroys its
curses view, explicitly restores the terminal, stops the execution thread, and
then lets `main()` report the contextual failure.

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
prepare(request, locked transcript view) -> owning RequestPayload
perform(payload, delta callback, cancellation atomic) -> CompletionResult
```

This boundary lets tests inject backends while ensuring the transcript lock
cannot extend into transport work.

`CompletionClient` has two modes:

- `test`: return the submitted prompt as one delta without networking;
- `net`: use libcurl against an OpenAI-compatible API.

For each agent, configuration includes stable ID/name, host, port, mode, model,
streaming flag, sampling temperature, API key or environment-based key,
optional reasoning effort, reasoning format, and HTTP/HTTPS selection.
`api_key_env` overrides `api_key` and must resolve to a non-empty environment
value. `reasoning_effort` controls the requested generation policy;
`reasoning_format` independently describes the provider's response shape.

`reasoning_format` defaults to `auto` and accepts:

| Value | Response mapping |
| --- | --- |
| `auto` | Read non-empty string `reasoning_content`, otherwise `reasoning`; ignore malformed optional reasoning values. |
| `none` | Ignore structured reasoning fields. |
| `reasoning_content` | Read only that field and reject a present non-null non-string value. |
| `reasoning` | Read only that field and reject a present non-null non-string value. |

All modes continue to map ordinary `content` to answer output. The client does
not parse `<think>` or other tags embedded in `content`; tagged content is
therefore displayed, stored, and replayed as answer text.

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

Streaming mode parses SSE `data:` events from `choices[0].delta`. When one
event contains both selected reasoning and answer content, reasoning is emitted
first. In `auto`, `reasoning_content` takes precedence over `reasoning` per
event; later events may use a different supported field. Empty strings,
`null`, role-only deltas, and finish metadata do not emit output. Data after
`[DONE]` is ignored.

The streaming response context separately tracks recognized reasoning,
recognized answer output, total received bytes, `[DONE]`, and the first
protocol error. Successful completion requires both `[DONE]` and at least one
non-empty answer delta; reasoning alone is not a completed assistant response.
An early end distinguishes “output received before `[DONE]`” from “not valid
SSE”.

Non-streaming mode applies the same reasoning-format mapping to
`choices[0].message`, emits reasoning before `message.content`, and likewise
requires non-empty answer content.

Non-2xx bodies may provide the existing structured server-error message.
Successful-HTTP streaming protocol errors never echo body bytes; they include
only numeric status, a control-character-free content type capped at 128
bytes (or `unknown`), and the saturating total byte count. The client retains
at most 64 KiB of a streaming body so a response later identified as non-2xx
can still supply its error message. libcurl failures are transport errors.

## Shutdown and failure handling

After a normal loop exit, `run_user()` calls `UserSession::shutdown()`. On an
exceptional exit it first preserves the original exception and destroys the
`Tui`, then restores the terminal and calls `SessionController::shutdown()`
directly. A shutdown exception does not replace the operation failure that
caused the exceptional exit. `SessionController::shutdown()` is idempotent:

`ConsoleSession` likewise calls the controller shutdown path on every exit.
EOF is not itself an exit: queued prompts and an active turn drain first. An
idle interrupt, `/exit`, a closed event queue, or a fatal wait/write error
ends the loop immediately. Before shutdown, the console finalizes sanitizer
state; a failed final flush changes an otherwise successful result to exit code
1.

1. `AgentRegistry::stop()` sets the shared cancellation flag.
2. The registry closes its request queue, preserving any accepted work, and
   joins the execution thread. A queued request observes cancellation and
   publishes its terminal event before the thread exits.
3. After the execution thread stops, the registry closes the shared event
   queue, wakes the frontend, and becomes permanently stopped.
4. The controller drains remaining queued events so a final cancellation or
   completion receives its durable terminal transition.

Destructors call their shutdown paths defensively. `AgentRegistry` makes a
final join attempt if ordinary shutdown throws, preventing destruction of a
joinable `std::thread`.

Database writes remain on the main thread. A journal opens existing databases
read-write without `SQLITE_OPEN_CREATE`; only session creation may initialize a
database, so a mistaken path cannot silently become an empty session.

## Component map

| Component | Responsibility |
| --- | --- |
| `src/util/` | Shared text, path, environment, prompt templates, concurrent-queue, notifier, and input-wait utilities. |
| `src/transcript/` | Typed transcript, turn identifiers, and response-content values. |
| `src/agents/` | Agent definitions, runtime metadata, context projection, execution, provider communication, and the runtime event queue. |
| `src/session/` | Forum personas, chat coordination and in-flight turn state (`SessionController`), workspace and session operations, session catalogs, SQLite journaling, presentation-safe summaries, and generation status. |
| `src/ui/text/` | Shared slash-command and leading-mention parsing and dispatch. |
| `src/ui/render/` | Frontend-independent transcript labels and writing vocabulary. |
| `src/ui/tui/` | Ncurses selection, input, layout, redraw planning, and terminal session flow. |
| `src/ui/console/` | Console CLI, queued line input, signal handling, append-only emission, and sanitizing. |
| `src/apps/` | Executable composition roots. |
| `tests/` | Tests mirroring the source layout, plus integration and shared test support. |

All reusable non-curses sources are compiled into `cha_core`. Curses sources
are isolated in `cha_tui`; both application entry points remain outside those
libraries.

## Key invariants

1. `ForumPersonas` is non-empty, ordered, and fixed for one run.
2. Agent IDs and ASCII-folded display names are unique.
3. Runtime-information slot `i` and backend slot `i` describe the same agent.
4. Exactly one application-owned execution thread exists regardless of the
   number of forum personas.
5. Only the execution thread calls backend request methods after startup.
6. At most one turn is active across the session.
7. The request queue contains at most one work item, and at most one request
   is queued or executing.
8. Exactly one backend may perform at a time.
9. Registry control operations are externally serialized on the main thread.
10. At most one streaming transcript entry exists, and it is the last entry.
11. The main thread is the only transcript and database writer.
12. Backend preparation holds a read view; backend performance never does.
13. The transcript outlives the registry and its joined execution thread.
14. Registry routing sends a request to exactly one backend while the
    active-turn state prevents intervening transcript mutations.
15. The outstanding gate clears before a terminal event becomes observable.
16. Request IDs are positive, strictly increasing, and durable. Entry IDs are
    positive and strictly increasing within the live or restored transcript;
    gaps are valid, including the reserved response ID of a reasoning-only
    cancelled turn.
17. A durable started turn has exactly one human prompt in its originating
    epoch.
18. Streaming entries are never persisted.
19. Reasoning belongs only to `ActiveResponse`: it never enters the transcript,
    SQLite, or later model context and is cleared at the terminal event.
20. Complete responses require non-empty answer content; reasoning alone
    cannot complete a turn.
21. `ResponsePhase` is monotonic from `waiting` through optional `reasoning` to
    `answering`; late reasoning never moves it backward.
22. Completion, cancellation, and failure transition only the currently
    started turn.
23. Database state is committed before the corresponding in-memory terminal
    mutation.
24. Failed turns and cancelled output are excluded from model context according
    to their distinct rules.
25. Successful-HTTP streaming protocol errors never contain response-body
    bytes.
26. Every styled transcript and input-rendering path restores normal
    attributes.
27. Sessions bind to a forum, not to the forum's current personas.
28. The shared event queue closes only after the execution thread stops.
29. A journal mutation failure ends the current run; the application never
    continues with transcript state it could not persist.
30. Prompt-template includes resolve under the forum directory only: the include
    graph of a forum is closed under the directory that is shipped as a unit.
31. Forum validation loads the same static persona definitions as session
    startup but creates neither a session nor a completion provider.

## Build and testing

CMake builds:

- `cha_sqlite3`: pinned SQLite amalgamation;
- `cha_core`: static reusable core and console implementation, with no curses;
- `cha_tui`: static ncurses frontend when `CHA_BUILD_TUI=ON`;
- `cha`: full-screen application;
- `chacon`: line-oriented console application;
- `cha_tests`: discovered unit tests;
- `console_tests`: registered process tests for the console executable;
- `itest`: separately invoked integration executable.

The build requires threads; wide ncurses is required only when
`CHA_BUILD_TUI=ON`. It uses an installed libcurl when available, otherwise
fetches pinned curl 8.14.1. It also fetches libuv 1.52.1,
nlohmann/json 3.11.3, toml++ 3.4.0, SQLite 3.46.1, and GoogleTest 1.15.2 for
tests. The bundled curl uses Schannel on Windows, Secure Transport on macOS,
and OpenSSL elsewhere; Unix-like builds require OpenSSL so HTTPS is always
available. The TUI defaults on only for Linux;
macOS and Windows default to the console-only build. The POSIX socket and
process integration harnesses are omitted from Windows builds.

The `console` CMake preset explicitly sets `CHA_BUILD_TUI=OFF` and is the
portable console-only build and CI entry point:

```bash
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console
```

`make test` builds and runs the unit suite through CTest. Unit tests cover
configuration and prompt scopes, template expansion, identity,
forum-persona/mention routing, textual command parsing and dispatch, registry
execution behavior, transcript/context semantics, controller lifecycle
operations, workspace layout resolution, forum checking and session-summary
mapping, SQLite constraints and recovery, structured reasoning parsing and
safe diagnostics, concurrent queues and notification, input/UI state, and
styled incremental rendering.

`make itest` runs the separate integration binary from the checked-in
`workspace/`. Its tests cover live configured streaming, non-streaming, and
cancellation paths plus local mock-server multi-agent routing, per-agent
context, structured streaming and non-streaming reasoning, answer-only
persistence/context replay, and reopening after the personas in a forum change.

The principal test seams are:

- `CompletionBackend`, for registry/controller tests without a network;
- `SessionView`, for `UserSession` tests without curses;
- `TranscriptSurface`, for complete and incremental attribute-state recording
  without curses;
- `project_agent_context()`, whose materialized messages expose projected
  boundaries, roles, and content directly.

Persistence, transcript logic, parsing, rendering plans, and other local
components are concrete and tested directly.
