# Console frontend

`ui/console/` implements the line-oriented `chacon` frontend. It depends on
`ChatApplication` for the mutable current session and persona, shared text
command grammar, and shared transcript rendering; it never discovers workspace
entities or opens catalog storage itself.

## Process and CLI contract

```text
chacon [--color=auto|always|never]
chacon --check
```

Interactive startup constructs `ChatApplication`, which opens `Guest` in
`Entrance / Welcome`. A TTY receives `Entrance / Welcome ready` on the notice
stream and the initial prompt names `Assistant`. Entity selection and listing
flags are intentionally unsupported; use `/iam`, `/open`, `/create`,
`/forums`, `/sessions`, and `/personas` in chat.

`--check` validates the application configuration and every workspace persona,
definition, forum, member override, and prompt file. It opens no session,
creates no Welcome storage, constructs no provider backend, resolves no
credential, and makes no network request. It prints `Workspace is valid.` and
returns 0 on success; validation failures return 1 and usage failures return 2.

## Event loop and switching

`SystemConsole` puts stdin, agent wakeups, and SIGINT on one libuv loop.
`ConsoleSession` queries `application.controller()` for every operation, so
signals, input, prompts, and notifications always address the current session.
It keeps FIFO and pipe backpressure for ordinary prompts. Bare `/stop` remains
immediate. `/help` is immediate during generation; the six navigation/list
commands are dispatched immediately and rejected while generation is active,
never deferred behind it.

Before an idle `/open` or `/create`, the old emitter writes and flushes its
final suffix. After a successful switch, the console writes the public notice
to stderr, replaces its emitter, emits restored target history exactly once,
and rearms the prompt with the new forum default agent. Application lists and
notices use stderr only and never enter a transcript or SQLite database.

## Append-only output

Transcript data goes to stdout; notices and the interactive bold
`@DefaultAgentName> ` prompt go to stderr. `TranscriptEmitter` retains only
watermarks for its current controller and commits them only after a successful
stdout flush. Restored history is emitted before waiting, completed entries are
written once, and streaming entries append only their new suffix. Because stdout
is append-only, `/clear` emits a marker rather than retracting prior bytes.

`ConsoleSurface` sanitizes untrusted transcript and notice text. ANSI styling
comes only from trusted attributes. On POSIX SIGPIPE is ignored, so closed
stdout becomes a normal write failure with exit code 1.
