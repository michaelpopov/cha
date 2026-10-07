# Assistant implementation plan

Status: implementation checklist. Steps 1–4 and the logger sink
foundation in step 5 are marked complete. Remaining step 5 items are
application-boundary log tools.

Implement [the Assistant design](assistant.md) for both the desktop application
and `cha-daemon` through ChaWeb. The design is the source of behavior and limits;
this plan gives the order of work, code locations, and verification for each
step. Add focused tests with the step that introduces the behavior.

Keep the implementation small. Reuse the configuration store, loader, provider
loop, session runtime, journal, and chat rendering. Add no configuration knobs,
plugin framework, database migration, file-log reader, automatic repair loop,
or server-administration API. The only added UI entry is Welcome in ChaWeb.

## Tool contract

Implement exactly these five model tools. All are available only to the
built-in Assistant in Entrance's Welcome session.

| Tool | Inputs | Main result |
| --- | --- | --- |
| `vault_config_list` | `prefix` | Paths, sizes, access policy, configuration version. |
| `vault_config_read` | `paths` | Exact source from one current snapshot and its version; key metadata only. |
| `vault_config_apply` | `action`, `version`, `changes` | Committed result or error; `action` is `apply` or `undo`. |
| `assistant_logs` | `after`, `minimum_level`, `contains`, `limit` | Numbered, sanitized buffer entries and current logging state. |
| `assistant_logging` | `verbose` | Effective level, expiry, and latest entry number. |

Only apply and undo take an input version. Apply validates and saves together.
Undo uses native saved contents with `changes = null`. All maintenance requests
exclude web search, page reading, and provider-hosted search.

Implement steps 1–9 before enabling the complete maintenance conversation.
Step 10 makes that same conversation reachable in ChaWeb. Step 11 verifies the
whole feature and updates documentation to describe implemented behavior.

## 1. Make unknown and obsolete fields harmless across existing load paths

Work in [workspace.cpp](../src/workspace/workspace.cpp),
[workspace.h](../src/workspace/workspace.h), and the shared configuration parsers
they use. This step is independent of Assistant tools and store revisions.

- [x] Where a shared loader rejects harmless unknown, unused, or obsolete
  fields, ignore them and log a warning. Use the same behavior for startup,
  import, and manual editing. Keep existing validation of syntax, required
  values, and references needed by active configuration.
- [x] Pass one optional per-load collector through `Workspace::load` and its
  helpers. Store `(logical path, message)` items with vault-relative paths that
  match configuration rows. Use a small warning helper that takes the path,
  appends to the collector when present, and writes the existing `log_warn`
  message unchanged. Thread it through warning-producing normalizers, including
  xAI voice input delay. Cover existing web search, session-naming, Jev, and
  ignored-configuration warnings as well as new unknown-field warnings.
- [x] Loads without a collector retain ordinary logging. Do not infer paths
  from message text, scrape logs, or add another validation subsystem.

Add regression cases to existing suites before connecting the tools:

- `tests/workspace/unit_workspace.cpp`: harmless fields load with warnings and
  keep known settings unchanged; warning paths are correct, including web search,
  session naming, Jev, and xAI voice input. Preserve existing log messages.
- `tests/workspace/unit_workspace_config_store.cpp`: import a configuration
  with harmless fields, then open/reopen the saved vault successfully. Invalid
  active configuration still fails without replacing committed data.
- `tests/app/unit_application.cpp`: startup opens a vault containing those
  fields and exposes the expected known settings with warnings.
- `tests/app/unit_settings_operations.cpp`: existing manual editors can save
  valid changes when the edited or another loaded file has harmless fields;
  reopening shows the saved values. Invalid active settings still fail without
  changing committed configuration.

## 2. Add configuration revisions and snapshot reads

Work in [workspace_config_store.h](../src/workspace/workspace_config_store.h)
and [workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp).
Use [builtins.h](../src/workspace/builtins.h) for reserved identities.

- [x] Add a process-local revision and typed list/read results. Read SQLite
  configuration rows into the existing `TextFiles` representation under the
  store mutex; return files and their revision from the same snapshot.
