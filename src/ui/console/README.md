# Console frontend

`ui/console/` implements the line-oriented `chacon` frontend. It reuses
`Workspace`, `SessionController`, the text command grammar, and the shared
transcript-writing vocabulary without depending on curses or `ui/tui/`.

## Process and CLI contract

`apps/console_main.cpp` parses:

```text
chacon --list-rooms
chacon --room ROOM --list-sessions
chacon --room ROOM [--session ID | --new LABEL] [--color=auto|always|never]
```

Room listings are one name per line. Session listings always contain three
tab-separated fields—ID, label, error—with no header, padding, or color.
Invalid sessions are included. Tabs and line breaks inside fields are replaced
with spaces, and terminal controls are sanitized.

Exit code 0 means normal EOF drain, `/exit`, idle Ctrl-C, or a completed
listing. Usage errors return 2. Workspace, persistence, transport setup, and
output failures return 1.

## Event loop and queue

`ConsoleSession` waits on stdin, the controller notification descriptor, and a
`signalfd` for SIGINT. Readiness flags are independent: in particular, a pipe
commonly reports input and hangup together, so input is drained before EOF is
recorded. EOF stops future reads but does not shut down the controller; the
active turn and the complete queued input are allowed to finish.

Submissions are single-flight FIFO. Piped input is bounded by a 64-line queue;
once full, stdin is left out of `poll()` until work drains, applying
backpressure in the pipe. Interactive stdin is never suppressed, so `/stop`,
`/exit`, and Ctrl-C remain reachable.

This intentionally differs from the TUI. `handle_text_input()` refuses ordinary
input submitted during generation, leaving the draft in its editor. The console
instead defers complete input lines and dispatches them one at a time after the
active turn. Bare `/stop` and `/exit` are recognized while enqueueing because
their meaning depends on immediacy; all other commands keep FIFO order.

## Input

`SystemConsole` makes stdin non-blocking and restores its original flags on
destruction. It reads bytes only after readiness and feeds them to
`LineReader`. Each newline completes a submission. A trailing backslash joins
the next physical line with no separator, matching `InputEditor`; a final
unterminated submission is flushed at EOF.

Ctrl-C is blocked before the agent thread starts and consumed through
`signalfd`. During generation it requests cancellation and keeps the process
alive. While idle it exits successfully. SIGPIPE is ignored so a closed stdout
is reported as a normal write failure rather than terminating the process by
signal.

## Append-only output

Transcript data goes to stdout; notices and the interactive prompt go to
stderr. `TranscriptEmitter` treats stdout as an append-only event log, not a
repaintable view. Restored history is emitted before the first wait, completed
entries appear once, and streamed entries append only their new suffix.
`/clear` emits a marker because previously delivered bytes cannot be removed.

Writing and committing the emitter watermark are separate operations:
`ConsoleSession` writes, flushes stdout, and only then commits. A failed flush
therefore never records undelivered output as delivered.

The stream records what the user was shown, which can differ from the final
stored transcript. If an agent emits partial answer text and later fails, the
controller discards that open entry and stores an error entry. The console
keeps the already-written partial text, closes it, and appends the error. This
is intentional append-only behavior, not a persistence leak.

`ConsoleSurface` sanitizes untrusted transcript bytes in both color modes:
newlines and tabs pass, carriage returns are dropped, C0 and DEL controls become
caret notation, and UTF-8 C1 controls are replaced. ANSI styling is generated
only by `attributes()`.

## Components

| Source | Responsibility |
| --- | --- |
| `console_port.h` | Test seam for waiting, input, signals, transcript output, notices, and flushing. |
| `line_reader.*` | Pure byte-to-submission parser. |
| `transcript_emitter.*` | Append-only entry-ID and streaming-suffix watermarks. |
| `console_writer.*` | Sanitizing stdout `TranscriptSurface`. |
| `console_session.*` | Queue, EOF, signal, event, emission, and shutdown state machine. |
| `console_startup.*` | CLI parsing, stable listings, and workspace session selection. |
| `system_console.*` | Real non-blocking descriptors and process streams. |

## Regression traps

| Trap | Consequence if missed |
| --- | --- |
| Shutting down at EOF | `printf 'hello\n' \| chacon` cancels its own prompt |
| Dispatching every queued line at once | Every prompt after the first becomes a busy notice |
| Reusing `TranscriptRenderPlanner` | No rebuild information, and streamed text lands after the separator |
| `signal()` plus an `EINTR` flag | Ctrl-C is silently lost when it arrives outside `poll()` |
| Writing transcript text unsanitized | Model output controls the user's terminal |
| Joining continued lines with a newline | `handle_text_input()` sees input the TUI can never produce |

Unit tests live in `tests/ui/console/`. The process tests in
`tests/integration/console_process_test.cpp` cover actual EOF, signals, closed
stdout, sanitization, queue order, and the absence of an ncurses dependency.
