# Terminal application workflow

`application/` is the shared owner above one live `SessionController` and below
the terminal frontends. It constructs the built-in Guest, Assistant, Entrance,
and run-scoped Welcome values; owns the immutable effective persona catalog and
workspace inventory; resolves public names; and performs session-switch
transactions. It has no terminal output, frontend event-loop, or process-argument
dependency.

`ChatApplication` owns the selected persona and current `OpenedSession`. Its
operations implement `/iam`, `/open`, `/create`, `/forums`, `/sessions`, and
`/personas`; `ui/text/application_dispatcher.*` parses those commands and turns
their results into frontend presentation. `/help` is a built-in result so it
remains available while a response is generating. The other application
commands reject immediately during generation.

The application catalog uses public names only. Built-ins are omitted from
discovery lists by provenance but remain valid explicit targets. Welcome is an
ephemeral source separate from Entrance's persistent session source, which is
why it can be opened by name without appearing in an Entrance stored-session
list. Persistent Entrance sessions are application-owned storage, not
workspace-defined forum configuration.

The web frontend does not use this layer: it keeps its key-based routes and
workspace/session lifecycle. It shares only the workspace provider configuration
through the layers below this directory.

## Dependencies

`application/` may depend on `session/`, `agents/`, `transcript/`, and `util/`.
It must not depend on `ui/`, `apps/`, curses, console stream types, or HTTP
types. Frontends depend on it to obtain a replaceable current controller;
`SessionController` remains the owner of exactly one live chat and never
discovers or switches sessions.
