# Utilities

`util/` holds small, broadly reusable helpers that belong to no single domain.
It is a leaf: nothing here may know about transcripts, agents, sessions, or
front ends. If a helper starts encoding policy from one of those areas, it
belongs in that directory instead.

## Contents

| Source | Responsibility |
| --- | --- |
| `text.*` | Byte-oriented whitespace tests, trimming, and ASCII case folding. |
| `path_name.*` | `require_path_component()` — a configured name must be one safe path component. |
| `environment.*` | `load_dotenv()` — optional `.env` loading that never overrides the real environment. |
| `concurrent_queue.h` | `ConcurrentQueue<T>` — a portable typed thread-safe queue. |
| `wake_notifier.h` | `WakeNotifier` — the event-loop wake interface used by producers. |
| `uv_event_loop.*` | `UvEventLoop` — the portable libuv loop and cross-thread wake adapter used by the terminal frontends. |

## Text helpers

These centralize the ASCII rules used by command parsing, agent-handle matching,
list-file reading, and environment loading. They are deliberately byte-oriented:
no locale, no Unicode normalization. Handle matching is case-insensitive only in
the ASCII range, which is exactly what the agent-name rules allow.

## Path safety

`require_path_component()` rejects empty names, absolute paths, anything with a
separator, and `.` / `..`. Every workspace-controlled name — a room from
`rooms.list`, a persona from `personas.list`, a session ID derived from a
filename — passes through it before it is joined onto a path. This is the single
chokepoint that keeps workspace files from addressing anything outside the
workspace.

## Dotenv loading

`load_dotenv()` reads `NAME=value` lines, ignoring blanks and `#` comments and
accepting simple quoting. A missing file is fine; a malformed entry or an
unreadable existing file is an error. Variables already present in the process
environment always win, so a shell export beats the file — which is what makes
`api_key_env` usable in both development and deployment.

## Queue and event-loop notification

Queue storage and event-loop notification are separate. `ConcurrentQueue<T>`
contains no descriptor or operating-system dependency. Producers that also need
to wake a frontend call its injected `WakeNotifier` after a successful push.

```mermaid
flowchart LR
    subgraph producer["Producer thread"]
        push["push value"]
    end
    q["ConcurrentQueue<br/>mutex + deque"]
    notifier["WakeNotifier"]
    async["UvEventLoop<br/>uv_async_t"]
    subgraph consumer["Consumer thread"]
        loop["run libuv loop<br/>with frontend handles"]
        observe["take notification"]
        take["drain with try_get"]
    end

    push --> q
    push --> notifier
    notifier --> async
    async -->|"callback"| loop
    loop --> observe
    observe --> take
    take --> q
```

Semantics worth knowing:

| Operation | Behavior |
| --- | --- |
| `push` | Enqueues a value. Returns `false` if the queue is closed. |
| `get` | Blocks until a value arrives. After close, drains what is queued and then returns `nullopt` forever. |
| `try_get` | Never blocks; distinguishes *empty* from *closed* so a caller can tell "nothing yet" from "no more". |
| `close` | Stops new writes and wakes blocked readers. Already queued values are still delivered. |

`UvEventLoop` uses libuv's coalescing, thread-safe `uv_async_send()`. The
frontend observes the async callback before draining the event queue. The
request queue needs no notifier because the agent thread blocks directly in
`get()`.

## Event-loop ownership

`UvEventLoop` owns only the loop and its async wake handle. Each frontend owns
the input and signal handles it adds to that loop. This keeps queue producers
independent of frontend policy and keeps native handle details out of the
session layer.

## Dependencies

- **Depends on:** the standard library, the process environment, and libuv.
- **Must not depend on:** `transcript/`, `agents/`, `session/`, or
  `ui/`.
- **Used by:** agent code, workspace and session code, the text grammar, and
  the composition root.

## Tests

`tests/util/unit_text.cpp`, `tests/util/unit_environment.cpp`,
`tests/util/unit_concurrent_queue.cpp`, and
`tests/util/unit_uv_event_loop.cpp`.
