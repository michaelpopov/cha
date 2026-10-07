# Block 2: Complete Assistant maintenance chat

Status: complete. Assistant maintenance chat works in the desktop application
and in ChaWeb. Execute this note on the same checkout as [block 1](block1.md).

Read the repository instructions, block 1's completion note,
[assistant.md](assistant.md), [ass-plan.md](ass-plan.md), and the existing
[Assistant instructions](../resources/assistant-maintenance.md) before coding.
Inspect the actual interfaces produced by block 1 rather than inventing a
parallel store or logger. The design remains the behavior specification.

Scope: the application portions of plan step 5 and steps 6–11, excluding
instruction writing, reference updates, and resource preparation in step 9.
The instructions already exist in `resources/assistant-maintenance.md`;
this block only integrates the supplied content.
Planning allowance: about 400,000 tokens including inspection, implementation,
tests, and fixes. Keep the session below 500,000 tokens and reserve the final
50,000 for validation and completion notes. This estimate is not a measured
guarantee; avoid unrelated cleanup or new infrastructure.

## Outcome and boundaries

Deliver the same five tools through desktop Welcome and `cha-daemon` Welcome
in ChaWeb: list, read, apply/undo, log reads, and temporary buffer logging.
The user diagnoses, repairs, and undoes through chat. Stored save notices and
frontend refresh must work even when the model's final answer fails.

Keep runtime integration, provider dispatch, the reference prompt, and frontend
behavior in this block so the enabled feature has all required parts. Reuse
existing APIs and test suites. Add no maintenance HTTP endpoint, new bridge
event, extra controls, browser test stack, model test tool, or settings revision
protocol. Preserve ordinary conversations and the manual provider Test routine.

## Work in this order

### 1. Native service, request ownership, and logging tools

Implement the native portion of plan step 6 and finish step 5's application work.
Start in `src/app/application*`, `settings_operations.*`, `workspace_operations.*`,
`vault_operations.*`, `src/runtime/live_session*`, `src/session/`, and
`src/providers/providers.*`.

- Add the small `AssistantService` owned by `Application::Impl`. Delegate
  configuration to block 1's store and memory logging to its sink.
- Capture the epoch admitted when Welcome starts from
  `LiveSessionManager::context_epoch()`. Pass it through the session opener to
  `SessionController` and into each maintenance `ProviderRequestInput`, with
  native character/forum/session/request identities. Never obtain a fresh
  epoch in an old request's callback.
- Inject the maintenance executor into `Providers` through the same constructor
  pattern as `WebSearchExecutor`. Workers own copies; bind request context and
  cancellation without capturing raw controllers or live sessions.
- Apply the exact lifecycle and lock rules in design section 9. Use cancellable
  `try_lock_for` acquisition; recheck admission and pre-commit cancellation.
  Release lifecycle/store locks before runtime operations and callbacks.
  Preserve worker joining and application lifetime during shutdown.
- Complete log selection by native level, literal substring, exclusive `after`,
  and last-N chronology. Return one JSON item per log call, entry boundaries,
  effective level/expiry, and explicit lost/limited evidence within size limits.
- Redact saved keys and available OAuth tokens and remove the private workspace
  root from tool logs/errors. Never expose raw snapshot text to the model first.
  Check expiry on every maintenance tool call and return logging state only in
  tool results; add no logging notices.
- On vault switch, drain old workers, reset verbosity, and clear the buffer
  before admitting the new vault. Reset verbosity at shutdown. Preserve store
  revision/undo invalidation and recheck admission before returning log data.

### 2. Runtime effects and stored save notices

Finish native runtime effects in plan step 6 and implement step 7 before
enabling real model writes.

- Reuse typed settings effects for presentation, optional services, ordinary
  session invalidation, and new forum registration. Refreshing all ordinary
  sessions is acceptable when simpler than dependency tracking.
- Never invalidate `builtin-entrance`. Refresh its presentation while its
  Assistant turn continues; subsequent requests use the new inventory.
- Carry epoch and committed version through native effects and save-result
  delivery. Reject obsolete-context delivery. Report post-commit refresh
  failures as saved changes, including restart requirements where applicable.
- Post committed apply/undo results to the runtime thread after releasing
  locks. Store a short `EntryKind::notice` through `make_notice_entry()` and
  `SessionJournal::record_entry()`, then append and publish the snapshot.
- While an answer is active, queue notice text and allocate/store/append it
  after completion, failure, or Stop, preserving entry ordering. Provider
  callbacks must not wait for notice insertion.
- Keep raw tool JSON out of persisted history and use the existing model-history
  exclusion for notice entries. The transient `LiveSession::notice_` field is
  not the saved result. Retain Welcome's existing lifetime.

### 3. Five tools and both provider protocols