- [x] Advance the revision whenever committed configuration rows change,
  including manual settings and credential edits. Advance it on store reopen.
  Cover writers through the common edit path; transcript writes do not count.
  Preserve the revision for a byte-identical edit.
- [x] List paths in lexical order with a literal directory-boundary prefix.
  Return path, byte size, readable/writable policy, and a protection reason.
  Limit a result to 500 entries and report that limit without pagination.
- [x] Read requested paths without a prior listing or input version. Report
  missing paths explicitly. Return source Markdown/TOML, not expanded prompts.
  Read-only Assistant files remain inspectable.
- [x] Return only ID, display name, type, and credential-present status for
  keys. Never return raw key rows, passwords, OAuth tokens, or key values.
- [x] Apply the design's size bounds: 64 KiB per editable file and 256 KiB
  per call's arguments or results. A file read is complete or `too_large`;
  never present truncated content as a file ready for replacement.

Verify in `tests/workspace/unit_workspace_config_store.cpp`: consistent
snapshots during edits, reads after unrelated saves, missing paths, prefix
boundaries, explicit limits, key metadata, revisions for all writers and reopen,
and unchanged revisions after no-ops. Use the existing temporary-vault fixtures.

## 3. Implement atomic apply and native validation

Continue in the store and [workspace.cpp](../src/workspace/workspace.cpp).
Reuse the in-memory candidate-loading and transaction paths already present;
do not export configuration or introduce a second parser.

- [x] Accept a nonempty array of full-file `create`/`replace` changes. Reject
  duplicate paths, unsupported operations, and incorrect create/replace
  preconditions. Validate canonical stored names and the supported roots from
  design section 11; extensions alone do not grant write access.
- [x] Compare the supplied revision under the store mutex before building the
  candidate. On conflict, return `stale_version`; do not merge or retry the
  old proposal. Preserve untouched rows byte for byte.
- [x] Reject writes to `system/assistant/`, every Assistant member directory,
  and Assistant's selected provider, including byte-identical writes. Select
  the protected provider from the committed workspace. Reject credential,
  transcript, host-file, deletion, stable-ID rename, and membership-removal
  operations.
- [x] Load the complete candidate through `Workspace::load` using the supplied
  text map, with no fallback to filesystem content. For proposed edits, verify
  that existing loaded entity IDs and forum memberships remain present.
- [x] Use the warning collector from step 1 for the candidate load. Filter
  warnings by the paths actually changed and return their paths and messages.
- [x] Compare the destination/key-reference pairs described in design section
  4. Include every credential-bearing service, unresolved references, and
  legacy key-name resolution. Treat the search provider's `api_key` and
  Firecrawl's `firecrawl_api_key` as separate destinations. Reject new pairs;
  do not build a dependency graph.
- [x] Before commit, check cancellation and allocate the candidate publication,
  result, and undo data. Commit all changed rows in one SQLite transaction,
  then publish the candidate and advance the revision under the existing
  publication lock. Pre-commit errors change nothing. A post-commit publication
  failure follows the existing restart-required path and remains a saved change.
- [x] Return commit status, version, changed paths with `old_bytes` and
  `new_bytes`, warnings, and undo availability. Use `old_bytes: null` for
  creation; compare full contents to identify changes. Distinguish the errors
  in design section 6 and sanitize error text. A permitted no-op preserves
  revision and undo.

Verify in the workspace tests using existing fault injection: multi-file
atomicity, unchanged comments and prompts, invalid paths/references/templates,
protection before no-op handling, allowed shared settings, and separate-provider
creation for an ordinary character. Cover new
credential destinations, unresolved keys, Brave-to-Tavily changes, and a Brave
key assigned to Firecrawl. Exercise validation, SQLite, and publication failures.
Check path/byte-size results for creation and replacement, including a
same-length edit. Verify that warnings from changed files retain their paths
and warnings from unchanged files stay out of the result.

## 4. Add one native undo record

Keep the record in `WorkspaceConfigStore`; no new table or history service.

