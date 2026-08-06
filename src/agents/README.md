# Agent runtime

`agents/` owns everything about configured chat agents: loading a character,
projecting the transcript into model context, running completions as
registry-owned pool tasks, and speaking the provider's HTTP protocol. The ordered
characters in a forum and `@handle` resolution belong to `session/`.

It is the only layer that talks to a model server, and the only one that owns
completion pool tasks.

## Contents

| Source | Responsibility |
| --- | --- |
| `config.*` | `LoadedConfig` — typed character metadata, connection settings, and prompt variables from one TOML overlay, with field validation. |
| `persona.h` | `Persona` and the ordered `PersonaRoster` values passed down from workspace discovery. |
| `agent.*` | `AgentDefinition`, `CharacterInfo`, `AgentRuntimeInfo`, identity validation, definition loading with template expansion, the request and event protocol types, and `project_agent_context()`. |
| `json_serialization.h` | JSON dumping with consistent, context-specific invalid-UTF-8 errors. |
| `agent_registry.*` | Runtime metadata, gated pool executions, per-run event routing, cancellation, and batch cleanup. |
| `completion_backend.h` | The `CompletionBackend` seam and its prepared-request and result types. |
| `completion_client.*` | The HTTP backend: request bodies, SSE and non-streaming parsing, model discovery, and protocol diagnostics. |

## From character directory to running agent

```mermaid
flowchart LR
    subgraph disk["Workspace files"]
        app_cfg["app.toml [provider]"]
        definition_cfg["characters/X/character.toml"]
        definition_prompt["characters/X/CHARACTER.md"]
        base["forums/R/members/character_defaults.toml<br/>optional forum defaults + [prompt]"]
        member_cfg["forums/R/members/X/character.toml<br/>optional"]
        member_prompt["forums/R/members/X/CHARACTER.md<br/>optional replacement"]
        usr["forums/R/FORUM.md"]
        roster["personas/*/persona.toml + PERSONA.md<br/>verbatim"]
        shared["definition includes under characters/;<br/>member/forum includes under the forum"]
    end

    app_cfg --> load["load_config<br/>one parsed overlay"]
    definition_cfg --> load
    base --> load
    member_cfg --> load
    load --> conf["Config + TemplateScope"]
    conf --> expand["expand_template_file<br/>CHARACTER.md and FORUM.md"]
    definition_prompt --> expand
    member_prompt --> expand
    usr --> expand
    shared --> expand
    expand --> def["AgentDefinition<br/>config + effective system prompt"]
    conf --> def
    roster --> def
    def -->|"one per character"| client["CompletionClient"]
    client --> registry["AgentRegistry"]
    client -->|"info"| runtime["AgentRuntimeInfo"]
    runtime --> registry
    runtime -->|"identity only"| characters["session/ForumCharacters"]
```

The effective system prompt has four sections in this exact order: expanded
character `CHARACTER.md`, expanded forum `FORUM.md`, the static persona roster (each
`PERSONA.md` verbatim under its display-name heading), and generated forum context.
The roster is in lexicographic ID order and does not change for a live session.
It is model reference context only, not forum/session membership and not an
authorization list. Persona authorship is resolved independently at the
frontend/application input boundary.
Expansion is implemented in `util/text_template.*`; this layer supplies the
policy: a definition prompt is contained to workspace `characters/`, while a
member prompt and `FORUM.md` are contained to the forum directory. It also
supplies reserved `character.*` / `forum.*` names and the three-layer `[prompt]`
initial scope. An adjacent template `config.toml` overlays
that inherited scope for its template directory and descendants; reserved
loader values always win. The generated section names the current agent, lists
the other current characters, and defines how quoted shared history is encoded.
It is added even for a single-agent forum, because restored history can still
mention a character that has left. During session construction, loading happens
on the session's owner thread (the process main thread in `cha` and `chacon`);
a forum check loads synchronously on its calling thread. `session/` decides
*which* directories to load, `agents/` decides *how*.

