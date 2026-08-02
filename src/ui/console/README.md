# Console frontend

`ui/console/` implements the line-oriented `chacon` frontend. It reuses
`Workspace`, `SessionController`, the text command grammar, and the shared
transcript-writing vocabulary without depending on curses or `ui/tui/`.

## Process and CLI contract

`apps/console_main.cpp` parses:

```text
chacon --list-forums
chacon --forum FORUM --list-sessions
chacon --forum FORUM --check
chacon --user USER --forum FORUM [--session ID | --new LABEL] [--color=auto|always|never]
```

`--check` fully loads the forum configuration and expanded prompts, validates
effective connection settings plus persona identity and uniqueness, then exits
without inspecting stored sessions, creating a session, resolving
`api_key_env`, initializing completion providers, discovering a model, or
making network requests. It validates prompt-template includes, not arbitrary
Markdown hyperlinks. A valid forum prints one plain summary line, such as
`Forum 'stoics' is valid (3 personas).`; validation failures use the normal
`Failed: ...` diagnostic and exit code 1.

Forum listings are one display name per line. Session listings always contain three
tab-separated fields—ID, label, error—with no header, padding, or color.
Invalid sessions are included. Tabs and line breaks inside fields are replaced
with spaces, and terminal controls are sanitized. Listing modes take precedence
over otherwise irrelevant session-selection flags. They do not accept `--user`;
chat startup requires it and resolves the ID from the workspace user roster
before opening a session.

Creating a session returns both its controller and its generated ID. With
interactive stdin, the ready banner prints that resolved ID before the first
prompt, including for `--new` and default session creation, so the session can
later be reopened with `--session`.

Exit code 0 means normal EOF drain, `/exit`, idle Ctrl-C, a completed listing,
or a successful forum check. Usage errors return 2. Workspace, forum
validation, persistence, transport setup, and output failures return 1.

## Event loop and queue

`SystemConsole` puts stdin, the agent wake handle, and a SIGINT watcher on one
libuv loop. It returns only semantic readiness flags to `ConsoleSession`.
EOF stops future reads but does not shut down the controller; the active turn
and the complete queued input are allowed to finish.

Submissions are single-flight FIFO. Piped input is bounded by a 64-line queue;
once full, libuv input reads are paused until work drains, applying
backpressure in the pipe. Interactive stdin is never suppressed, so `/stop`,
`/exit`, and Ctrl-C remain reachable.

This intentionally differs from the TUI. `handle_text_input()` refuses ordinary
input submitted during generation, leaving the draft in its editor. The console
instead defers complete input lines and dispatches them one at a time after the
active turn. Bare `/stop` and `/exit` are recognized while enqueueing because
their meaning depends on immediacy; all other commands keep FIFO order.
An immediate `/exit` during an active turn shuts the session down and therefore
cancels every live runner in that request's batch; it does not wait for provider
responses to finish.

## Input

`SystemConsole` uses a libuv TTY or pipe stream for interactive and piped
stdin, and asynchronous libuv filesystem reads for redirected regular files.
It feeds bytes to `LineReader`. Each newline completes a submission, and one
trailing carriage return is removed from every physical line so CRLF files and
pipes do not send `\r` to the model. A trailing backslash joins the next
physical line with no separator, matching `InputEditor`; a final unterminated
submission is flushed at EOF under the same carriage-return and continuation
rules.

Once stdin is exhausted its watcher remains stopped, preventing a closed input
source from spinning the loop. Ctrl-C is delivered by libuv. During generation
it requests cancellation of every live runner in the current batch and keeps
the process alive through cleanup; while idle it exits successfully. On POSIX,
SIGPIPE is ignored so a closed stdout is reported as a normal write failure
rather than terminating the process by signal.

## Append-only output

Transcript data goes to stdout; notices and the bold interactive prompt
(`@DefaultAgentName> `) go to stderr. The prompt follows `/@Name` default-agent
changes and is shown only while the controller is idle and able to accept
input. `--color=auto` decides transcript and prompt attributes independently
from the TTY status of stdout and stderr; `always` and `never` override both.
`TranscriptEmitter` treats stdout as an append-only event log, not a repaintable
view. It consumes a call-scoped borrowed transcript view and retains only an
entry index, open-entry ID, and text length between emissions. Restored history
is emitted before the first wait, completed entries appear once, and streamed
entries append only their new suffix. `/clear` emits a marker because previously
delivered bytes cannot be removed. The off-record
markers need no special handling for the same reason: they are ordinary notice
entries, so they are emitted once each, in the order the commands were given,
and nothing already written is ever retracted when the span changes.

