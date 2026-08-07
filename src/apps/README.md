# Entry point

`apps/` contains the executable composition root. It owns process wiring and
top-level error handling, never reusable application policy.

## `web_main.cpp`

`chaweb` resolves its application directory, loads `app.toml` plus command-line
overrides, and loads provider and logging settings from the selected workspace.
It then assembles one immutable `WorkspaceModel`, one `SessionRepository`, a
`SessionRegistry` whose callback captures both and calls `open_session()`, the
static browser asset handler, route set, and HTTP listener.

Live `SessionController` instances are created only on registry owner threads.
The entry point bridges process signals into bounded shutdown: it stops HTTP
acceptance, wakes pending opens, asks every live owner to stop, and waits for one
configured grace period. If an owner remains stuck, the process logs its
identity and exits so operating-system lease cleanup can proceed.

Declaration order inside the inner scope is the shutdown order: the HTTP server
is destroyed first and releases its route captures, the registry then stops and
joins owners and releases its callback captures, the repository and model are
released next, and diagnostic logging is shut down only after the scope ends.

A composition root may depend on any concrete component it needs. HTTP/SSE
policy remains in `ui/web/`; nothing depends on `apps/`.