Implement plan step 8 in the existing provider loop and protocol adapters.

- Add strict schemas and dispatch for exactly `vault_config_list`,
  `vault_config_read`, `vault_config_apply`, `assistant_logs`, and
  `assistant_logging`. Use block 1's native operations, including undo through
  apply. Read takes no input version; apply and undo require one.
- Attach maintenance tools only to the built-in Assistant in Welcome, using
  reserved identities. Preserve Jev behavior and its `@Assistant` bypass.
- Exclude all web tools and provider-hosted search from maintenance requests,
  continuations, and the final tools-disabled request. Apply request-local
  overrides; never change the saved protected provider. Reject unsolicited web
  calls before executor dispatch. Ordinary forum web behavior stays unchanged.
- Support streaming and non-streaming Chat Completions and Responses, sequential
  calls, usage aggregation, existing cancellation/timeouts, and the existing
  handling of intermediate tool-round text.
- Enforce 64 KiB per editable file, 256 KiB per call's arguments/results,
  512 KiB of results per answer, and 24 calls per answer. Count malformed calls
  and bound streamed arguments. At the call limit, request a final answer with
  tools disabled. Never execute a truncated call.
- Map maintenance output truncation (`length` or `max_output_tokens`) to the
  design's actionable manual-edit message, including incomplete argument JSON.
  Do not claim earlier committed calls were rolled back or mislabel other errors.

Use existing fake transports to test schemas and continuations while wiring
the feature. Do not enable the finished request path without the prompt and
frontend behavior in the next two sections.

### 4. Integrate the independently prepared instructions

Use the existing `resources/assistant-maintenance.md`, prepared independently
of this block. Do not regenerate or rewrite it or update the reference guides here.

- Embed the whole `resources/assistant-maintenance.md`, `docs/MaintainerGuide.md`,
  and `packaging/linux/README.md` as separate `embed_text.cmake` inputs.
  Concatenate their embedded text when constructing the maintenance prompt.
  Use no section slicing, duplicate field catalogue, or help tool.
- Prepend the native host line exactly as specified: `Host: desktop application`
  or `Host: cha-daemon (ChaWeb)`. Do not infer it from the OS or vault settings.
- Include the complete maintenance reference only on maintenance requests.
  Preserve the general application guide, give maintenance restrictions
  precedence, and register build dependencies for both hosts.
- Verify that prompt fixtures for both hosts contain the complete existing
  Assistant instruction resource and the correct native host line.

### 5. Desktop refresh and ChaWeb Welcome

Complete the frontend portion of plan step 6 and all of step 10. Work in
`webapp/src/useLiveSession.ts`, desktop `components/App.tsx`, `Screens.tsx`,
`Settings.tsx`, and `webapp/src/chaweb/`.

- Refresh bootstrap, open entity views, and session lists when Welcome ends
  with completion, failure, or Stop, including read-only answers. Use existing
  generation/submission tracking for fast answers between snapshots; ignore
  repeated idle snapshots. Refresh on opening/reconnecting Welcome and keep
  tracking across navigation with the existing stale-response guards.
- Retain a form's loaded baseline and draft while refetching. Compare detail
  values rather than object identity. Equal data preserves the draft and
  normal Save eligibility. Changed data refreshes a clean form; only a dirty
  form whose detail changed becomes stale and requires reset/reopen before Save.
- Keep Save disabled during a refetch or its failure without discarding the
  draft. A successful retry with equal detail restores normal Save behavior.
  Add no automatic draft merge or revision argument to typed settings writes.
- Show one Welcome entry using `bootstrap.entrance_forum_id` and
  `builtin-welcome`. Remove route/navigation exclusions; permit Send using the
  bootstrap forum catalogue and support vaults with no ordinary forums.
- Preserve direct URLs, reload, history, and built-in deletion rules. Opening
  Welcome creates no session. Use existing snapshots and notice rendering.
  Add no other maintenance UI, events, or polling loops.

### 6. Acceptance tests and documentation

Complete plan step 11 and every acceptance criterion in design section 17.
Add focused tests as each behavior is implemented; do not defer all testing
until this section.

- Native tests cover the four recipes, both protocols, web isolation, stale
  epochs, cancellation/shutdown and lock ordering, runtime refresh, unchanged
  Welcome generation, sanitized logs, and buffer expiry without a browser.
- Check notice ordering and persistence after completion, failure, Stop, and
  later snapshots; notices stay out of subsequent model history. Saved config
  survives restart, while the buffer and undo record do not.
- In desktop component tests, start a Welcome answer, navigate to a form and
  edit it, then finish the answer. Read-only answers and unrelated repairs
  preserve a draft when detail is equal. Changed detail blocks stale Save.
  Include failed-refetch/retry, missed active snapshots, and context changes.