On an interactive TTY, live human prompts are not rewritten as transcript
lines: the terminal already echoed the typed input, so a second `[You] ...`
line would only repeat it. Restored history still prints human turns so a
reopened session is readable. Piped stdin keeps echoing human prompts because
there is no interactive display of the input body.

Writing and committing the emitter watermark are separate operations:
`ConsoleSession` writes, flushes stdout, and only then commits. A failed flush
therefore never records undelivered output as delivered.

The stream records what the user was shown, which can differ from the final
stored transcript. If an agent emits partial answer text and later fails, the
controller discards that open entry and stores an error entry. The console
keeps the already-written partial text, closes it, and appends the error. This
is intentional append-only behavior, not a persistence leak.

`ConsoleSurface` sanitizes text written through both attributed console
surfaces in both color modes: newlines and tabs pass, carriage returns are
dropped, C0 and DEL controls become caret notation, and UTF-8 C1 controls are
replaced. ANSI styling is generated only by `attributes()`.
Console and redirected streams use UTF-8 bytes. On Windows, the attached
console is configured process-wide to interpret those bytes as UTF-8;
redirected streams remain byte-oriented. Automatic color then enables
virtual-terminal processing on each console output handle; if the host does
not support it, automatic color stays off.

Sanitizing is a property of the whole stream, not of one call. C1 is the only
rule spanning two bytes, and `U+009B` is an alternative CSI introducer, so
`ConsoleSurface` withholds a trailing lead byte and decides it against the next
non-empty write; empty writes leave the pending decision unchanged, so a
sequence cannot bypass sanitizing through a zero-length chunk. Whole strings —
startup listings, for instance — go through `sanitize_console_text()`, which is
the same rule with no carried state. At session end, `finish_transcript()`
emits any incomplete trailing lead byte before the final checked flush. A
failure changes an otherwise successful exit to status 1. Surface destruction
performs no output.

## Components

| Source | Responsibility |
| --- | --- |
| `console_port.h` | Test seam for waiting, input, signals, attributed transcript and prompt output, notices, ordinary flushing, and checked finalization. |
| `line_reader.*` | Pure byte-to-submission parser. |
| `transcript_emitter.*` | Append-only entry-index and streaming-suffix watermarks. |
| `console_writer.*` | Sanitizing attributed surfaces and the bold `@Name> ` prompt writer. |
| `console_session.*` | Queue, EOF, signal, event, emission, and shutdown state machine. |
| `console_startup.*` | CLI parsing, stable listings, forum checking, and workspace session selection. |
| `system_console.*` | Libuv input, signal, and wake handles plus stdout transcript and stderr prompt surfaces. |

## Regression traps

| Trap | Consequence if missed |
| --- | --- |
| Shutting down at EOF | `printf 'hello\n' \| chacon` cancels its own prompt |
| Dispatching every queued line at once | Every prompt after the first becomes a busy notice |
| Reusing `TranscriptRenderPlanner` | No rebuild information, and streamed text lands after the separator |
| An ad-hoc process signal flag | Ctrl-C delivery races with the blocking input wait |
| Writing transcript text unsanitized | Model output controls the user's terminal |
| Joining continued lines with a newline | `handle_text_input()` sees input the TUI can never produce |
| Leaving a closed stdin watcher active | The loop can spin at 100% CPU for the rest of the turn |
| Emitting a zero-length streamed suffix | Each spin writes two SGR resets; measured 26 MB of escapes for 48 bytes of transcript |
| Deciding the C1 rule one `write()` at a time | A `U+009B` split across two chunks reaches the terminal intact |
| Treating an empty write as a byte-stream boundary | A held C1 lead byte is released and the next chunk reconstructs the control |
| Emitting held sanitizer state from a destructor | The byte bypasses the checked flush and output failure exit status |

Unit tests live in `tests/ui/console/`. The process tests in
`tests/integration/console_process_test.cpp` cover forum checking without
sessions, credentials, providers, or network access, plus actual EOF, signals,
closed stdout, sanitization, queue order, and the absence of an ncurses
dependency.
The session and writer tests additionally cover default-agent prompt changes
and the prompt's bold/normal attribute boundary.
