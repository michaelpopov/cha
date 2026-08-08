# Plan 5: Implement unified completion-batch ownership

Status: implementation plan for [Design 5](design-5.md).

This plan replaces `SessionController::ResponseBatch` and
`AgentRegistry::BatchRecord` with one move-only `CompletionBatch`. It also
narrows and renames `AgentRegistry` to `CompletionExecutor`, which owns
configured backends but no active operation.

The phases are ordered to preserve the existing concurrency behavior at every
checkpoint. Do not combine this work with changes to the session actor,
`ThreadPool`, persistence schema, controller update protocol, or web runtime.

## Implementation rules

- Preserve one owner thread and one fixed worker pool per live session.
- Preserve one pool worker per configured backend and full-width multicast
  fan-out.
- Preserve the closed-gate staging transaction: all tasks are accepted before
  any backend can run.
- Preserve exactly one terminal event per accepted execution.
- Preserve ordered foreground presentation and lossless buffering for later
  multicast children.
- Preserve non-blocking `/stop`, event-loop-driven abort cleanup, and bounded
  synchronous shutdown.
- Preserve the controller's HTTP-independent `ControllerUpdate` behavior and
  every observable transcript, notice, and persistence result.
- Do not add virtual executor or batch interfaces, a batch registry, batch IDs,
  a task-source hierarchy, detached cleanup, or a general scheduler.
- Do not make `CompletionBatch` depend on session persistence or web types.
- Do not leave a compatibility `AgentRegistry` facade in the final tree.
- Keep each phase buildable and run the relevant tests at each checkpoint.

## Final production files

Add these files to `cha_core`:

| File | Purpose |
| --- | --- |
| `src/agents/completion_batch.h/.cpp` | Own one batch's runs, gate, executions, foreground position, cancellation, queues, and wait state. |
| `src/agents/completion_executor.h/.cpp` | Own configured backends and runtime metadata and stage new batches into the borrowed pool. |

Remove these files after all callers migrate:

| File | Reason |
| --- | --- |
| `src/agents/agent_registry.h/.cpp` | Its backend ownership moves to `CompletionExecutor`; its live batch state moves to `CompletionBatch`. |

Replace `tests/agents/unit_agent_registry.cpp` with focused executor and batch
tests. If the shared test setup would become larger than the tests themselves,
one `unit_completion_batch.cpp` may cover both concrete types; do not introduce
a public test-support abstraction solely to force a file split.

## Phase 0: Establish and record the baseline

1. Configure and build the current tree:

   ```sh
   cmake --preset ninja
   cmake --build --preset ninja
   ```

2. Run the core tests that directly exercise the affected behavior:

   ```sh
   build/ninja/cha_tests \
     --gtest_filter='AgentRegistry.*:SessionController.*:ConcurrentControllers.*'
   ```

3. Run the complete C++ suite:

   ```sh
   ctest --test-dir build/ninja --output-on-failure
   ```

4. Record any pre-existing failures. Do not change unrelated code to make this
   redesign appear green.
5. Use these searches as the initial migration inventory:

   ```sh
   rg -n 'AgentRegistry|ResponseBatch|stage_batch|open_gate|try_receive\(|cancel_batch|executions_finished|clear_batch' src tests CMakeLists.txt
   ```

6. Read the existing tests around partial submission, cancellation, multicast
   ordering, activation faults, persistence faults, and delayed final wakes
   before moving code. Those tests define the safety contract.

## Phase 1: Lock down the behavior being moved

Before changing ownership, fill only genuine gaps in the existing test suite.
Prefer strengthening an existing test over adding a near-duplicate.

1. Verify failure-atomic staging explicitly proves all three facts after a
   submission hook throws:

   - no backend was prepared or performed;
   - every accepted gated task finished;
   - a later batch can be staged and run.

2. Verify multicast completion out of order still commits prompts and answers
   in selected foreground order.
3. Verify `/stop` cancels every started backend but does not synchronously wait
   in the command handler.
4. Verify cancellation while the gate is still closed never calls a backend.
5. Verify shutdown can wait for a worker whose final notifier wake occurs after
   its execution-finished flag becomes observable.
6. Verify an activation or persistence exception leaves the controller able to
   shut down without a live worker accessing destroyed state.

Do not assert private type layouts, mutex counts, or exact scheduling. The tests
must describe behavior that the target design retains.

### Phase 1 checkpoint

Build and run the focused and full suites. There should be no production
behavior change.

## Phase 2: Introduce `CompletionBatch`

Add `src/agents/completion_batch.h/.cpp` and move the operation-lived pieces of
`AgentRegistry::Impl` into it.

### 1. Define a narrow move-only public type

Declare `CompletionBatch` as final, non-copyable, nothrow move-constructible,
and not move-assignable unless a clear need appears. `std::optional` can use
`reset()` and `emplace()` without move assignment.

