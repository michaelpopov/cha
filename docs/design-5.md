# Design 5: Unify completion-batch ownership

Status: proposed C++ redesign.

This change replaces the two synchronized representations of an in-flight
completion batch with one move-only, RAII `CompletionBatch`. It preserves the
session actor, the fixed worker pool, concurrent multicast fan-out, ordered
foreground presentation, persistence behavior, and frontend protocol.

## Decision

`SessionController` will own one optional `CompletionBatch`. That object is the
single authority for:

- the selected `RunSpec` values and their order;
- the shared start gate;
- the corresponding backend executions and event queues;
- the foreground position;
- cancellation state;
- execution completion and synchronous cleanup.

Rename the backend-owning part of `AgentRegistry` to `CompletionExecutor`.
`CompletionExecutor` owns configured `CompletionBackend` objects and their
public runtime metadata and can stage a new batch into the session's worker
pool. It does not store a current batch or expose operations addressed to an
implicit current batch.

The resulting ownership is:

```text
SessionController
  |- transcript, journal, and visible ActiveResponse
  |- ThreadPool
  |- CompletionExecutor
  `- optional<CompletionBatch>
       |- ordered execution slots, each owning its CompletionInput
       |- foreground position
       |- shared start gate
       |- cancellation state
       `- completion/wait state
```

The batch's executions borrow the session-scoped executor's backends and the
notifier. `CompletionExecutor` borrows the worker pool while staging their
tasks. The controller's declaration order and explicit shutdown keep all three
dependencies alive until the batch has cancelled and waited for every
execution and the pool has joined its workers.

## Why change the current design

One logical batch is currently represented by two state machines.

`SessionController::ResponseBatch` owns:

- shared completion history;
- an ordered vector of `RunSpec` values;
- `foreground_index`;
- `abort_requested`;
- controller notice accumulation.

`AgentRegistry::BatchRecord` independently owns:

- the start gate;
- an ordered vector of `Execution` objects;
- each execution's cancellation flag and event queue;
- completion and wait state.

The controller first constructs `ResponseBatch::runs`, then copies those runs
into `CompletionInput` values passed to `AgentRegistry::stage_batch()`. Event
delivery later reconnects the two structures with:

```cpp
registry_.try_receive(batch_->foreground_index, event);
```

Correctness therefore depends on an implicit cross-class invariant:

> `ResponseBatch::runs[i]` and `AgentRegistry::BatchRecord::executions[i]`
> must describe the same request for their complete and identical lifetimes.

The current code maintains that invariant, but neither type can enforce it.
The controller advances one vector while the registry indexes the other, and
cancellation is recorded once in the controller and requested separately from
the registry. Cleanup similarly requires paired calls such as
`abandon_batch()` and `registry_.clear_batch()` on every normal and exceptional
path.

`AgentRegistry` also combines two different lifetimes:

- configured backends live for the whole `SessionController`;
- one response batch lives only for one submitted operation.

Giving those lifetimes separate concrete types makes the code easier to follow
and makes the run/execution correspondence structural rather than conventional.

## Scope and non-goals

This redesign does:

- establish one source of truth for batch runs, foreground selection,
  cancellation, and execution completion;
- make batch cleanup follow object lifetime;
- separate process/session-lived backend configuration from operation-lived
  execution state;
- remove the indexed controller-to-registry event-routing API;
- retain a narrow submission fault-injection seam for concurrency tests.

This redesign does not:

- replace the `LiveSession` owner thread or actor command queue;
- replace the fixed `ThreadPool` with `std::jthread`, coroutines, or a shared
  application pool;
- serialize multicast backend work;
- make background multicast output durable;
- change transcript, SQLite, `ControllerUpdate`, SSE, or HTTP behavior;
- move frontend snapshots or JSON into the agent layer;
- introduce an executor interface, batch interface, source hierarchy, or
  general task framework;
- make `CompletionBackend` instances generally safe for concurrent calls.

There remains at most one live batch per controller. That rule is enforced by
the controller's `optional<CompletionBatch>` and owner-thread command
serialization, not by a second current-batch record in the executor.

## Target types

### `CompletionExecutor`

`CompletionExecutor` is the long-lived backend supplier. Its responsibilities
are:

- construct or accept one `CompletionBackend` per forum character;
- validate backend identity and public runtime metadata;
- expose the immutable `AgentRuntimeInfo` vector;
- validate that the borrowed pool has one worker per backend;
- resolve each staged `CompletionInput` to its backend;
- create all execution slots and submit them behind one closed gate;
- provide the existing test-only hook immediately before pool submission.

A representative interface is:

```cpp
class CompletionExecutor final {
public:
    using BeforeSubmitHook = std::function<void(std::size_t)>;

    CompletionExecutor(
        std::vector<AgentDefinition>,
        WakeNotifier&,
        ThreadPool&);
    CompletionExecutor(
        std::vector<std::unique_ptr<CompletionBackend>>,
        WakeNotifier&,
        ThreadPool&,
        BeforeSubmitHook = {});

    const std::vector<AgentRuntimeInfo>& runtime_info() const noexcept;

    [[nodiscard]] CompletionBatch stage_batch(
        std::vector<CompletionInput> inputs);
};
```