- [x] Save old file bytes and the resulting revision for the latest successful
  Assistant batch that only replaces files. Allocate the record before commit
  and install it only after success. A batch creating files clears undo.
- [x] Invalidate undo after another configuration change, store/vault reopen,
  vault switch, or restart. A no-op leaves the record intact.
- [x] For `action = "undo"`, require `changes = null`, the supplied current
  revision, and a native record whose resulting revision matches it. Restore
  only the record's bytes, run normal candidate loading, and commit atomically.
- [x] Do not repeat Assistant-protection, credential-destination, or entity
  comparisons for undo. The version check and native record identify the exact
  earlier committed state. Preserve epoch admission and cancellation checks.
- [x] Advance the revision and clear the record after successful undo. Return
  the same result shape as apply. Add no redo, file deletion, or automatic undo
  after failed verification.

Verify replacement undo, stale/unavailable undo, no undo after creation, and
repair followed by undo of a broken provider or style omitted by the loader.
The last case must restore the exact bytes and warning even when that entity
disappears from the loaded catalogue again. Check the restored paths and byte
sizes in the undo result.

## 5. Add the memory log sink and expiry checks

Work in [logging.cpp](../src/util/logging.cpp) and
[logging.h](../src/util/logging.h). Keep session and application types out of
`util/`; sanitization needing credentials belongs at the application boundary.

- [x] Add a small sink derived from `spdlog::sinks::base_sink<std::mutex>` with
  a deque of at most 2,000 entries. Store each entry's number, native level,
  and text formatted with the existing formatter.
- [x] Assign numbers inside `sink_it_()` under the sink mutex, after expiry
  and level checks. Evict the oldest retained entry when full. Rejected messages
  do not advance the counter. Add locked `snapshot()` and `clear()` methods;
  snapshot entries and latest number together, and preserve numbering on clear.
- [x] Attach the sink during logger initialization and keep the sink list fixed
  while writers run. The buffer must exist when file logging is off. Keep file
  and buffer thresholds independent and the logger threshold permissive enough
  for either sink.
- [x] Route `log_debug_payload` only to the file sink. Keep
  `debug_logging_enabled()` tied to file payload logging. Preserve the existing
  file path, format, rotation, configured level, and behavior when disabled.
- [x] Keep one `verbose_until` in the buffer sink, protected by its mutex and
  based on a monotonic clock. Enable debug for five minutes; repeated enable
  updates expiry; disable restores info. Check expiry on logging and every
  Assistant tool call. Recheck level inside `sink_it_()` so a pre-lock level
  check cannot admit expired debug messages. Add no timer or logging notices.
- [ ] Expose snapshots and effective logging state for the two logging tools.
  Filter by native severity, literal substring, and exclusive `after`; return
  the last requested matching entries in chronological order. Enforce a positive
  limit no greater than capacity and report lost/limited evidence.
- [ ] At the application boundary, redact available saved key values and OAuth
  tokens, remove the private workspace root, and enforce result limits. Return
  one JSON item per log call with ordinary JSON escaping. Do not parse message
  text as additional entries or trusted records.

Verify in `tests/util/unit_logging.cpp`: independent file/buffer behavior,
payload exclusion, concurrent insertion/snapshot/clear, eviction and `after`,
level rejection, stable numbering across clear, and expiry using a controlled
clock rather than a five-minute wait. Test CR/LF and fake records in warning
text. Application tests cover redaction and the absence of logging notices.

## 6. Connect the native service, lifecycle, and runtime effects

Add `src/app/assistant_service.h` and `.cpp`, owned by `Application::Impl`.
Update [application_internal.h](../src/app/application_internal.h), application
construction, and `CMakeLists.txt`. Reuse `workspace_operations.*`,
`settings_operations.*`, `vault_operations.cpp`, and `runtime/live_session*`.

- [ ] Give the service typed operations matching the five tools. Delegate file
  access and transactions to the store and logging to the sink. Translate
  results without placing application/store types in provider protocol code.
