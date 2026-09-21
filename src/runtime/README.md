# Native protocol and live sessions

`runtime/` owns the native protocol values, session presentation, chat input
grammar, and the single-threaded runtime for live sessions. The bridge
exchanges owning request and result values with the application; it never
reads a controller or a borrowed controller view.

Workspace characters choose providers in workspace configuration. A session's
forum chooses its persona, so the submitter cannot select authorship. The raw
input path recognizes an optional leading character mention and `/mcast`, then
lets `SessionController` resolve recipients against the authoritative forum
configuration.

## Session runtime

There is one permanent `SessionRuntime` thread for all live sessions. It owns:

- one bounded command queue;
- one remembered-wake notifier shared by every provider request;
- the map from `FullSessionId` to live controller entries; and
- the selected session identity.

The runtime thread constructs, calls, shuts down, and destroys every
`SessionController`. Provider transports remain independent workers. Each
provider request keeps its own event queue and wakes the shared notifier; on
every turn the runtime drains a bounded event batch from every live controller.
Bounded command and event batches keep either source from starving the other.

`LiveSession` is the stable endpoint for one runtime instance. Controller state
behind that endpoint is private to the runtime thread; callers see only
immutable identity, an instance number, atomically published lifecycle state,
and a shared `SessionOutput`. A handle may outlive retirement, but the runtime
clears its controller before removal and a command includes the instance number,
so it cannot reach a later controller opened for the same identity.

The runtime loop performs these steps:

1. process a bounded batch of commands;
2. receive provider events from every live controller, including idle ones;
3. satisfy requested presentation snapshots and mirror durable changes;
4. retire ended entries and unselected entries whose generation is complete;
5. wait on the shared notifier unless a batch indicated more work.

Opening and journal work remain synchronous. A slow open or write can delay
other live sessions, but it cannot create concurrent controller access. This is
intentional until measurement demonstrates a need for more machinery.

## Selection and retirement

Selecting an already-live identity reuses its controller. A stored session is
opened before selection changes, so a failed or timed-out open preserves the
previous selection. Selection changes do not cancel generation: a deselected
generating controller remains in the map and continues receiving events until
its complete durable result has been processed. An unselected idle controller
is retired.

Before a selection is rejected for capacity, the runtime removes eligible idle
entries. An idle selected entry is replaceable after its candidate opens
successfully. If all capacity is genuinely occupied by generation, selection
fails promptly rather than waiting for progress that only the runtime thread
can make.

Normal retirement publishes a terminal `stopping` snapshot with reason
`retired`, closes the output producer, destroys the controller, and erases the
entry. Explicit close may cancel generation. Neither operation deletes the
stored conversation.

Shutdown reasons use this precedence:

```text
session_closed = retired < reloading < session_failed
    < session_deleted < server_stopping
```

The strongest reason observed before finalization is published. The final
snapshot is an owning value, and `SessionOutput::close()` preserves pending and
in-flight values so delivery can finish without retaining a controller or
waiting for renderer acknowledgement.

## Commands and output

Every command names a full session identity and a live instance. Commands and
replies own their arguments and results. An accepted ordinary mutation may
finish after its caller's deadline; an abandoned reply simply drops that
result. Selection has a commit boundary so a timeout before commit cannot
change selection.

`SessionOutput` is the thread-safe producer/consumer boundary. It keeps at most
one immutable in-flight payload and one replaceable pending payload, coalesces
compatible text appends, and falls back to a fresh snapshot when an append can
no longer be represented exactly. A background controller persists results
without building snapshots unless output is attached and requested.

Subscription completion carries the endpoint that was attached. The bridge
therefore does not synchronously look the session up from an inline runtime
completion callback. Runtime completions are handed to a separate bridge-ready
queue before bridge locks are taken, avoiding a cycle between bridge and
runtime waits. Stale acknowledgements and unsubscribe commands are checked
against subscription identity and output generation.

## Maintenance and shutdown

Deletion reserves one identity before releasing its live controller and keeps
that reservation until storage and media cleanup finish. Global vault
maintenance closes admission, publishes `reloading`, releases all controllers
and their journal connections, performs the storage operation, then reopens
admission with a new context epoch. Workspace invalidation uses the same
terminal path; presentation-only edits request a fresh snapshot without ending
the controller.

Application shutdown sets a stop flag that does not depend on ordinary queue
capacity, wakes the runtime, resolves queued work, publishes
`server_stopping`, and releases every controller. One absolute grace period
bounds the join. A runtime that misses that deadline is left to the existing
forced-exit path rather than being joined unconditionally by a destructor.

Controller failures are contained to their entry. Authoritative journal
failures end that session; Markdown mirror failures retain their best-effort
policy. Provider workers may finish transport cleanup after their controller
releases request handles, and late notifier wakes are harmless.

## Source map

| Files | Responsibility |
| --- | --- |
| `protocol.*` | Owning request, result, and presentation values exchanged with the bridge. |
| `request_parser.*` | Validate and parse incoming protocol requests. |
| `command_queue.*` | Bounded queue for runtime commands and their replies. |
| `runtime_settings.h` | Runtime capacity, timing, and batching settings. |
| `session_projection.*` | Build protocol snapshots from controller and workspace state. |
| `session_output.*` | Deliver and coalesce session output across threads. |
| `text_input.*` | Parse and dispatch messages, mentions, and `/mcast`. |
| `live_session.*` | Represent one stable live-session endpoint and its runtime-owned controller. |
| `live_session_manager.*` | Own the runtime thread, live-session registry, selection, retirement, and shutdown. |

This directory may depend on `characters/`, `chat/`, `session/`, `storage/`,
`util/`, and `workspace/`. Application and bridge code depend on it, so it
does not depend on `app/` or `bridge/`.

Focused tests live in `tests/runtime/`.
