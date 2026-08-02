# Agent runtime

`agents/` owns everything about configured chat agents: loading a persona,
projecting the transcript into model context, running completions as
registry-owned pool tasks, and speaking the provider's HTTP protocol. The ordered
personas in a forum and `@handle` resolution belong to `session/`.

It is the only layer that talks to a model server, and the only one that owns
completion pool tasks.

## Contents

| Source | Responsibility |
| --- | --- |
| `config.*` | `LoadedConfig` — typed connection settings plus prompt variables from one TOML overlay, with field validation. |
| `user.h` | `User` and the ordered `UserRoster` values passed down from workspace discovery. |
| `agent.*` | `AgentDefinition`, `PersonaInfo`, `AgentRuntimeInfo`, identity validation, definition loading with template expansion, the request and event protocol types, and `project_agent_context()`. |
| `json_serialization.h` | JSON dumping with consistent, context-specific invalid-UTF-8 errors. |
| `agent_registry.*` | Runtime metadata, gated pool executions, per-run event routing, cancellation, and batch cleanup. |
| `completion_backend.h` | The `CompletionBackend` seam and its prepared-request and result types. |
| `completion_client.*` | The HTTP backend: request bodies, SSE and non-streaming parsing, model discovery, and protocol diagnostics. |

## From persona directory to running agent

```mermaid
flowchart LR
    subgraph disk["Workspace files"]
        base["forums/R/personas/persona_defaults.toml<br/>optional forum defaults + [prompt]"]
        cfg["forums/R/personas/X/persona.toml"]
        sys["forums/R/personas/X/SYSTEM.md"]
        usr["forums/R/FORUM.md"]
        roster["users/*/user.toml + USER.md<br/>verbatim"]
        shared["shared prompt files under the forum"]
    end

    base --> load["load_config<br/>one parsed overlay"]
    cfg --> load
    load --> conf["Config + TemplateScope"]
    conf --> expand["expand_template_file<br/>SYSTEM.md and FORUM.md"]
    sys --> expand
    usr --> expand
    shared --> expand
    expand --> def["AgentDefinition<br/>config + effective system prompt"]
    conf --> def
    roster --> def
    def -->|"one per persona"| client["CompletionClient"]
    client --> registry["AgentRegistry"]
    client -->|"info"| runtime["AgentRuntimeInfo"]
    runtime --> registry
    runtime -->|"identity only"| personas["session/ForumPersonas"]
```

The effective system prompt has four sections in this exact order: expanded
persona `SYSTEM.md`, expanded forum `FORUM.md`, the static user roster (each
`USER.md` verbatim under its display-name heading), and generated forum context.
The roster is in lexicographic ID order and does not change for a live session.
Expansion is
implemented in `util/text_template.*`; this layer supplies the policy: forum
directory as containment root, reserved `persona.*` / `forum.*` names, and the
base-then-persona `[prompt]` initial scope. An adjacent template `config.toml` overlays
that inherited scope for its template directory and descendants; reserved
loader values always win. The generated section names the current agent, lists
the other current personas, and defines how quoted shared history is encoded.
It is added even for a single-agent forum, because restored history can still
mention a persona that has left. During session construction, loading happens
on the session's owner thread (the process main thread in `cha` and `chacon`);
a forum check loads synchronously on its calling thread. `session/` decides
*which* directories to load, `agents/` decides *how*.

Configuration is a one-level overlay, not general inheritance. Built-in
defaults are applied first, then the optional forum
`personas/persona_defaults.toml`, then the persona's own `persona.toml`. An omitted
field inherits the value below it. The persona directory name provides the ID,
and each persona file must define `display_name`; the defaults file must not. The
removed `id` and `name` fields are rejected. Parsing and validation errors
identify the file that supplied the invalid value.

Identity rules, enforced by `validate_persona_id` and `validate_persona_name`:

- an **ID** is ASCII letters, digits, underscores, and hyphens. It is stable and
  is what transcript entries record — never change it when renaming a persona.
- a **name** is the visible `@handle`. It may not be empty, start or end with
  whitespace, start with `@` or `/`, or be a reserved participant name in any casing. Internal
  whitespace is allowed for multi-word handles.
- within one forum, IDs are unique and names are unique case-insensitively.

When `load_agent_definitions()` combines a forum with its roster, it also
rejects user/persona ID collisions and case-insensitive display-name collisions.
This is a workspace configuration error reported as a plain `runtime_error`.

`AgentRegistry` validates these rules when it accepts backend metadata.
`ForumPersonas` in `session/` separately owns the ordered identity-only view used
for lookup and handle resolution.