- [ ] Capture the epoch from `LiveSessionManager::context_epoch()` when the
  Welcome live session starts, using the epoch admitted for that opening. Pass
  it through the session opener to `SessionController`, then copy it into each
  maintenance `ProviderRequestInput` with the native character/forum/session/
  request IDs. A vault switch replaces live sessions; never refresh an old
  request's epoch in a tool callback. Check identities, epoch, and request
  cancellation on every call; the model cannot supply these values.
- [ ] Inject an application-owned maintenance executor into `Providers` at
  construction, following the existing `WebSearchExecutor` path. Copy it into
  each provider worker and bind the trusted input context and cancellation to
  the request's tool callback. Keep application/store types out of protocol code.
- [ ] Use cancellable `try_lock_for` acquisition of `lifecycle_mutex`, as
  specified in design section 9. Check admission before work and cancellation
  before commit. Release lifecycle/store locks before runtime work or callbacks.
  Never wait for a provider from a runtime thread that the provider needs.
- [ ] Reuse application-owned worker supervision. Callbacks may refer to
  application-owned stores/managers while workers are joined; they must not
  capture `SessionController` or `LiveSession` pointers.
- [ ] Reuse existing settings effects after apply and undo: refresh presentation,
  invalidate affected ordinary conversations, refresh optional services,
  and register new forums. Invalidating all ordinary
  sessions is acceptable when simpler than computing precise dependencies.
- [ ] Explicitly exclude `builtin-entrance` before invalidation. Refresh its
  presentation and let its next request use new inventory/shared values while
  the current Assistant answer keeps its immutable inputs. Do not start global
  vault maintenance for a configuration batch.
- [ ] Carry epoch and committed version through native result delivery and runtime effects.
  Preserve commit order and discard events for an obsolete vault context.
  Report post-commit refresh failures without claiming the save rolled back.
- [ ] Wire a frontend refresh trigger when a Welcome answer ends, on completion,
  failure, or Stop. In desktop `webapp/src/useLiveSession.ts` and
  `components/App.tsx`, reload bootstrap and refetch open entity views/session
  lists with existing read APIs. Apply the same rule in ChaWeb in step 10.
  Refresh even after a read-only answer; do not parse save-notice text.
- [ ] Use existing generation/submission tracking to cover fast answers without
  an observed active snapshot, and avoid repeating refresh on duplicate idle
  snapshots. Refresh on opening/reconnecting Welcome as well. Keep the trigger
  state across screen changes and use existing context/navigation guards for
  late responses; add no bridge event or background polling.
- [ ] Invalidate cached details and reload clean forms in `components/Screens.tsx`
  and `components/Settings.tsx`. Keep the loaded `detail` and draft during the
  refetch and compare fetched values with the form's loaded baseline. Equal
  detail preserves the draft and normal Save eligibility. Only mark a dirty
  form stale if its detail changed; keep the draft visible but disable Save
  until reset/reopen loads fresh data. Keep Save disabled during a refetch or
  after its failure, using existing retry/reopen actions. A successful retry
  with unchanged detail restores normal Save eligibility without a reset.
  Do not merge drafts or add revision arguments to all typed settings writes.
- [ ] On vault switch, drain old workers, reset verbosity, and clear the buffer
  before admitting the new vault. Reset verbosity at shutdown and preserve
  existing undo/revision invalidation during maintenance. Recheck admission
  before returning log data after filtering/redaction.

Verify in `tests/app/`, `tests/runtime/`, and `tests/session/`: stale epoch
admission, cancellation before/after commit, shutdown while a callback waits,
vault switching during tool activity, and worker lifetime. Rename a character
and change a shared prompt while Assistant is answering: ordinary sessions
refresh, Welcome remains active, and later requests see the committed state.
Add frontend tests in `useLiveSession.test.ts` and component tests for the
terminal refresh, updated lists/details, and stale forms, including failed
refetch, missed active snapshots, duplicate snapshots, and navigation.
Start a read-only Welcome answer, switch to a form and edit it, then finish
the answer: unchanged detail must preserve the draft and allow Save under its
normal rules. Repeat after an unrelated repair. Changed detail must block a
dirty form's Save until reset/reopen. A failed refetch keeps the draft, and a
retry returning unchanged detail restores Save without re-entering edits.
Verify the captured epoch reaches `ProviderRequestInput` unchanged: a request
from an old Welcome is rejected after a switch, and a new Welcome uses the
new epoch. Use an injected fake maintenance executor to check the context.