- In `webapp/src/chaweb/App.test.tsx`, render the real App with the existing
  fake client and `userEvent`. Click Welcome, send, observe response/save notice,
  navigate back/forward, and remount at the Welcome URL. Check that no session
  creation occurs and that bootstrap reflects changed names after an answer.
- Cover actual daemon input/snapshot behavior in
  `tests/daemon/unit_chaweb_adapter.cpp` and `itest-daemon`. Add a small scripted
  repair exchange to the existing Python fake provider. Keep Unix-socket nginx;
  no Playwright setup, browser download, TCP fixture, or new test command.
- Update `docs/chaweb.md` once Welcome access works. Keep the design consistent
  with the implementation and mark only verified items in the plan. Preserve the existing manual
  provider Test: saved settings, net mode, 10-second timeouts, no web search,
  and no added output-token cap.

## Verification and completion

Run focused native and frontend suites while developing, then run from the
repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
npm --prefix webapp run check
npm --prefix webapp run build:chaweb
```

On Linux/macOS also run:

```sh
make itest-local
make itest-daemon
```

Use the existing `tsan` preset for touched concurrency paths where supported.
Reuse block 1's completed checks; rerun or extend them when integration changes
those paths. The daemon runner needs nginx, temporary vaults/sockets, and the
local fake provider. No live paid API or R2 service is required. Report any
unavailable host-specific check and its reason; do not mark it as passed.

Completion means the complete feature works on both hosts with all required
protection, notices, refresh, undo, and logging behavior. Resolve implementation
gaps found in either block before declaring completion. Do not leave enabled
tools with TODOs for required lifecycle or frontend handling.

Update this block's status and append a concise completion note with changed
areas, test commands/results, and any unverified environment-specific checks.
Update [ass-plan.md](ass-plan.md) to reflect actual verified progress. If the
session ceiling is reached before completion, record the exact remaining work
and leave the block incomplete; do not silently drop acceptance criteria.

## Completion note

Changed areas:

- Welcome maintenance tools on the application service: list, read, apply,
  undo, logs, and logging. Undo goes through apply. Tool text redacts saved
  keys, available OAuth tokens, and the private workspace root. Log search
  uses the redacted text.
- Maintenance tools and the maintenance reference are attached only to
  Assistant in Welcome. Both provider protocols omit hosted web search for
  those requests. A truncated tool call is not executed.
- The maintenance prompt is the host line plus the whole
  `resources/assistant-maintenance.md`, `docs/MaintainerGuide.md`, and
  `packaging/linux/README.md`. Desktop uses `Host: desktop application`.
  ChaWeb uses `Host: cha-daemon (ChaWeb)`.
- A committed apply or undo stores a Welcome transcript notice. Completion,
  failure, and Stop keep that notice. Ordinary sessions refresh. Welcome
  stays open.
- Desktop and ChaWeb reload bootstrap and open details when a Welcome answer
  ends. ChaWeb shows one Welcome entry for `builtin-welcome`.

Commands and results, from the repository root:

```
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
npm --prefix webapp run check
npm --prefix webapp run build:chaweb
make itest-local
make itest-daemon
```

Results:

- C++: 1068 tests passed. Two live-provider tests were skipped:
  `OpenAiOAuthLive.LoginAndRefresh` and
  `ProviderClientLive.SubscriptionStreamedRequest`.
- Web check: API types, typecheck, and 1061 Vitest tests passed.
- `build:chaweb` passed.
- `make itest-local`: 24 tests passed.
- `make itest-daemon`: 13 tests passed, including
  `test_welcome_repair_notice_survives_failure_and_restart`.

Unverified on this host:

- `cmake --preset tsan` failed. `build/tsan/CMakeCache.txt` has
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`, and
  `build/tsan/_deps/curl-src` is missing, so `CURL::libcurl` was not found.
  ThreadSanitizer was not run and is not marked passed. The ninja build ran
  the logging, provider, and runtime tests.
- No browser session was used. Desktop and ChaWeb screens were verified with
  Vitest and with the daemon HTTP tests.
- `docs/MaintainerGuide.md`, `packaging/linux/README.md`, and
  `resources/assistant-maintenance.md` were embedded as supplied. Their text
  was not rewritten.
- The manual provider Test routine in `src/app/settings_operations.cpp` was
  not changed.

`docs/ass-plan.md` is checked for the items these commands verified.
`docs/chaweb.md` describes the Welcome route. Four items stay open: the three
step 9 writing tasks (guide gaps, the four recipes, and daemon deployment),
and the step 11 item that also asks to rewrite the maintainer guide.
After the suite above, `ChaWebAdapter.WelcomeRepairsAndUndoesThroughChat`
was rebuilt and passed again with the Welcome system-prompt sentence check.

COMPLETED
