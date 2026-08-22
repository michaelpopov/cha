# Implementation plan: request-owned provider execution

This plan implements [design.md](design.md). The design document is the source
of truth for behavior and ownership; this document defines the implementation
order and the checks required at each boundary.

The work is divided into five substantial blocks. Each block is intended to fit
one focused Codex session and must leave the repository buildable and its normal
test suite green. Blocks 1–3 prepare and prove the new boundaries while the
current `GenerationExecutor`/`GenerationBatch` path remains the only production
execution path. Block 4 is the atomic production cutover. Block 5 hardens the
new lifecycle and completes cleanup and documentation.

## Rules for every block

- Read the current `docs/design.md` before starting; do not implement from this
  plan if the design has since changed.
- Preserve unrelated worktree changes.
- Prefer direct ownership and existing types. Do not add a scheduler, task
  queue, provider cache, client lease, concurrency limit, or service locator.
- Do not reintroduce model discovery or `/models` calls.
- Do not store resolved credentials outside the request-local transport.
- Add no temporary second production execution path. Transitional adapters may
  exist only inside the old executor while it remains the sole production path,
  and must be removed in Block 4.
- Run `make test` before finishing each block.
- Update nearby comments as ownership changes; defer broad README rewrites to
  Block 5.

For concurrency blocks, also run:

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --test-dir build/tsan --output-on-failure
```

## Block map

| Block | Logical result | Production execution after block |
| --- | --- | --- |
| 1 | Provider identity, required model, and eager credential-name validation | Existing executor and batch |
| 2 | Shared immutable definitions, static runtime information, and the final client factory boundary | Existing executor and batch |
| 3 | Fully tested standalone `Providers` and `ProviderRequest` | Existing executor and batch |
| 4 | Atomic controller, session-opening, process-owner, and shutdown cutover; old execution code removed | Request-owned execution |
| 5 | Reload/process race coverage, sanitizer verification, and documentation cleanup | Request-owned execution |

---

## Block 1 — Provider identity and eager configuration validation

### Goal

Make a loaded character carry the exact named provider snapshot needed by a
future request. Eliminate model discovery and move configuration mistakes that
can be detected locally to workspace loading. Do not change generation
ownership or session scheduling in this block.

### Implementation steps

1. Add `ProviderSelection` near `CharacterDefinition` in the agent
   configuration layer:

   ```cpp
   struct ProviderSelection {
       std::string id;
       ModelBackendConfig config;
   };
   ```

   Replace the bare `CharacterDefinition::backend` with the selection. Do not
   add equality or hashing; no cache needs them.

2. Preserve the provider name that `load_character_config()` already reads.
   Carry it through `LoadedCharacterConfig` and into every
   `CharacterDefinition`, including built-in definitions and the application
   assistant. Update trusted test factories to supply an explicit provider ID.

3. Require a non-empty configured model when a referenced provider is loaded.
   Report the provider configuration path and provider ID. Remove tests and
   fixture data that relied on an empty model meaning automatic selection; add
   explicit models to workspace and test provider files.

4. When a referenced provider has a non-empty `api_key_env`, validate during
   workspace loading that `getenv()` returns a non-null, non-empty value. The
   error may include the variable name and provider path, but never the value.
   Keep an empty `api_key_env` valid for providers that require no credential.

5. Keep the validation ordering explicit:

   - production `web_main` loads `.env` before constructing
     `WorkspaceRuntime`;
   - `WorkspaceDefinition::load()` eagerly resolves all forum definitions;
   - a failed initial load prevents startup;
   - a failed `WorkspaceRuntime::reload()` candidate is never published;
   - no production code mutates the process environment after startup.

6. Remove model-discovery behavior from `ProviderClient`: delete
   `discover_model()`, `models_endpoint()`, the GET request construction, and
   discovery-only logging and tests. Client construction must reject an
   impossible empty model defensively rather than contact the provider.

7. Update configuration logging so it always reports the configured model and
   provider ID without logging the complete configuration, API key, or
   credential value.

8. Update all direct `.backend` users to read the configuration from the
   provider selection. Keep changes mechanical outside the loader and client;
   do not introduce a provider registry or lookup table.

### Tests

Add or update focused tests in the configuration, character-definition, and
workspace suites:

- a resolved character retains both provider ID and effective configuration;
- every referenced provider requires a non-empty model;
- no test observes a `/models` call;
- unset and empty `api_key_env` values reject workspace loading;
- a present, non-empty environment variable allows loading without exposing
  its value in runtime information or diagnostics;
- changing a provider file under the same ID affects only definitions loaded
  afterward;
- a failed reload candidate is not published;
- environment tests set variables before loading and restore them afterward.

### Boundaries

- `GenerationExecutor`, `GenerationBatch`, and the session `ThreadPool` remain
  unchanged as production owners.
- `ProviderClient` may still be constructed during session opening through the
  old executor. The final lazy request-time construction happens in Block 4.
- Do not add `Providers` or request threads here.

### Exit criteria

`make test` is green, every loaded character has a named provider snapshot,
missing model/credential-variable errors occur during workspace loading, and
the repository contains no model-discovery implementation.

---

## Block 2 — Immutable character ownership and transport boundary

### Goal

Prepare the data and transport interfaces that request-owned execution will
use, while keeping the old executor and batch as the sole production scheduler.
Move character-facing runtime information away from initialized backends.

### Implementation steps

1. Introduce the shared immutable definition form used by controllers,
   requests, and clients:

   ```cpp
   using SharedCharacterDefinition =
       std::shared_ptr<const CharacterDefinition>;
   ```

   Add one small conversion helper for turning an ordered vector of loaded
   definitions into ordered shared definitions. Reject null definitions at the
   receiving boundary.

2. Change `SessionController` construction to retain the ordered shared
   definitions. Keep their order identical to the forum roster. The controller
   must be able to resolve a character ID directly to its shared definition;
   a simple linear lookup is sufficient for CHA's small rosters.

3. Build one immutable `ModelBackendInfo` record per character directly from
   its definition and `ProviderSelection`. Keep the existing data type unless a
   rename clearly reduces churn. Extract any pure endpoint-formatting helper
   needed from `ProviderClient`; constructing runtime information must not read
   credentials, create curl, or perform network I/O.

4. Make the controller's `ForumCharacters` from those records and use the same
   immutable records for `/characters`, `/info`, and style-reset baselines.
   Several characters sharing a provider must still retain separate metadata
   and appearances.

5. Define the permanent test seam:

   ```cpp
   using ProviderClientFactory =
       std::function<std::unique_ptr<ModelBackend>(
           SharedCharacterDefinition)>;
   ```

   Place it with the provider-client/provider execution boundary rather than
   in the session layer.

6. Refactor `ProviderClient` to accept and retain a
   `SharedCharacterDefinition`. It reads transport configuration from the
   definition's provider selection and uses the definition's system prompt in
   `prepare(const GenerationRequest&)`. Keep `ModelBackend::prepare()` and
   `GenerationRequest` unchanged.

7. Adapt the transitional `GenerationExecutor` to construct its per-character
   clients through this factory from shared definitions. It may continue to
   own those clients and schedule batches until Block 4, but production runtime
   metadata must now come from definitions, not `ModelBackend::info()`.

8. Remove production uses of `ModelBackend::info()`. If the old
   `from_backends_for_testing` helper still needs the virtual operation until
   the atomic cutover, mark that use as temporary and remove both in Block 4;
   do not build new behavior on it.

9. Verify that `Transcript::model_history()` remains the single synchronous
   owning snapshot operation. Normal generation creates one
   `SharedModelHistory`; multicast creates one and shares it among all child
   `GenerationRequest` values. No worker-facing type may retain
   `TranscriptView`.

10. Update provider-client, executor, controller-runtime-info, style, and test
    fake construction to the shared-definition factory. Do not add a parallel
    `GenerationRequest` replacement.

### Tests

- the factory receives the exact shared definition selected for a request;
- `ProviderClient::prepare()` projects that definition's system prompt;
- one shared definition safely constructs multiple request-local clients;
- runtime information is available without constructing a client;
- two characters sharing one provider yield two runtime records with distinct
  metadata and configured appearances;
- style override and reset use the selected character's immutable baseline;
- `/characters` and `/info` preserve current output and ordering;
- a captured `SharedModelHistory` is unaffected by later transcript mutation;
- multicast inputs share the same history pointer.

### Boundaries

- The existing executor still owns session clients, and the existing batch and
  thread pool still execute generation.
- Do not create request-owned threads or queues yet.
- Do not change `/stop`, session destruction, production opener signatures, or
  process shutdown in this block.

### Exit criteria

`make test` is green; shared immutable definitions and static runtime records
are the authoritative character data; the provider-client factory has its final
shared-definition signature; and the old executor still provides the only
production execution path.

---

## Block 3 — Standalone `Providers` and `ProviderRequest`

### Goal

Implement and thoroughly test the complete process-level request execution
component without wiring it into sessions. This block establishes the hardest
threading and lifetime invariants before the production cutover.

### Implementation steps

1. Add `ProviderRequestInput`, composing existing types rather than duplicating
   them:

   ```cpp
   struct ProviderRequestInput {
       SharedCharacterDefinition character;
       GenerationRequest generation;
   };
   ```

2. Add `ProviderRequest` and `Providers`, preferably in one small
   `providers.h/.cpp` pair unless separation materially improves readability.
   Keep the public API limited to `run()`, `try_receive()`, `cancel()`,
   `make_request()`, and `shutdown()`.

3. Add `ProviderThreadLauncher` as a test seam. The production launcher must
   create and detach exactly one `std::thread`; an injected launcher either
   launches exactly once or throws before launch.

4. Give each request its own:

   - owning `ProviderRequestInput`;
   - `ConcurrentQueue<GenerationEvent>`;
   - atomic cancellation flag;
   - exactly-once terminal state;
   - shared `WakeNotifier`;
   - request token used only for registry supervision and diagnostics.

5. Implement one shared internal registry state containing only the admission
   flag, active request map, mutex, condition variable, and next token. Do not
   add provider state or provider-information storage. `Providers` and detached
   closures share this registry object; workers never retain a raw
   `Providers*`.

6. Make `make_request()` thread-safe and preserve the durable caller contract:

   - construct a request handle for every non-fatal outcome;
   - validate unexpected bad input into a failed terminal handle;
   - if admission is closed, return a failed handle without launching;
   - register a valid request before launching;
   - synchronize admission/registration/launch so shutdown cannot admit work
     after closure;
   - if thread creation throws, publish one failure terminal, remove the
     registry entry, and return the handle;
   - allow allocation failure to follow the existing fatal policy.

7. In the worker, check cancellation, invoke `ProviderClientFactory` with the
   request's shared definition, call `prepare(input.generation)`, then perform
   the synchronous provider operation. Create one fresh client and curl easy
   handle per request.

8. Publish each delta to the private queue with the request ID attached, then
   wake the shared notifier. Convert completed, cancelled, provider, protocol,
   and caught-exception outcomes into exactly one terminal event using
   `close_with()` so buffered deltas remain readable first.

9. Keep cancellation idempotent and non-blocking. A pre-transport cancellation
   skips client construction; an active request relies on curl's progress
   callback. Cancellation races must never publish a second terminal event.

10. Enforce worker completion ordering:

    1. publish the terminal event;
    2. destroy the `ModelBackend`, curl handle, callbacks, response state, and
       request-local credential;
    3. unregister from the active registry and notify shutdown waiters;
    4. perform only shared-pointer release and return.

11. Keep `ProviderRequest` and registry-state destructors passive: no join,
    unregister, notifier call, logging, or callback into `Providers`. The last
    request reference is allowed to disappear on the worker thread.

12. Implement idempotent `Providers::shutdown()` by closing admission,
    cancelling a snapshot of active requests, and waiting for registry
    emptiness. Registry emptiness means transport quiescence, not necessarily
    that each detached closure has executed its final return instruction.

13. Keep credential resolution request-local. Read `api_key_env` on the worker
    even though workspace loading already validated presence. Treat an
    unexpected missing value as that request's failure. Never cache or log the
    resolved value.

14. Add concise request lifecycle diagnostics using provider ID, request IDs,
    internal token, configured model, duration, and active count only.

### Tests

Create focused provider-request tests with fresh fake backends from the
factory. Cover at least:

- immediate start and no concurrency cap;
- distinct backend instances, queues, and cancellation flags;
- delta ordering followed by exactly one terminal event;
- completion, provider failure, thrown exception, and cancellation;
- cancellation before client creation and during `perform()`;
- thread-launch failure returning a failed handle;
- `make_request()` racing with shutdown;
- `make_request()` after shutdown launching nothing;
- one failed multicast-style request not affecting another;
- dropping the consumer handle while the registry keeps work alive;
- notifier wake after the original session owner has gone;
- last-reference destruction on the worker tail;
- curl/client destruction before registry removal;
- request A retaining configuration A while a later request with the same
  provider ID uses configuration B;
- repeated requests independently resolving credentials without sharing
  credential state;
- idempotent shutdown and no outer-`Providers` access after unregister.

Run both `make test` and the complete ThreadSanitizer command sequence for this
block.

### Boundaries

- Do not inject `Providers` into `SessionController`, `WorkspaceRuntime`,
  `LiveSession`, or `web_main` yet.
- Do not change normal generation, multicast, `/stop`, or session destruction.
- The old executor remains the sole production path.

### Exit criteria

The standalone component satisfies the request, registry, cancellation,
terminal-event, and detached-tail invariants under normal tests and TSan, while
no production session can call it yet.

---

## Block 4 — Atomic production cutover and old execution removal

### Goal

Switch the entire production ownership graph from session-owned executor/batch
execution to the one process-owned `Providers`. Controller behavior,
session-opening plumbing, composition-root ownership, shutdown order, test
harnesses, integration helpers, and deletion of the old scheduler land
together. Do not leave a reviewed state with two production execution paths.

### Construction and data ownership

1. Change `WorkspaceDefinition` session-opening access to return the immutable
   definitions already validated in the current workspace generation. Stop
   reparsing character/provider definitions during session opening; changes to
   those files take effect through workspace reload. Preserve the existing
   default-character/default-persona selection behavior separately.

2. Convert the selected forum definitions to ordered
   `SharedCharacterDefinition` values once and pass them through session
   construction. Remove the obsolete open-time character-settings fallback
   notice if no path can produce it after this change.

3. Change the free `open_session()` and
   `WorkspaceRuntime::open_session()` signatures to take `Providers&` and
   `std::shared_ptr<WakeNotifier>`. They pass both into
   `SessionController`; no session-opening layer constructs a provider client,
   reads the environment, or performs provider I/O.

4. Change `web::SessionOpener` to receive the shared notifier. The production
   opener captures the process-owned `Providers&` and forwards it to
   `WorkspaceRuntime`. `LiveSession` passes its existing
   `shared_ptr<OwnerWakeSignal>` rather than a borrowed reference.

5. Construct exactly one `Providers` in `prepare_and_run()` before the
   `LiveSessionManager` ownership scope. Ensure every opener and controller
   that can call it is destroyed before the provider owner.

### Controller execution

6. Remove `ThreadPool`, `GenerationExecutor`, and `GenerationBatch` fields from
   `SessionController`. Add:

   - borrowed `Providers&`;
   - the shared wake notifier used for new requests;
   - ordered shared character definitions;
   - immutable per-character runtime records;
   - one optional session-local active-generation value containing ordered
     `shared_ptr<ProviderRequest>` handles, a foreground index, and only the
     cancellation/presentation state the controller needs.

   This active-generation value is not an executor and owns no queue, thread,
   transport, or wait primitive.

7. Add a direct character-ID-to-definition lookup over the retained ordered
   definitions. Before committing a normal prompt or multicast, resolve every
   target and build every `ProviderRequestInput`, including one synchronous
   `SharedModelHistory` snapshot shared by all multicast targets. Reject
   programmer/input errors before durable state.

8. Preserve durable ordering: activate and journal the first foreground turn,
   then call `make_request()` independently for every prepared target. Because
   every operational outcome is a returned handle with a terminal event, a
   post-commit failure cannot leave an open journal turn without a consumable
   request.

9. Normal generation stores one returned handle. Multicast stores every handle
   in target order; all workers start immediately. A failed target does not
   stop construction or execution of independent later targets.

10. Replace `try_receive_foreground()` with
    `active_generation.requests[foreground]->try_receive()`. Preserve bounded
    draining, request-ID validation, streaming persistence, and target-ordered
    presentation. A later target may finish early but remains isolated in its
    own queue until the controller activates it.

11. After a foreground terminal is persisted, release that handle. If another
    target exists and the generation was not stopped, advance the foreground,
    durably activate its turn, and drain its already-buffered events. Do not
    add a `finished()` operation to `ProviderRequest`.

12. Implement `/stop` exactly as specified by the design:

    - cancel every handle;
    - immediately release and discard all non-foreground handles and queues;
    - retain only the durable foreground request;
    - keep draining that queue asynchronously until its terminal is persisted;
    - never activate a later target;
    - clear the active-generation state and `busy()` after the foreground
      terminal, without waiting for registry cleanup.

13. Implement controller shutdown/destruction without waiting for providers:

    - cancel every retained request;
    - synchronously persist cancellation of the currently durable turn using
      its existing partial-response rules;
    - release every request handle;
    - destroy transcript, journal, lease, and controller state normally.

    Do not drain abandoned non-foreground queues and do not call
    `Providers::shutdown()` from a session.

14. Update `busy()`, `is_generating()`, generation views, notices, and command
    rejection to reflect only session-visible active-generation state. A new
    prompt is valid while older cancelled workers remain in the process
    registry.

15. Keep `/characters`, `/info`, and style reset on the immutable runtime
    records introduced in Block 2. Remove all controller dependence on backend
    runtime information.

### Test construction and migration

16. Replace `from_backends_for_testing` with a test harness that owns objects in
    production order: `Providers`, shared notifier/definitions, then
    `SessionController`. Its destructor destroys the controller first and then
    calls `Providers::shutdown()`.

17. Make controller fake behavior come from `ProviderClientFactory`. Each call
    returns a fresh fake backend for one request. Put coordination counters,
    barriers, scripted deltas, and observations in separately shared test state;
    never reuse a fake `ModelBackend` instance between requests.

18. Convert controller, concurrent-controller, live-session, text-input,
    session-open, web-graph, and C++ integration helpers to the new harness and
    opener signatures. Preserve their existing behavioral assertions before
    adding the new stop/destruction assertions.

### Process shutdown and deletion

19. Make normal process shutdown order explicit:

    1. stop HTTP admission;
    2. request and join all live-session owners;
    3. destroy their controllers and shared notifier owners;
    4. call `Providers::shutdown()` and wait for registry emptiness;
    5. shut down diagnostic logging.

    Refactor `run_web_server()`/`prepare_and_run()` as needed so bind failure,
    normal server stop, and signal stop all preserve this order. The existing
    forced `_Exit` path for an expired session-owner grace remains a forced
    exit.

20. Remove the old production execution implementation and its obsolete tests
    in the same block:

    - `generation_executor.*`;
    - `generation_batch.*`;
    - the application `util/thread_pool.*` if no non-generation user remains;
    - executor/batch/thread-pool CMake entries;
    - start gates, staging hooks, pool-width rules, waiting cleanup, and
      client collections;
    - the temporary `ModelBackend::info()` operation once no test uses it.

### Tests

In addition to converting the existing suites, add controller and production
plumbing coverage for:

- session opening creating no client and performing no provider/environment
  work;
- durable start preceding `make_request()`;
- independent immediate multicast starts and target-ordered presentation;
- one target start/transport failure not affecting another;
- exact `/stop` foreground drain and non-foreground discard behavior;
- `busy()` clearing before abandoned workers unregister;
- a new prompt starting while old cancelled workers remain registered;
- session destruction returning without waiting for a blocked transport;
- a late wake touching only the shared notifier object;
- controller destruction closing the current durable turn synchronously;
- process construction and shutdown ownership order;
- no curl handle remaining when provider shutdown returns.

Run `make test`, `make itest`, and the full TSan sequence. This block is not
complete while any production reference to `GenerationExecutor`,
`GenerationBatch`, or the application generation `ThreadPool` remains.

### Exit criteria

The complete application uses exactly one process-owned `Providers`; sessions
own only request handles and presentation state; the old execution path is
deleted; normal, multicast, stop, destruction, session opening, and shutdown
tests are green; and TSan reports no race in the new ownership graph.

---

## Block 5 — Reload, lifecycle hardening, sanitizers, and documentation

### Goal

Exercise the final design across production reload and process boundaries,
close concurrency coverage gaps, remove stale descriptions, and verify every
acceptance criterion in `docs/design.md`.

### Implementation steps

1. Add provider-level snapshot tests independent of the production reload
   route:

   - request A retains shared definition/configuration A;
   - a later request with the same provider ID and configuration B uses B;
   - A completes or cancels safely after B starts;
   - neither request shares credentials, client state, curl, or queues.

2. Add production workspace-reload coverage matching the real route order:

   - `reserve_workspace_reload()` shuts down and joins every live session
     before loading/publishing the candidate;
   - visible generation is cancelled and never survives into a new session;
   - an old cancelled worker may retain old snapshots only while finishing
     registry cleanup;
   - a new session uses only definitions from the published generation;
   - an unset/empty `api_key_env` rejects the candidate and leaves the old
     workspace generation published;
   - `.env` is not reloaded by workspace reload.

3. Add process-level shutdown tests proving that session owners finish before
   provider admission closes, provider shutdown waits for transport quiescence,
   easy handles and callbacks are destroyed before unregister, and logging
   remains available until provider shutdown completes.

4. Run the registry/shutdown tests repeatedly under TSan, emphasizing:

   - `make_request()` versus `shutdown()`;
   - cancellation versus terminal publication;
   - registry removal versus last request reference;
   - wake-after-session-destruction;
   - several sessions issuing requests concurrently;
   - new work after `/stop` while old cancelled work unregisters.

5. Audit diagnostics. Keep provider ID, request/session IDs, internal request
   token, configured model, active count, and durations. Remove scheduler/pool
   vocabulary and verify that secrets, complete configurations, prompts,
   transcript text, and response bodies are absent.

6. Remove stale includes, comments, helpers, CMake entries, and tests referring
   to generation pools, staged batches, gates, backend leasing, model discovery,
   provider caches, or session-owned provider execution.

7. Update ownership documentation in `src/README.md`, `src/agents/README.md`,
   `src/session/README.md`, `src/web/README.md`, workspace documentation, and
   relevant test-support comments. Keep the HTTP library thread pool clearly
   distinguished from removed generation scheduling.

8. Walk all 23 acceptance criteria in `docs/design.md` against code and tests.
   Change the design status from planned to implemented only after every
   criterion is actually true.

### Final verification

Run:

```bash
make test
make itest

cmake --preset tsan
cmake --build --preset tsan
ctest --test-dir build/tsan --output-on-failure

cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --test-dir build/asan-ubsan --output-on-failure
```

If any web application source or protocol shape changed during the cutover,
also run:

```bash
make web-check
make web-e2e
```

### Exit criteria

All normal, integration, TSan, and ASan/UBSan checks are green; production
reload and shutdown match the design; documentation describes only the final
request-owned architecture; and no deferred scheduler/cache/limit machinery
has been introduced.