The exact private organization may use a PIMPL if retaining the present header
is useful, but no polymorphic executor facade is needed. `stage_batch()` is an
owner-thread operation. It returns a fully staged, closed-gate batch by value
and does not retain a pointer, index, ID, or optional record for that batch.

### `CompletionBatch`

`CompletionBatch` is a move-only operation object. It owns an ordered vector of
execution slots. Each slot owns the one `CompletionInput` used by its backend,
including its `RunSpec`; there is no second vector of runs.

A representative interface is:

```cpp
class CompletionBatch final {
public:
    CompletionBatch(CompletionBatch&&) noexcept;
    CompletionBatch& operator=(CompletionBatch&&) = delete;
    ~CompletionBatch() noexcept;

    const RunSpec& foreground_run() const;
    std::size_t foreground_index() const noexcept;
    bool has_next_foreground() const noexcept;

    void open() noexcept;
    [[nodiscard]] ChannelReadStatus try_receive_foreground(AgentEvent& event);
    void advance_foreground();

    void cancel() noexcept;
    bool cancellation_requested() const noexcept;
    bool executions_finished() const noexcept;
    void wait_until_finished() noexcept;
};
```

This is an illustrative shape, not a requirement to expose every helper
publicly. The implementation should provide only what the controller and
focused tests need.

The important constraints are:

- `foreground_run()` and `try_receive_foreground()` select the same internal
  slot; no caller passes an index between objects.
- `advance_foreground()` advances that one selection. It is valid only after
  the current slot's terminal event has been delivered. The batch should track
  that condition and reject invalid advancement.
- `cancel()` is idempotent, marks the batch cancelled, sets every execution's
  cancellation flag, and cancels the unopened gate if it has not already
  opened.
- `wait_until_finished()` does not discard event queues. Shutdown can cancel,
  wait, and then drain the foreground terminal event before destroying the
  batch.
- destruction is `noexcept` and performs the safety fallback: cancel and wait
  for every submitted execution. Ordinary controller paths should still use
  explicit cancellation/wait operations where blocking matters.

Waiting here means waiting until executions can no longer access a backend. The
fixed `ThreadPool` continues to own and join its worker threads. Pool shutdown
is the stronger task-quiescence boundary and remains explicit in
`SessionController::shutdown()`.

## Execution slots and event correlation

The existing `Execution` implementation remains the right basic shape:

- it owns `CompletionInput`;
- it borrows exactly one backend and the notifier;
- it shares the batch's gate;
- it owns its cancellation flag and event queue;
- it publishes any number of deltas followed by exactly one terminal event;
- it catches worker exceptions and converts them to `AgentFailed`.

It moves under `CompletionBatch` as a private implementation detail. The batch
does not need a public `Execution` type.

Because the foreground `RunSpec` and foreground event queue are reached through
the same slot, it is impossible to ask one object for run `i` and another for
execution `i`. Request IDs remain on events as a defensive correlation check
for controller event application, but they are no longer the only protection
against positional mismatch.

## Batch lifecycle

### Failure-atomic staging

Staging retains the current strong guarantee:

1. Validate that inputs are non-empty, have history, name configured targets,
   and do not repeat a backend.
2. Construct the closed `StartGate` and every execution slot before submission.
3. Submit one task per execution to the fixed pool.
4. Keep all accepted tasks blocked on the gate.
5. Return the fully populated `CompletionBatch` only after every submission
   succeeds.

If validation, allocation, the test hook, or pool submission fails, cancel the
gate and wait for every already-accepted task. No backend may be called and no
task may remain live. The move constructor used for the return must be
`noexcept` so ownership cannot be lost after successful staging.

### Durable activation before execution

The controller builds `CompletionInput` values directly, stages the returned
batch, and stores it in `batch_`. It then reads
`batch_->foreground_run()`, persists that turn, installs the prompt and
`ActiveResponse`, and finally calls `batch_->open()`.

The gate is essential. A fast provider must not publish output before the
foreground request has durable and in-memory session state capable of receiving
it. If foreground activation throws, resetting the unopened batch cancels its
gate and waits without calling a backend.

### Parallel work, ordered presentation

Opening the shared gate allows all selected backends to run concurrently. Each
execution buffers events in its own queue. The controller consumes only
`try_receive_foreground()`.

After the foreground terminal event has been persisted and applied, the
controller either:

- advances the batch and activates `foreground_run()` for the next slot; or
- finishes the batch and destroys it after its executions are safe to release.

Later slots may have completed before they become foreground; their queued
events remain intact. This preserves full-width fan-out and ordered transcript
commits.

### Cancellation

`CompletionBatch::cancel()` becomes the single cancellation transition. The
controller no longer sets `ResponseBatch::abort_requested` and then separately
cancels a registry batch. Generation status reads
`batch_->cancellation_requested()` and `batch_->foreground_run()`.

