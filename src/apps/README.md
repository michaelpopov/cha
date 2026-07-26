# Entry points

`apps/` holds composition roots: the files that assemble concrete components
into a runnable program, decide top-level object lifetime, and define
process-level error handling. They contain wiring and workflow — never reusable
policy.

Only files in this directory are excluded from the `cha_core` library, which is
what keeps every other layer testable and linkable without a `main()`.

## `tui_main.cpp`

The terminal application. It owns four things — the workspace, the terminal, the
selector, and the chosen controller — in that order, and hands the last one to
the chat loop.

```mermaid
flowchart TD
    a["main<br/>catch and report any exception"] --> b["load_dotenv"]
    b --> c["construct Workspace<br/>requires personas/ and forums/"]
    c --> d["construct Terminal<br/>process-wide curses"]
    d --> e["StartupSelector.select_forum"]
    e -->|"cancelled"| x["throw, exit 1"]
    e --> f["Workspace.sessions of forum"]
    f --> g["StartupSelector.select_session"]
    g -->|"cancelled"| x
    g -->|"row carries an error"| x
    g -->|"empty id, meaning New session"| h["prompt_session_name"]
    h --> i["Workspace.create_session"]
    g -->|"existing id"| j["Workspace.open_session"]
    i -->|"CreatedSession.controller"| k["SessionController"]
    j --> k
    k --> l["run_user with terminal and controller"]
    l --> m["return 0"]
```

Two properties matter more than the sequence:

- **Everything file-related goes through `Workspace`.** The entry point never
  constructs a `SessionCatalog`, never reads a persona directory, and never
  opens a database. That is what lets a second front end reuse the same startup
  without copying logic.
- **Failures are reported after the terminal is restored.** `Terminal`'s
  destructor leaves curses mode during unwinding, and `run_user()` restores it
  explicitly before rethrowing, so the message printed by `main()` reaches a
  normal screen.

Cancelling either selector is an error, not a silent exit: the process reports
why it stopped.

## `console_main.cpp`

The line-oriented application parses forum/session selection, constructs a
`SystemConsole` whose libuv loop handles SIGINT and stdin, opens the controller
through `Workspace`, and assembles `TranscriptEmitter` and `ConsoleSession`. It also decides
TTY-dependent behavior: the named `@DefaultAgentName> ` prompt and ready banner
appear only for interactive stdin, pipe input receives queue backpressure, and
automatic attributes are enabled independently for terminal stdout and stderr.
`Workspace::create_session()` returns the generated session ID with the
controller; the ready banner prints that resolved ID rather than a placeholder
for newly created sessions.

The libuv signal watcher is started before the controller creates its agent
thread. On POSIX, SIGPIPE is ignored so `ConsoleSession` can turn a closed
stdout into exit code 1 with an error on stderr.

Listings return before any session or console object is constructed and take
precedence over session-selection validation. Usage errors return 2, runtime
errors return 1, and orderly EOF, `/exit`, or idle Ctrl-C return 0. Before a
successful return, the console explicitly finalizes sanitizer state and checks
the last stdout flush; destruction does not perform hidden output.

## Dependencies

A composition root may depend on any concrete component it needs to assemble a
program. `tui_main.cpp` uses `session/`, `ui/tui/`, and `util/`.
`console_main.cpp` uses `session/`, `ui/console/`, `ui/render/`, and `util/`.

Nothing depends on `apps/`.

## Adding an entry point

A second executable — an HTTP server, a scripted client, a benchmark — gets its
own source file here and its own `add_executable` in `CMakeLists.txt`, linking
`cha_core`. Its protocol code belongs in a matching `ui/` directory, not
in this file; this file should stay short enough to read in one sitting.