The generated context documents the shared-history JSONL encoding and the
`from <Name>:` convention. Context projection adds that prefix to ordinary
human `user` messages, for both replayed entries and the live `RunSpec` prompt.
It never mutates stored text; shared-history JSONL retains its own `speaker`
field and unprefixed text.

## Execution: staged pool tasks and foreground routing

`AgentRegistry` exists so slow providers never block the UI. It owns one
backend per forum persona and borrows the session's fixed-size `ThreadPool`.
Every execution has its own cancellation flag and event queue. The queue buffers
deltas and reserves separate storage for one final event supplied when it closes.

```mermaid
sequenceDiagram
    autonumber
    participant M as Session owner thread
    participant R as AgentRegistry
    participant P as ThreadPool
    participant W as Pool workers
    participant B as Backends

    M->>R: stage_batch(CompletionInput[])
    R->>P: submit one gated task per child
    R-->>M: staged; positions match input order
    M->>R: open_gate
    par available workers
        P->>W: run child 0
        W->>B: prepare then perform
    and
        P->>W: run other children
        W->>B: prepare then perform
    end
    W-->>M: foreground channel through try_receive(index)
    Note over W: background channels remain buffered
    M->>R: clear_batch after the final terminal
```

Rules that fall out of this design:

- **One operation, several backends.** The controller still admits one user
  operation, while a multicast may target several distinct backends at once.
- **One explicit batch.** The registry stores at most one `BatchRecord`, not a
  collection keyed by batch ID. Its fixed vector of executions follows input
  order for the full batch lifetime.
- **Backend exclusivity.** One live batch and distinct validated targets ensure
  a backend is never called concurrently with itself.
- **Failure-atomic staging.** Input and task construction complete before
  submission. A partial submission cancels the unopened gate and waits only
  for the accepted tasks to finish.
- **One start decision.** Opening or cancelling the shared gate is idempotent;
  the first transition wins. Queued tasks observe an already-open or cancelled
  gate when they receive a worker; cancellation produces `AgentCancelled`
  without calling the backend.
- **Foreground-only consumption.** `try_receive()` exposes only the selected
  execution at the controller-selected index. Executions remain owned by the
  batch until whole-batch cleanup.
- **Guaranteed terminal delivery.** Allocating delta storage may fail and
  becomes an execution failure, but every launched execution—including one
  cancelled before `perform()`—publishes exactly one terminal event through
  the queue's allocation-free `close_with()` operation.
- **Cancellation is per execution.** It is checked before preparation and by
  the transport. `/stop` cancels every live execution and returns immediately.
- **Abort cleanup is event-loop driven.** `/stop` only cancels the executions
  and marks the batch aborting. Each execution wakes the main loop after
  becoming done; the controller clears the batch only after its foreground
  terminal was committed and all executions are done.
- **Background buffering is lossless and unbounded.** There is no artificial
  queue cap or silent dropping in this version. Memory use is proportional to
  the total delta data and queue overhead buffered by children that have not
  yet become foreground.
- **Background multicast work is intentionally volatile.** Only the selected
  foreground child has a durable turn. A process crash may lose queued output
  or a completed answer from a later child. This reviewed tradeoff preserves
  concurrent backends and ordered commits without adding batch persistence or
  result-spooling machinery; simplicity is preferred over crash durability for
  this in-flight work.
- **Exceptions become events.** Anything thrown on the worker is converted to
  `AgentFailed`, so an accepted request always has an observable outcome.
- **Shutdown is ordered.** The registry cancels and waits for its executions;
  the controller then stops and joins the pool before returning from shutdown.

## The backend seam

`CompletionBackend` is deliberately two-phase:

| Step | Runs | Purpose |
| --- | --- | --- |
| `prepare(input)` | Agent worker | Project owned `CompletionHistory`, append the run prompt once, and build a `RequestPayload`. Must be fast and local. |
| `perform(payload, sink, cancellation)` | Agent worker | One synchronous completion, streaming fragments to the sink. |
| `info()` | Any time | Persona identity and public runtime details for the registry. |

The controller captures immutable completion history before activating a turn,
so neither the registry nor a backend reads the live transcript. Splitting
preparation from performance keeps request construction separate from slow
provider I/O. Tests supply their own backend and never touch the network;
`tests/support/test_backends.h` has the helpers.

## HTTP transport

`CompletionClient` implements the seam for OpenAI-compatible servers.

```mermaid
flowchart TD
    prep["prepare"] --> proj["project_agent_context"]
    proj --> body["JSON body: model, stream, messages,<br/>temperature and optional reasoning_effort"]
    body --> post["POST to /v1/chat/completions"]
    post --> mode{"streaming?"}
    mode -->|"yes"| sse["parse text/event-stream data lines"]
    mode -->|"no"| json["parse choices 0 message"]
    sse --> frag["CompletionDelta:<br/>reasoning or answer"]
    json --> frag
    sse --> done{"DONE marker seen?"}
    done -->|"no"| perr["protocol_error"]
    done -->|"yes"| ans{"any answer text?"}
    json --> ans
    ans -->|"no"| perr
    ans -->|"yes"| ok["completed"]
```