## 7. Store native save notices in Welcome

Use `runtime/live_session_manager.*`, `live_session.*`,
`session/session_controller.*`, [transcript.cpp](../src/chat/transcript.cpp),
and the existing `SessionJournal` path. No new transcript kind is needed.

- [ ] Post committed apply/undo results from the provider callback to the
  session runtime after releasing configuration locks. Do not wait for notice
  insertion or depend on a successful final model response.
- [ ] On the runtime thread, create a short `EntryKind::notice` with
  `make_notice_entry()`, persist it with `SessionJournal::record_entry()`, append
  it to Welcome, and publish the snapshot. Include any post-commit refresh
  error in the summary; keep full result JSON in the current tool exchange.
- [ ] If an answer is active, queue notice text and allocate/store/append entries
  after completion, failure, or cancellation. Flush all three terminal paths
  and preserve the transcript's streaming and entry-ID ordering rules. Append
  immediately if no answer is active.
- [ ] Use stored entries instead of `LiveSession::notice_`. Reuse desktop and
  ChaWeb notice rendering, and the existing model-context exclusion for notice
  entries. Keep raw tool results out of stored conversation history.
- [ ] Create native notices only for committed configuration changes. Logging
  state and other tool errors stay in tool results; Assistant reports them in
  its ordinary answer. Add no logger-to-session dependency.

Verify saved-change visibility after failure, Stop, browser reload, and later
transient notices. Verify multiple notices retain order and later model history
excludes them in `tests/agents/unit_model_context.cpp`. Use existing transcript,
controller, journal, and live-session tests; Welcome keeps its current lifetime.

## 8. Integrate the five tools with both provider protocols

Work in `providers/tool_calls.*`, `providers.*`, `provider_client.*`,
`model_backend.h`, `chat_completions_api.*`, `responses_api.*`, and
`session/session_controller.cpp`.

- [ ] Add strict schemas for the five tools and a small request-owned dispatch
  callback bound to the executor and context from step 6. Reject unknown names,
  extra arguments, wrong types, and malformed calls. Keep app/store dependencies
  behind the callback; no plugin registry.
- [ ] Attach maintenance definitions and the trusted context only for Assistant
  in Welcome. Ordinary characters, multicast targets, and Assistant in a
  user-defined forum receive no maintenance tools. Keep existing Jev routing;
  an explicit `@Assistant` remains its bypass.
- [ ] For maintenance requests, omit `web_search`/`web_read` definitions and
  callbacks and override provider-hosted search to `off` in the request copy.
  Do not mutate the saved provider or shared character definition. Keep this
  restriction through continuations and the final tools-disabled request,
  even when saved search is `required`. Reject unsolicited web calls before
  invoking any executor. Ordinary forum web behavior remains unchanged.
- [ ] Extend existing streaming and non-streaming decoding and continuation
  handling in both protocols. Execute multiple calls sequentially. Retain
  usage aggregation, cancellation, request timeouts, and current handling of
  intermediate tool-round text and provider errors.
- [ ] Replace maintenance calls' existing smaller argument-decoder limit with
  the design's 256 KiB bound, including streaming accumulation. Enforce 512 KiB
  of tool results and 24 Assistant calls per answer; malformed calls count.
  At the call limit, request the final answer with tools disabled. Never execute
  a truncated call. Preserve ordinary web-tool limits for ordinary requests.
- [ ] For a maintenance tool call cut off by the model output limit, surface:
  "The model's output limit cut off the tool call. This call was not applied.
  Edit the configuration file manually." Handle Chat Completions `length`
  and Responses `max_output_tokens`, including incomplete argument JSON, before
  the generic invalid/incomplete-call error. Keep other failures accurate and
  retain save notices for any earlier committed calls in the answer.