Cancellation remains non-blocking for `/stop`. The event loop continues to
process the active foreground execution. Once no visible response remains and
all executions report finished, the controller may release the batch. Events
buffered for later multicast children are intentionally discarded on an abort,
matching current behavior.

### Normal completion and cleanup

On the final foreground terminal event, the controller completes persistence
and presentation, waits as necessary for execution safety, and resets the
optional batch. Destruction releases the gate, execution slots, and their event
queues together.

Controller-only notice accumulation may remain in `SessionController`. It is
presentation state, not execution state: it must not contain another run list,
foreground index, completion flag, or cancellation flag. A single helper must
clear those notice fields whenever `batch_` is released so notice state cannot
leak into a later operation.

### Shutdown

Shutdown preserves the current bounded order:

1. Mark the controller as shutting down.
2. If a batch exists, request batch cancellation.
3. Wait until all batch executions can no longer access their backends.
4. Drain and apply the active foreground terminal event while the batch queues,
   journal, and notifier remain alive.
5. Release the batch and active response.
6. Stop the `ThreadPool`, which closes admission, drains accepted tasks, and
   joins every worker.
7. Allow the executor and its backends to be destroyed only after the pool is
   quiescent.

The exception path must still stop the pool before rethrowing a persistence
failure. An execution can issue its final notifier wake immediately after a
waiter observes its finished flag, so neither the notifier nor executor may be
destroyed until pool shutdown has joined the task.

## Controller responsibilities that do not move

`SessionController` remains responsible for:

- assigning request and transcript entry IDs;
- capturing immutable completion history;
- resolving target characters and personas;
- starting, completing, cancelling, and failing durable turns;
- mutating the transcript;
- maintaining the visible `ActiveResponse` and its response phase;
- deciding whether a `ControllerUpdate` is an append, snapshot, or notice;
- aggregating notices across ordered multicast children;
- deciding when the next foreground run becomes visible.

`CompletionBatch` does not depend on `ControllerUpdate`, `Transcript`,
`SessionJournal`, web DTOs, JSON, SSE, or `LiveSession`.

## Ownership and threading invariants

The implementation must make these invariants explicit in comments and tests:

1. Only the session owner thread calls `CompletionExecutor::stage_batch()` or
   mutates `CompletionBatch` foreground state.
2. Worker threads touch only their own execution, the shared gate, their
   backend, their cancellation atomic, their event queue, and the notifier.
3. The controller owns at most one batch, so a backend is not entered by two
   batches from the same session.
4. All tasks are accepted before the gate can open.
5. A launched execution publishes exactly one terminal event, including
   cancellation before start and exceptions.
6. The batch outlives all backend access by its executions.
7. `CompletionExecutor`, `WakeNotifier`, and `ThreadPool` outlive the batch;
   the pool is joined before the executor's backends are destroyed.
8. A batch destructor is never invoked from one of that batch's pool workers;
   production destruction happens on the session owner thread.

## Error behavior

Preserve existing error categories and observable controller behavior:

- invalid programmer inputs throw `std::invalid_argument`;
- unavailable pool admission or submission fault injection throws
  `std::runtime_error` from staging;
- controller dispatch failure remains the non-consuming notice
  `Request could not be dispatched`;
- backend and worker exceptions remain terminal `AgentFailed` events;
- cancellation remains an `AgentCancelled` terminal event;
- activation and persistence failures cancel and wait for the batch before
  propagating.

Do not add batch IDs, retry queues, detached cleanup, or background reapers to
handle these cases.

## Test strategy

Split the existing `AgentRegistry` tests by responsibility:

- `CompletionExecutor` tests cover backend construction, metadata validation,
  pool-width validation, target resolution, input validation, and
  failure-atomic submission.
- `CompletionBatch` tests cover the closed gate, full-width fan-out,
  foreground routing, ordered advancement, event buffering, cancellation,
  exactly-one terminal delivery, explicit waiting, and destructor cleanup.

Controller tests remain the authority for transcript and persistence behavior.
They must continue covering:

- single-target and multicast generation;
- ordered prompt/answer commits despite out-of-order backend completion;
- cancellation before activation, during execution, and between foreground
  children;
- activation and persistence fault cleanup;
- starting a later batch after completion or cancellation;
- shutdown while a backend is active;
- shutdown when the final notifier wake is delayed;
- concurrent independent controllers.

The old test that the registry rejects a second current batch should disappear:
the executor no longer owns current-batch state. The equivalent production
rule is covered by controller busy behavior. The old out-of-range indexed
routing test should be replaced by tests showing that foreground run access and
foreground event delivery come from the same slot and that invalid advancement
is rejected.

## Result

After this redesign, there is one object to inspect when reasoning about an
in-flight operation. The controller asks that object for the foreground run,
receives that run's events from the same object, requests cancellation on that
object, and releases that object only after its work is safe.

The concurrency model is unchanged; the unnecessary cross-class state machine
is removed.