Expose only the operations needed by the controller:

- `foreground_run()`;
- an owner-thread foreground position accessor only if required by the
  existing activation fault-injection hook;
- `has_next_foreground()`;
- `open()`;
- `try_receive_foreground()`;
- `advance_foreground()`;
- `cancel()` and `cancellation_requested()`;
- `executions_finished()` and `wait_until_finished()`.

Use a PIMPL if it keeps synchronization primitives and `Execution` out of the
header. Do not introduce a virtual interface.

### 2. Move the execution machinery without changing it semantically

Move these private concepts from `agent_registry.cpp`:

- `GateState` and `StartGate`;
- `Execution`;
- the ordered collection currently represented by `BatchRecord`.

Each execution must continue to own its `CompletionInput`. Make
`foreground_run()` read the `RunSpec` from the same execution slot that
`try_receive_foreground()` reads. Do not add a separate batch-owned
`vector<RunSpec>`.

Retain:

- one atomic cancellation flag per execution;
- one event queue per execution;
- preallocated fallback failure state;
- allocation-free terminal queue closure;
- exception-to-`AgentFailed` conversion;
- a finished flag and condition variable that prove backend access has ended;
- notifier wakes after delta publication, terminal publication, and execution
  completion.

### 3. Put foreground transition rules in the batch

Store `foreground_index` only in `CompletionBatch`.

Track whether the current foreground terminal event has been delivered.
`try_receive_foreground()` marks that state when it returns
`AgentCompleted`, `AgentCancelled`, or `AgentFailed`. Reject
`advance_foreground()` if:

- there is no current slot;
- the current terminal has not been delivered; or
- there is no next slot.

Advancement clears the terminal-delivered marker for the new slot. This keeps
the ordering rule inside the one object that owns both runs and queues.

### 4. Implement cancellation and waiting

`cancel()` must be idempotent and owner-thread safe:

1. record cancellation on the batch;
2. set every execution's cancellation atomic;
3. cancel the shared gate.

The existing gate transition remains first-decision-wins: cancelling a closed
gate prevents backend calls, while cancelling an already opened gate relies on
the execution cancellation atomics.

`executions_finished()` checks every execution without consuming events.
`wait_until_finished()` waits for every execution and leaves all event queues
alive and drainable.

The `noexcept` destructor calls `cancel()` and `wait_until_finished()` before
releasing execution ownership. Document that production destruction occurs on
the session owner thread, never on a pool worker. Keep explicit waits in
shutdown and other paths where the blocking point should remain visible.

### Phase 2 checkpoint

At this checkpoint the new type may still be constructed by code in the old
translation unit while production uses the old registry surface. Build and run
the focused tests. Do not duplicate the execution algorithm in two permanent
implementations; the code must already have one `Execution` implementation.

## Phase 3: Replace `AgentRegistry` with `CompletionExecutor`

Add `src/agents/completion_executor.h/.cpp`. Move only session-lived backend
state and batch construction into it.

### 1. Move backend construction and validation

Move these responsibilities unchanged:

- construction of `CompletionClient` backends from `AgentDefinition`;
- validation of non-empty, non-null backends;
- character ID and name validation;
- duplicate ID and folded-name rejection;
- construction and exposure of `AgentRuntimeInfo`;
- exact worker-pool-width validation.

Retain the current error wording unless a renamed type appears in the text. Do
not leak provider configuration through `runtime_info()`.

### 2. Make staging return ownership

Implement:

```cpp
CompletionBatch CompletionExecutor::stage_batch(
    std::vector<CompletionInput> inputs);
```

The method must:

1. reject an empty batch, missing history, unknown target, or duplicate target;
2. resolve all targets before submitting any task;
3. construct the batch's closed gate and every execution before submission;
4. submit one task per execution;
5. return the move-only batch only after all submissions succeed.

On partial submission failure:

1. cancel the unopened gate;
2. wait for only the executions whose tasks were accepted;
3. release the unsubmitted slots;
4. rethrow the original exception.

Preserve the existing `BeforeSubmitHook` on `CompletionExecutor` as a test-only
constructor argument. Do not put submission-failure behavior behind a virtual
pool interface.

### 3. Remove current-batch state from the executor

`CompletionExecutor` must not contain:

- `optional<BatchRecord>`;
- a live batch ID or run index;
- `open_gate()`;
- indexed `try_receive()`;
- `cancel_batch()`;
- `executions_finished()` for an implicit batch;
- `clear_batch()`;
- registry `StopState` used to coordinate an implicit batch.

It may borrow the pool and notifier for staging, but the returned batch owns
the execution objects that use them.

### 4. Split and update agent-runtime tests

Move the construction-oriented tests from
`tests/agents/unit_agent_registry.cpp` to
`tests/agents/unit_completion_executor.cpp`:

- wrong pool width;
- empty/null backends;
- invalid and duplicate metadata;
- backend startup failure attribution;
- missing history, unknown targets, and duplicate targets;
- stopped-pool rejection;
- partial-submission rollback.

Move operation-oriented tests to
`tests/agents/unit_completion_batch.cpp`:

- a closed gate does not start backends;
- cancellation before opening skips preparation and performance;
- opening starts every selected backend at full pool width;
- foreground run and event delivery refer to the same slot;
- later slots buffer events until foreground advancement;
- advancement requires a consumed terminal event;
- backend inputs retain shared captured history;
- exceptions become `AgentFailed`;
- every execution delivers exactly one terminal event;
- cancellation reaches every opened execution;
- explicit waiting leaves terminal events drainable;
- destruction cancels and waits;
- a completed or cancelled batch permits a later batch through the same
  executor.

Delete the test that `AgentRegistry` rejects a second batch while one is live.
That is no longer an executor responsibility. Do not replace it with an
executor mutex or active flag.

Delete the indexed out-of-range routing test. Replace it with the foreground
transition tests above.

### Phase 3 checkpoint

Update `CMakeLists.txt` to compile the new production and test files. Build and
run all `CompletionExecutor.*`, `CompletionBatch.*`, and existing
`SessionController.*` tests. Production controller behavior may still use a
short-lived compile bridge within this phase, but do not commit a final state
with both registry and executor APIs serving the same role.

## Phase 4: Give the controller sole batch ownership

Update `src/session/session_controller.h/.cpp` to use the new types directly.

### 1. Replace controller members

Replace:

```cpp
ThreadPool worker_pool_;
AgentRegistry registry_;
std::optional<ResponseBatch> batch_;
```

with:

```cpp
ThreadPool worker_pool_;
CompletionExecutor completion_executor_;
std::optional<CompletionBatch> batch_;
```

Keep the pool declared before the executor and the batch declared after the
executor so reverse destruction releases the batch before the executor and
the executor before the pool. Retain explicit shutdown as the primary lifetime
boundary; declaration order is construction-failure fallback.

Remove `ResponseBatch`. Keep `ActiveResponse`, because it represents the one
durably activated and visibly streaming transcript response, not backend batch
execution.

Controller-only `terminal_notices` and `stop_notice_recorded` may become direct
private fields. They must be reset in the one helper that releases `batch_`.
Do not put runs, indices, cancellation, or completion flags beside them.

Use `completion_executor_.runtime_info()` wherever the controller constructs
`ForumCharacters` or formats agent information.

### 2. Build completion inputs once

Refactor `start_batch()` to construct one `vector<CompletionInput>` directly.
Each input receives:

- the shared immutable history;
- its assigned request ID;
- target `CharacterInfo`;
- author identity;
- prompt text.

Do not first build a controller run vector and copy it into inputs.

Stage into a local `CompletionBatch`, then move-emplace it into `batch_`.
Preserve the current behavior where a staging `runtime_error` returns
`Request could not be dispatched`, leaves input unconsumed, and may leave gaps
in assigned request IDs.

### 3. Activate from the batch

Change `activate_current_run()` to read:

```cpp
const RunSpec& run = batch_->foreground_run();
```

Use the batch's foreground position only for the existing activation test hook.
Do not pass it to the executor or use it to index another collection.

After durable turn creation, transcript insertion, and `ActiveResponse`
installation succeed, call `batch_->open()` for the first run. An activation
exception must reset the unopened batch; RAII then cancels the gate and waits
for all accepted tasks without calling a backend.

### 4. Receive and advance through one object

Replace:

```cpp
registry_.try_receive(batch_->foreground_index, event);
```

with:

```cpp
batch_->try_receive_foreground(event);
```

On a normal non-final terminal event:

1. finish persistence and update `ActiveResponse` exactly as today;
2. append the child's notice;
3. call `batch_->advance_foreground()`;
4. activate `batch_->foreground_run()`.

Do not advance on abort. Later child queues are discarded after all executions
finish, matching current behavior.

### 5. Read cancellation from the batch

Refactor `request_stop()`, generation projection, run finishing, and abort
polling to use:

- `batch_->cancel()`;
- `batch_->cancellation_requested()`;
- `batch_->executions_finished()`;
- `batch_->foreground_run()`.

`request_stop()` must remain non-blocking. It requests cancellation, requests
a snapshot, and returns `Stopping generation...`; it does not call
`wait_until_finished()`.

When no `ActiveResponse` remains and every execution has finished, release the
batch and emit the same final notices as today.

### 6. Centralize release and notice reset

Replace paired `abandon_batch()` / `registry_.clear_batch()` calls with one
controller helper that:

1. explicitly waits if the calling path requires execution safety now;
2. resets `batch_`;
3. clears controller-only batch notice accumulation.