- [ ] Check logging expiry on each maintenance call. Return effective logging
  state through the logging tools. Keep raw results confined to the current
  continuation exchange and exclude them from ordinary diagnostic log messages.

Verify in existing provider/protocol tests with fake transports: list/read/apply
continuations, validation errors and correction, undo, logging, sequential calls,
limits, truncation, cancellation, and attempted web calls. Check the actionable
output-limit message and zero executor calls for truncated calls in both
streaming and non-streaming paths. Exercise subscription
Responses using its valid saved settings; add no `max_tokens` override or model
test tool. Confirm global search/Firecrawl edits cannot enable Welcome web tools.

## 9. Assemble the maintenance reference and identify the host

Use [MaintainerGuide.md](MaintainerGuide.md), the
[Linux packaging guide](../packaging/linux/README.md), `resources/`,
[embed_text.cmake](../cmake/embed_text.cmake), and `CMakeLists.txt`.

- [ ] Embed the whole `docs/MaintainerGuide.md` and `packaging/linux/README.md`
  as separate inputs to the existing `embed_text.cmake`. Add a resource with
  brief Assistant-specific operating instructions. Concatenate the embedded
  strings when constructing the maintenance prompt. Do not slice by section
  numbers, add a slicing script, or maintain a second field catalogue.
- [ ] Fill actual guide gaps against the parsers: paths, fields, types/defaults,
  omission behavior, precedence/includes, IDs, services, key metadata, ignored
  fields, and protected Assistant settings. Extend the existing troubleshooting
  map where needed rather than adding a separate topic system.
- [ ] Include the four design recipes and rules for diagnosis versus permission
  to write, fresh reads, preserving unrelated source, stale versions, undo,
  no web tools, temporary logging, untrusted data, and saved versus verified
  results. Explain Jev's explicit-recipient bypass.
- [ ] Describe daemon deployment and manual recovery: nginx/socket/service
  access, `--config`, host configuration versus vault rows, protected-vault
  startup, and unavailable history. A stopped/unreachable daemon requires
  administrator action. Do not suggest desktop Settings/Test controls in ChaWeb.
- [ ] Include the document in full only in maintenance requests; retain
  `resources/application-guide.md` as the general guide. Keep embedded content
  outside the writable namespace and give maintenance-specific restrictions
  precedence over general manual editing/export guidance.
- [ ] Pass host identity from native application construction and prepend exactly
  `Host: desktop application` or `Host: cha-daemon (ChaWeb)`. Update the daemon
  and native host construction paths; do not infer host from OS or vault data.
- [ ] Register embedding inputs/dependencies so guide changes rebuild the prompt
  in both hosts. Update Welcome's introductory prompt text to describe its
  maintenance role without contradicting the embedded instructions.

Verify prompt fixtures for both hosts, whole-file reference inclusion only with
maintenance tools, preservation of the general guide, and parser agreement for
the documented fields. Verify instructions distinguish saved changes from
successful reproduction; do not test exact generated model prose.

## 10. Make Welcome accessible in ChaWeb

Work in `webapp/src/chaweb/route.ts`, `sessions.tsx`, `useChaweb.ts`, `App.tsx`,
and existing tests. Use `daemon/chaweb_adapter.*` only for required changes to
existing snapshot delivery; add no maintenance endpoint.

- [ ] Show one Welcome entry in existing navigation. Open
  `bootstrap.entrance_forum_id` / `builtin-welcome` directly in the existing
  conversation view without creating a session or additional Entrance sessions.
- [ ] Remove route blocks and navigation filters that make Welcome unreachable.
  Validate chat input against the bootstrap forum catalogue rather than the
  ordinary-forum picker, so Send works in Welcome. Allow Welcome even when the
  vault has no ordinary forums. Preserve built-in session deletion rules.
- [ ] Support direct URLs, browser reload, and back/forward navigation. Render
  stored save notices through existing transcript snapshots.