Details worth knowing before changing this file:

- **Model discovery.** When `model` is unset, the constructor GETs `/v1/models`
  and takes `data[0].id`. That request has a 10-second timeout; the chat request
  deliberately has none, because generations are long. Both use a 10-second
  connect timeout.
- **Cancellation** is wired through libcurl's progress callback, so an in-flight
  transfer aborts promptly and is reported as `cancelled` rather than an error.
- **Reasoning formats.** `auto` accepts `reasoning_content` or `reasoning`
  (preferring the former), `none` disables extraction, and the two named formats
  select exactly one field. Reasoning inside ordinary `content`, such as
  `<think>` tags, is *not* parsed — it has no structured boundary, so it is
  treated as answer text.
- **Outcome taxonomy.** `completed`, `cancelled`, `protocol_error` (bad HTTP
  status, malformed JSON or SSE, missing terminator, no answer content), and
  `transport_error` (libcurl failure). Only the error outcomes carry a message,
  and streaming protocol errors report sanitized status, content type, and byte
  counts — never model output.
- **Test mode.** `mode = "test"` skips HTTP entirely: `prepare` returns the
  prompt text and `perform` echoes it back as a single answer delta. This is
  what makes the checked-in workspace runnable without a server.

## Context projection

`project_agent_context()` decides what one agent sees of a shared chat transcript.
It is pure, and it is tested directly.

```mermaid
flowchart TD
    E["transcript entries"] --> F{"projectable?"}
    F -->|"open streaming entry"| D["drop"]
    F -->|"inside the off-record span"| D
    F -->|"notice or error"| D
    F -->|"human turn of a failed request"| D
    F -->|"agent entry not complete, or empty"| D
    F -->|"otherwise"| K["keep"]
    K --> W{"whose entry?"}
    W -->|"this agent"| A["assistant message"]
    W -->|"human to this agent"| U["ordinary user message"]
    W -->|"human to another agent"| J["shared-history JSON object"]
    W -->|"another agent"| J
    J --> B["contiguous JSON Lines block<br/>in a separate user message"]
```

The generated system section explains that shared-history objects are quoted
statements whose first-person claims belong to their named speakers. JSON
escaping prevents embedded newlines, quotes, or label-like text from creating
false entry boundaries. A human prompt addressed to the requesting agent is
always emitted outside the preceding shared-history block. Plain single-agent
history retains its ordinary user/assistant wire shape, and reasoning text is
never included.

The predicate is a conjunction, so the off-record rule needs no ordering against
the others. The span is passed in as one `OffrecordSpan` value taken from the
same owned `CompletionHistory` as the entries, and it is global: every persona
in the forum sees the same exclusion, so the shared history they quote stays
consistent between them. Excluded turns are spliced out silently — no
placeholder marks the gap, since a note saying material was withheld is itself
the influence the span exists to remove. Because the bounds only ever land on
turn boundaries the span holds whole turns, so a splice can merge the runs on
either side of it into one shared-history block but can never separate a prompt
from its answer.

## Dependencies

- **Depends on:** `transcript/` for entries, completion histories, and IDs; `util/` for
  text helpers, `ConcurrentQueue`, and `WakeNotifier`; nlohmann/json for shared-history and HTTP
  JSON; libcurl in the HTTP client; toml++ in the config loader.
- **Must not depend on:** `session/` or `ui/`. Workspace discovery
  stays above; once the persona and forum directories are known, this layer owns
  the loading.

## Tests

| Test | Covers |
| --- | --- |
| `tests/agents/unit_config_loader.cpp` | TOML fields, defaults, and rejection of malformed values. |
| `tests/agents/unit_agent_definition_loader.cpp` | Persona and forum prompt expansion, composition, scopes, and load errors. |
| `tests/session/unit_forum_personas.cpp` | Forum-persona validation and every handle-resolution branch. |
| `tests/agents/unit_agent_registry.cpp` | Pool-width validation, batch gating, terminal delivery, cancellation, indexed routing, and shutdown closure. |
| `tests/agents/unit_agent_context.cpp` | Projection rules, JSONL attribution, escaping, and message boundaries. |
| `tests/agents/unit_json_serialization.cpp` | Context-specific invalid-UTF-8 diagnostics for JSON serialization. |
| `tests/agents/unit_completion_client.cpp` | Request bodies, SSE and JSON parsing, reasoning formats, and the error taxonomy, driven by `tests/support/mock_http_server.h`. |