Configuration is a key-wise overlay, not general inheritance. Built-in defaults
are applied first, then the application `[provider]`, the character definition, the optional forum
`members/character_defaults.toml`, and the optional per-member override. An omitted field inherits the
value below it. The character definition directory name provides the ID, and its
file must define `display_name`; it may also carry an optional one-line
`description`. Both forum-local layers must not define either field or
`tags`. `tags` are definition-only optional free-form strings: each is trimmed,
non-empty, free of controls and line breaks, and unique under ASCII case
folding while retaining authored casing. The removed `id` and `name` fields are
rejected. Parsing and validation errors identify the file that supplied the
invalid value.

Identity rules, enforced by `validate_character_id` and `validate_character_name`:

- an **ID** is ASCII letters, digits, underscores, and hyphens. It is stable and
  is what transcript entries record — never change it when renaming a character.
- a **name** is the visible `@handle`. It may not be empty, start or end with
  whitespace, start with `@` or `/`, or be a reserved participant name in any casing. Internal
  whitespace is allowed for multi-word handles.
- within one forum, IDs are unique and names are unique case-insensitively.

Workspace construction rejects persona/character ID collisions and
case-insensitive display-name collisions across all definitions. Tags organize
definitions only; they never imply membership in a forum.

`AgentRegistry` validates these rules when it accepts backend metadata.
`ForumCharacters` in `session/` separately owns the ordered identity-only view used
for lookup and handle resolution.

The generated context documents the shared-history JSONL encoding and the
`from <Name>:` convention. Context projection adds that prefix to ordinary
human `persona` messages, for both replayed entries and the live `RunSpec` prompt.
It never mutates stored text; shared-history JSONL retains its own `speaker`
field and unprefixed text.

## Execution: staged pool tasks and foreground routing

`AgentRegistry` exists so slow providers never block the UI. It owns one
backend per forum character and borrows the session's fixed-size `ThreadPool`.
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

- **One operation, several backends.** The controller still admits one persona
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
| `info()` | Any time | Character identity and public runtime details for the registry. |

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
    W -->|"human to this agent"| U["ordinary persona message"]
    W -->|"human to another agent"| J["shared-history JSON object"]
    W -->|"another agent"| J
    J --> B["contiguous JSON Lines block<br/>in a separate persona message"]
```

The generated system section explains that shared-history objects are quoted
statements whose first-person claims belong to their named speakers. JSON
escaping prevents embedded newlines, quotes, or label-like text from creating
false entry boundaries. A human prompt addressed to the requesting agent is
always emitted outside the preceding shared-history block. Plain single-agent
history retains its ordinary persona/assistant wire shape, and reasoning text is
never included.

The predicate is a conjunction, so the off-record rule needs no ordering against
the others. The span is passed in as one `OffrecordSpan` value taken from the
same owned `CompletionHistory` as the entries, and it is global: every character
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
  stays above; once the character and forum directories are known, this layer owns
  the loading.

## Tests

| Test | Covers |
| --- | --- |
| `tests/agents/unit_config_loader.cpp` | TOML fields, defaults, and rejection of malformed values. |
| `tests/agents/unit_agent_definition_loader.cpp` | Character and forum prompt expansion, composition, scopes, and load errors. |
| `tests/session/unit_forum_characters.cpp` | Forum-character validation and every handle-resolution branch. |
| `tests/agents/unit_agent_registry.cpp` | Pool-width validation, batch gating, terminal delivery, cancellation, indexed routing, and shutdown closure. |
| `tests/agents/unit_agent_context.cpp` | Projection rules, JSONL attribution, escaping, and message boundaries. |
| `tests/agents/unit_json_serialization.cpp` | Context-specific invalid-UTF-8 diagnostics for JSON serialization. |
| `tests/agents/unit_completion_client.cpp` | Request bodies, SSE and JSON parsing, reasoning formats, and the error taxonomy, driven by `tests/support/mock_http_server.h`. |