- [ ] In `useChaweb.ts`, reload bootstrap and the session list when a Welcome
  answer ends (completion, failure, or Stop), including fast answers between
  polls. Also refresh on opening/reconnecting Welcome. Reuse the existing
  snapshot/read loop and prevent repeated refresh on unchanged idle snapshots;
  do not add an event type or background polling.
- [ ] Keep all maintenance operations in chat. Add no settings screen, repair,
  undo, logging, approval, or status controls, and no extra notification polling.
- [ ] Update route/component expectations that currently require Welcome to be
  hidden or rejected. Test opening the entry, enabled Send, a vault with only
  Entrance, and no session-creation request when Welcome opens.

Verify `route.test.ts`, `App.test.tsx`, relevant behavior/transcript tests, and
`tests/daemon/unit_chaweb_adapter.cpp`. Native save notices must remain visible
after answer failure and reload within the same daemon's Welcome lifetime.
After a repair, updated character names and forum lists must appear without a
manual reload, including when the final model answer fails.

## 11. Verify the complete feature and finish documentation

- [ ] Extend deterministic fake-provider fixtures to request the maintenance
  tools, inspect their results, and complete or fail the answer. Use existing
  native fake transports for the full tool coverage. For `itest-daemon`, extend
  the text-only fake provider in `tests/integration/daemon_integration_test.py`
  with a small scripted repair exchange. Reuse temporary vaults and the existing
  daemon/socket-activation fixtures and Unix-socket nginx setup; no paid API is
  required.
- [ ] Exercise all four recipes in desktop application tests and through the
  daemon input/snapshot path in `tests/daemon/unit_chaweb_adapter.cpp`: read-only
  diagnosis, authorized repair and undo, verbose reproduction with stop/expiry,
  and an unresolved external failure. In `itest-daemon`, cover Welcome input,
  a repair, and the saved notice in subsequent snapshots, including failure
  after commit. Verify that a fresh bootstrap read reflects the changed catalogue.
- [ ] Extend `webapp/src/chaweb/App.test.tsx` using the real `App`, `userEvent`,
  and the existing fake client. Start at the rendered session list, click
  Welcome, type and send a message, verify the Welcome input request, and
  display the response and save notice from returned snapshots. Test back and
  forward navigation and remount at the Welcome URL with the saved snapshot.
  Opening Welcome must not create a session. These tests run in the existing
  Vitest suite; no browser installation, new server fixture, or test command
  is required.
- [ ] Verify daemon behavior without a connected browser: log writes enforce
  expiry and configuration saves persist after restart, while the log buffer
  and undo record do not. Verify ordinary conversations and manual settings
  continue to work after Assistant edits.
- [ ] Run the relevant focused suites during development, then the repository
  checks below. Use the existing `tsan` preset for the touched sink, provider,
  and runtime concurrency cases. Register new C++ sources/tests in existing
  CMake targets. Report unavailable host-specific checks explicitly.
- [ ] Update [chaweb.md](chaweb.md) when access is implemented, replacing the
  Entrance/Welcome exclusion and describing chat-based maintenance. Update
  maintainer-guide gaps and the existing troubleshooting map from step 9.
  Keep [assistant.md](assistant.md) consistent with the completed behavior and
  mark this checklist only as steps are actually verified.
- [ ] Check every acceptance criterion in design section 17. Preserve the
  existing manual provider Test routine: saved settings for repair verification,
  net mode, 10-second timeouts, no web search, and no added output-token cap.
  Use logs and reproduction for routine ChaWeb verification.

Run from the repository root on a supported development host:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
npm --prefix webapp run check
npm --prefix webapp run build:chaweb
```

On Linux/macOS, also run `make itest-local` and `make itest-daemon`. The daemon
integration runner needs nginx and uses temporary sockets/vaults and a local
fake provider. The rendered ChaWeb tests run as part of
`npm --prefix webapp run check`. The live provider/R2 suite is not required to
verify this feature.

Completion means the same five tools work through desktop Welcome and visible
ChaWeb Welcome, protected configuration stays fixed, writes are atomic, undo
works for eligible repairs, logs are isolated and sanitized, save notices
survive failed answers/reloads, and the final design's acceptance checks pass.
