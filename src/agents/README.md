# Agent runtime

`agents/` owns everything about configured chat agents: loading a persona,
validating a room's roster, resolving `@handles`, projecting the transcript into
model context, running completions on a dedicated thread, and speaking the
provider's HTTP protocol.

It is the only layer that talks to a model server, and the only one that owns a
thread.

## Contents

| Source | Responsibility |
| --- | --- |
| `config.*` | `Config` — identity, connection, model, streaming, auth, and reasoning settings — plus TOML loading and field validation. |
| `agent.*` | `AgentDefinition` and `AgentInfo`, ID/name validation, the request and event protocol types, and `project_agent_context()`. |
| `agent_registry.*` | `AgentRoster` (validation, lookup, handle resolution) and `AgentRegistry` (execution thread, request routing, cancellation, channels). |
| `completion_backend.h` | The `CompletionBackend` seam and its prepared-request and result types. |
| `completion_client.*` | The HTTP backend: request bodies, SSE and non-streaming parsing, model discovery, and protocol diagnostics. |

## From persona directory to running agent

```mermaid
flowchart LR
    subgraph disk["Workspace files"]
        cfg["personas/X/config.toml"]
        sys["personas/X/SYSTEM.md"]
        usr["rooms/R/USER.md"]
    end

    cfg -->|"load_config"| conf["Config"]
    sys --> def["AgentDefinition<br/>config + effective system prompt"]
    usr --> def
    conf --> def
    def -->|"one per persona"| client["CompletionClient"]
    client -->|"info"| card["AgentInfo"]
    card --> roster["AgentRoster<br/>ordered, validated"]
    client --> registry["AgentRegistry"]
    roster --> registry
```

The effective system prompt is the persona's `SYSTEM.md` followed by the room's
`USER.md`, so the same persona behaves differently in different rooms. Loading
happens on the main thread during session construction: `session/` decides
*which* directories to load, `agents/` decides *how*.

Identity rules, enforced by `validate_agent_id` and `validate_agent_name`:

- an **ID** is ASCII letters, digits, underscores, and hyphens. It is stable and
  is what transcript entries record — never change it when renaming a persona.
- a **name** is the visible `@handle`. It may not be empty, contain whitespace,
  start with `@` or `/`, or be `User` in any casing.
- within a roster, IDs are unique and names are unique case-insensitively.

## Handle resolution

`AgentRoster::resolve_handle()` implements what a user may type after `@`:

```mermaid
flowchart TD
    start["handle text"] --> exact{"exact name,<br/>case-insensitive?"}
    exact -->|"yes"| resolved["resolved"]
    exact -->|"no"| trim["strip trailing punctuation"]
    trim --> exact2{"exact match now?"}
    exact2 -->|"yes"| resolved
    exact2 -->|"no"| prefix["collect case-folded<br/>prefix matches"]
    prefix --> count{"how many?"}
    count -->|"exactly one"| resolved
    count -->|"none"| unknown["unknown"]
    count -->|"several"| ambiguous["ambiguous, with candidates"]
```

The roster only reports the outcome. `SessionController` turns `unknown` and
`ambiguous` into the notices the user sees, because the wording is a session
concern, not a roster one.

## Execution: one thread, one request

`AgentRegistry` exists so a slow provider can never block the UI. It owns
one worker thread, one backend per roster entry, and two channels.

```mermaid
sequenceDiagram
    autonumber
    participant M as Main thread
    participant R as AgentRegistry
    participant Q as WorkItem channel
    participant W as Agent thread
    participant B as CompletionBackend
    participant E as AgentEvent channel

    M->>R: submit CompletionRequest
    R->>R: find backend by prompt.addressed_to
    R->>R: claim outstanding-request gate
    Note over R: refused if already claimed or stopped
    R->>R: clear cancellation flag
    R->>Q: push WorkItem
    W->>Q: blocking get
    W->>W: take Conversation read view
    alt cancelled before preparation
        W->>E: AgentCancelled
    else
        W->>B: prepare, then release the view
        W->>B: perform with delta sink and cancellation flag
        loop while output arrives
            B-->>W: CompletionDelta
            W->>E: AgentDelta with request id
        end
        B-->>W: CompletionResult
        W->>W: release the gate
        W->>E: AgentCompleted, AgentCancelled, or AgentFailed
    end
    M->>E: try_receive, woken by eventfd
```