Capture any notice text needed for the returned `ControllerUpdate` before
calling this helper. Use it from normal completion, aborted completion,
activation failure, and shutdown so later operations cannot inherit stale
notice state.

### 7. Preserve explicit shutdown ordering

Rewrite `SessionController::shutdown()` in this order:

1. return if already shut down;
2. set `shutdown_`;
3. if `batch_` exists, call `cancel()` and `wait_until_finished()`;
4. drain the active foreground terminal event while the batch and notifier are
   alive;
5. release the batch and `ActiveResponse`;
6. call `worker_pool_.stop()` before allowing the executor to be destroyed.

Retain a catch path that calls `worker_pool_.stop()` before rethrowing if
terminal persistence fails during draining. Do not rely only on the
`CompletionBatch` or `ThreadPool` destructor for this ordering.

### 8. Update controller tests

Run and, where necessary, adapt the existing tests without changing their
observable expectations:

- generation status and busy rejection;
- single prompt success, cancellation, and failure;
- multicast full fan-out and selected-order persistence;
- background completion before foreground completion;
- stopping during a multicast;
- restarting after completion, cancellation, activation failure, and
  persistence failure;
- shutdown with live work;
- shutdown with a delayed final notifier wake;
- event-drain bounds and update merging;
- independent concurrent controllers.

Add a regression that would have exposed the old cross-object invariant:
selected targets complete out of order, each emits identifiable content, and
the controller always pairs each prompt, answer, request ID, and participant
with the batch's corresponding foreground slot.

### Phase 4 checkpoint

Build and run:

```sh
build/ninja/cha_tests \
  --gtest_filter='CompletionExecutor.*:CompletionBatch.*:SessionController.*:ConcurrentControllers.*'
```

Then run the full C++ suite. Do not proceed while any concurrency test is
flaky; repeat the focused suite enough times to exercise the scheduling paths
without adding sleeps to production or tests.

## Phase 5: Remove the old registry and update architecture documentation

1. Delete `src/agents/agent_registry.h/.cpp` and
   `tests/agents/unit_agent_registry.cpp`.
2. Remove their entries from `CMakeLists.txt` and ensure the new files are
   listed exactly once.
3. Replace includes and terminology throughout `src/`, `tests/`, and comments.
4. Update `src/agents/README.md`:

   - replace `AgentRegistry` with `CompletionExecutor` and `CompletionBatch`;
   - show the controller owning the batch;
   - document foreground routing without an external index;
   - retain the staging, terminal-delivery, cancellation, buffering, volatility,
     and shutdown guarantees.

5. Update `src/session/README.md` so `session_controller.*` is described as
   owning an optional `CompletionBatch`, while the agent layer owns backend
   execution mechanics.
6. Search for stale concepts:

   ```sh
   rg -n 'AgentRegistry|ResponseBatch|BatchRecord|registry_|cancel_batch|clear_batch|open_gate|try_receive\([^)]*index|foreground_index.*execut' src tests CMakeLists.txt
   ```

7. Inspect every remaining `foreground_index` use. It may exist inside
   `CompletionBatch` and in the activation fault-injection accessor, but it
   must not connect two containers or classes.
8. Inspect every `wait_until_finished()` call and every batch reset. Confirm
   that waits occur on the owner thread and that backend, pool, and notifier
   lifetimes remain valid.

### Phase 5 checkpoint

Build from the normal configured tree and run:

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure
```

Run the repository's existing ThreadSanitizer configuration because this
change moves concurrency ownership:

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --test-dir build/tsan --output-on-failure
```

Also run the existing AddressSanitizer/UBSan configuration before final review:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --test-dir build/asan-ubsan --output-on-failure
```

Record a toolchain or environment limitation if either existing sanitizer
cannot run; do not create a new sanitizer build system as part of this redesign.

## Final review checklist

The implementation is complete only when all of the following are true:

- `SessionController` owns exactly one optional `CompletionBatch`.
- There is no controller-owned vector of batch runs.
- There is no executor-owned optional current batch.
- The `RunSpec` returned for foreground activation and the event queue consumed
  for foreground delivery belong to the same execution slot.
- No API accepts a controller-provided run index to select an executor queue.
- Cancellation has one batch-level source of truth.
- Staging remains failure-atomic and no backend runs before the gate opens.
- Multicast backends still run concurrently and transcript turns remain
  foreground ordered.
- `/stop` remains non-blocking.
- Batch destruction cannot leave an execution accessing a backend.
- Shutdown joins the worker pool before backend or notifier lifetime ends,
  including persistence-exception paths.
- Controller notices, snapshots, appends, database transitions, and web-facing
  behavior are unchanged.
- All old registry files, tests, includes, comments, and CMake entries are gone.
- The focused concurrency tests and complete C++ suite pass.
