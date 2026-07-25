# Terminal interface

`interfaces/terminal/` implements the ncurses user interface. It owns terminal
lifecycle, startup selection, editable input, transcript presentation, event
polling, and the interactive session state machine.

## Structure

### Terminal lifecycle and startup

| Source | Responsibility |
| --- | --- |
| `terminal.*` | Own process-wide ncurses setup, mode changes, resize handling, and restoration. |
| `startup_selector.*` | Render room/session selectors and collect a new-session label using application-safe values. |

### Session input and control

| Source | Responsibility |
| --- | --- |
| `input_editor.*` | Maintain wide-character multiline input, cursor movement, editing, continuation lines, and UTF-8 submission. |
| `session_view.h` | Define typed input events and the `UserSession` rendering seam. |
| `user_session.*` | Apply terminal input and coordinator updates to one testable interactive session. |
| `user.*` | Poll stdin/agent readiness and run the top-level terminal event loop with orderly shutdown. |

### Rendering

| Source | Responsibility |
| --- | --- |
| `transcript_renderer.*` | Style entries, plan incremental updates, and maintain layout and scrolling. |
| `tui.*` | Implement `SessionView` with curses pads for the transcript, status line, and editor. |

## Runtime behavior

`StartupSelector` receives room names and `SessionSummary` values from the
application layer; it does not inspect workspace directories or repositories.

During a chat, `UserSession` translates typed terminal actions into editor,
scrolling, cancellation, submission, or shutdown behavior. Submitted text is
delegated to `interfaces/text/`. `UserEvents` lets the same main-thread loop
respond to keyboard input and agent notifications without polling either
source continuously.

The renderer reads `Conversation` snapshots and `GenerationStatus` directly.
Those values are already presentation-safe, so duplicating the full transcript
as terminal DTOs would add ceremony without strengthening the storage or agent
boundaries.

`SessionView` and `TranscriptSurface` isolate state-machine and rendering tests
from curses. Terminal resource ownership remains in `Terminal` and `Tui`.

## Dependencies

The terminal adapter may depend on:

- `application/` for workspace/session summaries, coordinator operations, and
  generation status;
- `conversation/` for transcript snapshots and entries;
- `interfaces/text/` for shared command and mention dispatch;
- wide ncurses and POSIX polling for the concrete terminal implementation.

It must not use storage repositories, workspace loaders, or agent backends
directly. Reusable chat policy belongs in `application/`; reusable textual
syntax belongs in `interfaces/text/`.