Rules that fall out of this design:

- **Single flight.** The gate is process-wide, not per agent: a second submit is
  refused while any request is outstanding.
- **The gate opens before the terminal event.** By the time the main thread sees
  a terminal event, the next request can already be submitted.
- **Cancellation is cooperative.** `cancel()` only sets an atomic flag. It is
  checked before preparation and continuously by the transport, so a request
  that has not started yet is cancelled without ever reaching the provider.
- **Exceptions become events.** Anything thrown on the worker is converted to
  `AgentFailed`, so an accepted request always has an observable outcome.
- **Shutdown is ordered.** `stop()` sets cancellation, closes the request side,
  joins the worker, and only then closes the event side — so events already
  published stay readable, and a closed event channel during execution is a bug
  that throws rather than a silent loss.

## The backend seam

`CompletionBackend` is deliberately two-phase:

| Step | Runs | Purpose |
| --- | --- | --- |
| `prepare(request, view)` | Under the conversation lock | Read the transcript and build a `RequestPayload`. Must be fast. |
| `perform(payload, sink, cancellation)` | Without the lock | One synchronous completion, streaming fragments to the sink. |
| `info()` / `agent_id()` | Any time | Roster identity for the registry. |

Splitting them is what lets the main thread keep writing to the conversation
while a generation runs. Tests supply their own backend and never touch the
network; `tests/support/test_backends.h` has the helpers.

## HTTP transport

`CompletionClient` implements the seam for OpenAI-compatible servers.

```mermaid
flowchart TD
    prep["prepare"] --> proj["project_agent_context"]
    proj --> body["JSON body: model, stream, messages,<br/>optional temperature and reasoning_effort"]
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

`project_agent_context()` decides what one agent sees of a shared conversation.
It is pure, and it is tested directly.

```mermaid
flowchart TD
    E["transcript entries"] --> F{"projectable?"}
    F -->|"open streaming entry"| D["drop"]
    F -->|"notice or error"| D
    F -->|"human turn of a failed request"| D
    F -->|"agent entry not complete, or empty"| D
    F -->|"otherwise"| K["keep"]
    K --> W{"whose entry?"}
    W -->|"this agent"| A["assistant message"]
    W -->|"another agent"| U1["user message prefixed with that name"]
    W -->|"human"| U2["user message, attributed only<br/>when others are involved"]
    U1 --> C["coalesce adjacent user messages"]
    U2 --> C
```

Attribution prefixes appear only when the projected history actually involves
someone other than the requesting agent; a plain single-agent conversation is
sent unadorned. The agent's system prompt is always the first message, and
reasoning text is never included.

## Dependencies

- **Depends on:** `conversation/` for entries, read views, and IDs; `util/` for
  text helpers and `EventChannel`; libcurl and nlohmann/json in the HTTP client;
  toml++ in the config loader.
- **Must not depend on:** `session/` or `ui/`. Workspace discovery
  stays above; once the persona and room directories are known, this layer owns
  the loading.

## Tests

| Test | Covers |
| --- | --- |
| `tests/agents/unit_config_loader.cpp` | TOML fields, defaults, and rejection of malformed values. |
| `tests/agents/unit_agent_definition_loader.cpp` | Persona and room prompt composition, and load errors. |
| `tests/agents/unit_agent_roster.cpp` | Roster validation and every handle-resolution branch. |
| `tests/agents/unit_agent_registry.cpp` | Single-flight gating, event correlation, cancellation, shutdown ordering. |
| `tests/agents/unit_agent_context.cpp` | Projection rules, attribution, and coalescing. |
| `tests/agents/unit_completion_client.cpp` | Request bodies, SSE and JSON parsing, reasoning formats, and the error taxonomy, driven by `tests/support/mock_http_server.h`. |
