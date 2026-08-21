# Implementation plan: process-wide provider execution

This plan implements [docs/design.md](design.md). Read that document first; this
one defines sequencing, temporary compatibility requirements, and verification.

## How to use this plan

The change is split into five blocks. Blocks run in order, and every block must
leave the complete application buildable and tested before the next begins.

- A later block assumes every earlier block has landed.
- A block ends when `make test` is green. Changes are left uncommitted for
  review; the next work session starts from the reviewed working tree.
- Blocks 3 and 4 add concurrency or lifetime behavior and also run the
  ThreadSanitizer preset.
- If a block cannot be finished, stop at the last green state and record the
  remaining work in that block. Do not begin the next block from a broken tree.

Commands used throughout:

```bash
make test
```

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --test-dir build/tsan --output-on-failure
```

The final block additionally runs ASan/UBSan and integration tests.

## Decisions already made

- `ModelBackend` survives as the polymorphic transport/test seam.
- A `ProviderClientFactory` returns a client plus the private resolved
  configuration needed to create another equivalent client. The first result
  may perform API-key resolution and model discovery; later calls receive the
  resolved configuration and must not repeat discovery.
- `GenerationExecutor` is removed at the session cutover. It is not retained as
  a forwarding adapter.
- The process worker-pool size is `WebSettings::session_limit`, with no new
  user-facing setting.
- `describe()` eagerly initializes provider state during session open, matching
  today's behavior.
- `stage_batch()` submits nothing. `GenerationBatch::open()` is the submission
  point, after durable session state exists.
- No compatibility mode may add a second scheduler, a global service locator,
  or a second production `Providers` instance.

## Block map

| Block | Scope | Primary risk |
| --- | --- | --- |
| 1 | Carry provider identity | Data plumbing |
| 2 | Make request inputs explicit and clients reusable | Protocol regressions |
| 3 | Add provider state, initialization, and client leasing | Cache concurrency |
| 4 | Add global execution and cut sessions over atomically | Concurrency and lifetime |
| 5 | Reload coverage, documentation, and final verification | Missed integration behavior |

Blocks 1 and 2 preserve runtime ownership and behavior. Block 3 adds a
process-capable provider-state component but does not alter live generation.
Block 4 is deliberately one coherent cutover: rewriting `GenerationBatch`,
removing the session executor, creating the process owner, and fixing shutdown
cannot be separated without an extra temporary execution path.

---

## Block 1 — Carry provider identity

**Goal.** A resolved character definition names the provider it came from, and
resolved configurations can be compared exactly. Nothing else changes.

### Steps

1. Add `ProviderSelection` (`id` plus `config`) to `src/agents/character.h`.
2. Give `ModelBackendConfig` a defaulted `operator==` in
   `src/agents/character_config.h`. Add a test asserting that configs differing
   only in `api_key` compare unequal. The cache key may compare this secret but
   must never log it.
3. Replace `CharacterDefinition::backend` with `ProviderSelection`. The provider
   name is already read by `resolve_provider()`; carry it into
   `LoadedCharacterConfig` and then the runtime definition instead of discarding
   it.
4. Update all `CharacterDefinition` construction sites and test fixtures.
5. Update `tests/agents/unit_config_loader.cpp` and
   `tests/agents/unit_character_definition_loader.cpp` to assert that provider
   identity survives resolution.

### Scope boundaries

- Do not introduce `Providers`, change execution ownership, or touch the worker
  pool.
- Do not change provider parsing or validation. Character configuration remains
  the sole provider selector, and the named provider file remains the source of
  `ModelBackendConfig`.

### Done when

`make test` is green and every loaded character exposes its provider ID and
exact resolved configuration.

---

## Block 2 — Make request inputs explicit and clients reusable

**Goal.** `ProviderClient` retains only provider connection state. Character
metadata and the system prompt travel with each request, while the existing
session-owned executor continues to work.

### Steps

1. Introduce the stable request boundary used by both the old executor and the
   future `Providers` component:

   ```cpp
   struct ProviderRequest {
     std::shared_ptr<const CharacterDefinition> definition;
     GenerationRequest generation;
   };
   ```

   Put it in a small agent-layer header that both `GenerationBatch` and the
   later `providers.h` can include without a cycle.
2. Change `ModelBackend::prepare()` to receive the immutable character metadata
   and system prompt from `ProviderRequest` alongside `GenerationRequest`.
   Update test backends accordingly.
3. Make `GenerationExecutor` retain shared immutable character definitions in
   parallel with its existing clients. When it resolves a target, construct a
   `ProviderRequest` containing that definition and pass it to
   `GenerationBatch`.
4. Change each current `GenerationBatch::Execution` to own a `ProviderRequest`
   rather than only a `GenerationRequest`. It still borrows the same backend,
   uses the same start gate, and submits to the same session pool in this block.
5. Remove `character_` and `system_prompt_` from `ProviderClient`. Construct it
   from `ProviderSelection`; use the request's definition during `prepare()`.
6. Separate provider initialization from ordinary client construction. Define a
   factory result containing:
   - `std::unique_ptr<ModelBackend>`;
   - `ProviderRuntimeInfo`;
   - a private resolved `ModelBackendConfig` with the chosen model and resolved
     API key, suitable for creating another client without discovery.

   The initial production factory call may resolve credentials and discover a
   model. A call using that resolved config creates only a curl/client instance.
   Tests supply the same factory shape with fake backends.
7. Keep `ModelBackendInfo` as character metadata combined with
   `ProviderRuntimeInfo`, so forum and session views remain unchanged.
8. Update `src/agents/README.md` to state the exclusive-use contract: one client
   owns one curl easy handle and serves one request at a time.

### Scope boundaries

- `SessionController` still owns its `ThreadPool` and `GenerationExecutor`.
- `GenerationExecutor` still owns one client per character. Its new definition
  retention is required to supply per-request data and is removed with the
  executor at cutover; the `ProviderRequest` and batch input shape survive.
- Do not add caching, leasing, or process-wide threads.
- Do not weaken `unit_provider_client.cpp`; it remains the transport regression
  suite.

### Done when

`make test` is green, existing generation behavior is unchanged, and a
`ProviderClient` can prepare calls for two different character definitions
without retaining either definition.

---

## Block 3 — Add provider state and client leasing

**Goal.** Add a standalone `Providers` component that initializes and caches
provider connections, but does not yet schedule generation or affect live
sessions.

### Steps

1. Add `src/agents/providers.h`, `src/agents/providers.cpp`, and their build
   entries. In this block `Providers` exposes construction, `describe()`, and
   shutdown; `stage_batch()` is added during the execution cutover.
2. Store provider states in a mutex-guarded vector with exact
   `(provider id, original resolved ModelBackendConfig)` equality. Keep the
   original selection as the cache key and the private initialized config as
   client-construction state.
3. Implement initialization shared by concurrent callers:
   - one caller invokes the factory for a missing selection;
   - other callers wait for that initialization;
   - the first created client becomes an idle client in the state;
   - every waiter observes the same success or failure;
   - a failed state is removed only after its current waiters can observe the
     failure, allowing a later call to retry.
4. Implement `describe()` by initializing or finding the state and returning
   its `ProviderRuntimeInfo`.
5. Implement exclusive client leasing. Reuse an idle client when available;
   otherwise invoke the factory with the state's private initialized config so
   API-key resolution and `/models` discovery are not repeated. Return healthy
   clients and discard unsafe ones.
6. Add an idempotent preliminary `shutdown()` that rejects new `describe()`
   calls and destroys idle provider states. No worker pool or execution registry
   exists in this block.
7. Write `tests/agents/unit_providers.cpp` for:
   - state reuse for identical `(id, config)`;
   - distinct states for the same ID with different configs;
   - one initialization shared by concurrent first callers;
   - failed initialization observed by current waiters and retried later;
   - exclusive concurrent leases;
   - extra clients created without repeated discovery;
   - healthy-client reuse and broken-client discard;
   - shutdown rejection and state destruction.

### Scope boundaries

- Do not modify `GenerationBatch`, `SessionController`, `session_open`, or
  `web_main`.
- Do not submit generation work to `Providers` yet.
- `GenerationExecutor` remains the only live execution path and requires no
  compatibility overload for provider states.

### Done when

`make test` and the ThreadSanitizer preset are green. Production behavior is
still unchanged.

---

## Block 4 — Global execution and atomic session cutover

**Goal.** Move generation, threads, clients, and curl handles out of sessions
and establish the complete process lifetime in one green change.

This block is cohesive by necessity. `GenerationBatch` is the existing live
path, so its global-pool rewrite and the controller cutover must land together.
Keeping either half temporarily would require a second execution mode whose only
purpose is migration.

### Steps

1. Give `Providers` its fixed `ThreadPool` and add the design's
   `stage_batch()` interface. Staging resolves every state and constructs inert
   executions but submits nothing.
2. Rework `GenerationBatch` for provider execution:
   - remove `StartGate` and its condition variable;
   - retain the `ProviderRequest` input introduced in Block 2;
   - replace the borrowed `ModelBackend&` with the execution's provider-state
     lease source;
   - make `open()` submit every execution to the global pool;
   - make `open()` non-throwing: allocation or pool-submission failure completes
     the affected execution in place with a failure event;
   - add `Staged / Queued / Running / Finished` transitions;
   - complete cancelled staged and queued executions immediately;
   - lease a client only after the execution wins the transition to `Running`.
3. Implement the abandoned-closure rule. A queued closure that observes
   `Finished` touches only its shared execution object and returns. It must not
   call the notifier, publish an event, inspect or lease provider state, or run
   the normal completion path. Remove the current unconditional final wake from
   this path.
4. Complete `Providers::shutdown()`: reject `describe()` and staging, stop and
   join the pool, then destroy cached states. `ThreadPool::stop()` drains
   accepted closures, so no execution registry is added.
5. Add `Providers&` to `SessionController`. Build `characters_` by combining
   each definition's metadata with `describe()` and remove `worker_pool_` and
   `generation_executor_`.
6. Change `SessionController::start_batch()` to create `ProviderRequest`s and
   call `Providers::stage_batch()`. Keep the durable
   `activate_current_run()`-then-`open()` sequence unchanged.
7. Replace `from_backends_for_testing`. Controller tests construct a real
   `Providers` with the fake factory defined in Block 2. Update
   `tests/support/test_backends.h`, `test_controller.h`, `test_generations.h`,
   and `test_live_session.h`.
8. Delete `generation_executor.{h,cpp}` and its unit test. Move any unique
   behavioral coverage into `unit_providers.cpp` or
   `unit_generation_batch.cpp` before deleting it.
9. Thread `Providers&` through `open_session()` and
   `WorkspaceRuntime::open_session()`.
10. In the same change, construct exactly one `Providers` in
    `prepare_and_run()` before `LiveSessionManager`, sized from
    `WebSettings::session_limit`, and capture it by reference in the opener.
    `web_main` must never be left with a call to `open_session()` for which no
    provider owner exists.
11. Make process shutdown ordering explicit with an inner manager/server scope:
    - stop HTTP admission and join live-session owners through the existing
      shutdown coordinator;
    - destroy `LiveSessionManager` after its owners are joined;
    - call `Providers::shutdown()`;
    - only then call `shutdown_diagnostic_logging()`.

    Remove logging shutdown from `run_web_server()` so both normal return and
    bind failure follow this order. Curl global cleanup remains the existing
    function-local static, which runs after automatic objects leave `main`.
12. Add or update tests for:
    - staging occupies no worker until `open()`;
    - staged cancellation reaches no provider;
    - queued cancellation finishes without being dequeued;
    - a queued closure can run after its batch and notifier are destroyed,
      without waking, publishing, or leasing a client;
    - running cancellation reaches curl/backend once;
    - partial submission failure produces failure events without throwing;
    - `/mcast` concurrency is bounded by global capacity;
    - presentation remains in target order regardless of execution order;
    - two controllers have isolated queues and share provider state;
    - a slow durable commit in one session occupies no global worker and does
      not delay another session;
    - shutdown destroys clients after session owners and before logging ends.

### Important test wording

- Do not assert that provider executions start in target order. FIFO queue
  removal does not imply observable start order across several workers.
- Do assert that at most the pool capacity runs concurrently and that the
  controller presents multicast results in target order.
- The abandoned-closure test destroys session-owned state, especially the
  notifier. It does not require `ProviderState` destruction: the cache owns that
  state until provider shutdown, and an execution may keep it alive through a
  `shared_ptr`.

### Scope boundaries

- Do not change transcript construction, durable persistence, foreground
  selection, or presentation logic.
- Do not add a temporary scheduler, forwarding executor, service locator,
  execution registry, or second production `Providers` instance.
- Do not add priorities, per-provider limits, or a new pool-size setting.

### Done when

`make test` and ThreadSanitizer are green, `chaweb` contains one process-owned
`Providers`, and every active session has stopped owning provider execution.

---

## Block 5 — Reload coverage, documentation, and final verification

**Goal.** Prove configuration-version behavior through the correct boundaries,
update documentation, and run the complete verification matrix.

### Steps

1. Add `Providers` state-version tests independent of the HTTP reload route:
   - an execution already running with selection A retains A;
   - a newly staged request using the same ID with changed config B selects a
     new state;
   - the A state is reclaimed only after its execution and leases release it.
2. Add workspace-reload integration coverage that matches current production
   semantics:
   - the reload reservation cancels and joins all live sessions before
     publishing the new workspace generation;
   - a new session after an unchanged reload reuses provider state;
   - a new session after a changed config uses the new state;
   - no test expects a live session request to continue through the production
     reload route, because that route deliberately closes live sessions first.
3. Verify shutdown with observable ownership rather than hooks into libcurl's
   private static:
   - fake clients are destroyed by `Providers::shutdown()`;
   - no worker or queued closure remains afterward;
   - process wiring shuts providers down before diagnostic logging;
   - cached curl easy handles therefore leave scope before the existing
     function-local curl-global destructor runs after `main`.
4. Update documentation that describes the old system:
   - `src/agents/README.md`: session-owned pool and start-gate sections;
   - `src/session/README.md`: in-flight turn and session control;
   - `docs/design.md`: change the status from planned to implemented only after
     every acceptance criterion is met.
5. Remove stale comments, includes, CMake entries, and test helpers associated
   with `GenerationExecutor`, session pools, or the start gate.
6. Run:

   ```bash
   make test
   cmake --preset asan-ubsan
   cmake --build --preset asan-ubsan
   ctest --test-dir build/asan-ubsan --output-on-failure
   cmake --preset tsan
   cmake --build --preset tsan
   ctest --test-dir build/tsan --output-on-failure
   make itest
   ```

### Done when

All twelve acceptance criteria in `docs/design.md` hold. In particular:

- no session owns threads, provider clients, or curl handles;
- no worker waits on session state;
- queued cancellation does not extend session lifetime;
- abandoned closures touch nothing owned by a destroyed session;
- provider state is reused only for exact selection/configuration matches;
- reload and shutdown behavior match the production routes and object lifetime.

## Mapping to the design

The design's five implementation steps still map to five blocks. The only
sequencing refinement is that provider-state caching is built alone in Block 3,
while global batch execution, controller cutover, composition-root ownership,
and shutdown land together in Block 4. This preserves a green application
without temporary scheduling machinery.
