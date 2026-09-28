# Assistant configuration maintenance: implementation plan

Status: not implemented. The checkboxes below track proposed work, not work
completed while writing this plan.

The revised [Assistant design](assistant.md) is the source of requirements.
This plan follows its narrowed Assistant protection, credential-destination
rule, UI-only Undo, and single embedded configuration reference. If the design
changes, reconcile this plan before implementation.

## 1. Scope and fixed decisions

Implement configuration review and requested repairs directly against the
active vault. Keep the existing SQLite schema, immutable `Workspace` values,
and `WorkspaceConfigStore` transaction path.

Preserve these decisions throughout implementation:

- Expose exactly five model functions: `vault_config_list`,
  `vault_config_read`, `vault_config_check`, `vault_config_apply`, and
  `vault_config_help`. Undo is a native operation used by the UI only.
- Attach tools only to a single-target request for `builtin-assistant` in
  `builtin-entrance`. Do not attach them to multicast or other forums.
- Protect `system/assistant/`, every forum's
  `members/builtin-assistant/`, and Assistant's selected provider definition.
  Shared styles, voices, defaults, and forum prompts remain editable. Do not
  restore the earlier design's comparison of Assistant's effective settings.
- Permit complete file creation and replacement. Do not permit deletion,
  stable-ID renaming, removal of existing entities, or removal of memberships.
  Undo has no deletion exception.
- Reject newly introduced `(destination, key reference)` pairs. Unresolved
  key references must also participate in this comparison.
- Use a process-local revision counter and the existing application epoch.
  Do not add store identities, per-file versions, or a database migration.
- Unknown configuration fields warn in all loading paths. A native warning
  does not prevent an otherwise valid save. Assistant is instructed to fix
  warnings in changed files and check the resulting runtime effects.
- Reuse the maintainer guide's configuration sections. Leave
  `resources/application-guide.md` free of configuration-tool instructions.
- Do not add a shell, MCP service, general tool registry, separate event bus,
  remote service probes, or persistent edit history.

The scope is independent of other proposed features. Do not make this work
depend on a headless interface or change unrelated design documents.

## 2. Delivery order

Each step should leave the application buildable and existing behavior tested.
Add tests with the step that changes the behavior. Keep production request
attachment disabled until the final integration step; no new user-facing
feature flag is needed for this development sequence.

| Step | Deliverable | Depends on |
| --- | --- | --- |
| 1 | Unknown-field tolerance in existing loaders | Existing application |
| 2 | Load diagnostics, configuration revision, and consistent reads | 1 |
| 3 | Batch preview, protected paths, credential destinations, and effects | 2 |
| 4 | Atomic apply and replacement-only native Undo | 3 |
| 5 | Admitted application operations, safe worker callbacks, and runtime effects | 4 |
| 6 | Five function schemas and bounded provider continuation | 2; native dispatch from 5 for integration |
| 7 | Embedded operating instructions and maintainer-guide topics | 1; tool contracts from 6 |
| 8 | Native save results, frontend diff, and Undo control | 5 |
| 9 | Entrance-only attachment and complete workflow verification | 6, 7, 8 |

The individual components in steps 6 and 7 can be tested with injected
callbacks and sample workspaces before production attachment. This does not
authorize publishing an incomplete write workflow.

## 3. Step 1: make unknown fields warnings

Primary files:

- `src/workspace/workspace.cpp`
- `tests/workspace/unit_workspace.cpp`
- `tests/workspace/unit_workspace_config_store.cpp`
- Relevant cases in `tests/app/unit_settings_operations.cpp` and
  `tests/app/unit_application.cpp`

Work:

- [ ] Replace `reject_unknown_fields` with a clearly named warning helper and
  update its callers. Check provider, character, member, persona, style,
  voice, service, and key configuration loaders.
- [ ] Keep recognized-field type checks, required values, ranges, reference
  resolution, and template errors intact. An unknown field is ignored; a
  malformed required field is still an error.
- [ ] Preserve warning behavior in the optional-service loaders that already
  tolerate unknown settings. Avoid duplicate warnings for the same condition.
- [ ] Verify the shared loader behavior through startup, import, and manual
  settings edits. Do not introduce a tool-only permissive mode.

Validation:

- An extra unused field no longer prevents a valid workspace from loading or
  being imported and edited.
- A misspelled optional field warns and leaves its resolved value unchanged.
- An unknown field cannot supply a missing required value.
- A referenced invalid provider still fails; an unused invalid definition
  retains the loader's existing warning/omission behavior.

Done when the change can ship independently of the Assistant tools and the
affected loader, store, and application tests pass.

## 4. Step 2: diagnostics, revision, and reads

Primary files:

- `src/workspace/workspace.h` and `workspace.cpp`
- `src/workspace/workspace_config_store.h` and `.cpp`
- `src/util/text_source.*` and `src/util/toml_file.*` where diagnostics originate
- Existing workspace tests

Use small value types for diagnostics, file metadata, and read results. Keep
them in the workspace layer; add a focused helper file only if the store file
would otherwise become difficult to read.

Work:

- [ ] Add an optional per-load warning collector to `Workspace::load` and pass
  it through the parsing helpers that emit warnings. Calls still write their
  normal logs. Do not scrape logs, replace the global logger, or use a global
  active collector.
- [ ] Retain the loader's message text and first-error behavior. Store the
  originating logical path with each warning so the batch layer can mark
  warnings from changed files without guessing from message text.
- [ ] Normalize diagnostic paths to vault-relative names at the tool boundary.
  Sanitize parser errors before returning them; do not include a raw source
  line from a credential file or an absolute host path.
- [ ] Add a revision counter to `WorkspaceConfigStore::Impl`. Advance it after
  an actual committed row change in the shared `edit()` path and on reopen.
  Audit credential writes, default-character updates, merge, and maintenance
  paths for revision or epoch invalidation.
- [ ] Return the revision as an opaque string. Read revision, rows, and access
  metadata under the store lock so they describe the same committed state.
- [ ] Add list and read methods. List uses a literal directory-boundary prefix,
  lexical ordering, a 500-entry limit, and an explicit limit indicator. There
  is no cursor. Read requires the current version and reports missing paths.
- [ ] Return raw TOML/Markdown source only for allowed readable paths. Supply
  saved-key ID, name, type, and presence metadata through listing, without raw
  credential files or R2 access values.

Validation:

- A concurrent edit cannot produce a read containing one revision and another
  revision's file contents. Old read versions fail.
- Actual form/key/default-character changes advance the revision; transcripts
  and byte-identical edits do not.
- Reopen invalidates old reads. Maintenance invalidates calls through the
  captured application epoch, without adding a second identity mechanism.
- A list reaching 500 entries reports that fact; a narrower prefix works.
- Parallel candidate loads do not mix diagnostic lists. Keys never appear in
  readable source or returned parser diagnostics.

Done when inspection and diagnostics work in native tests without model or
frontend changes.

## 5. Step 3: construct and check a candidate batch

Primary files:

- `src/workspace/workspace_config_store.*`
- `src/workspace/workspace_config_editor.h` where existing editing helpers help
- `src/characters/character_config.*` and provider/service helpers as references
- `tests/workspace/unit_workspace_config_store.cpp`

Add an internal candidate-building function shared by check, apply, and the
eligible parts of Undo. It receives the current workspace, current rows, and
requested changes. It returns the loaded candidate, changed rows, diagnostics,
native diff, and resolved effects. It does not publish anything.

### File and entity rules

- [ ] Accept only `create` and `replace`, with one operation per canonical
  path. Check existence/absence as required by the operation.
- [ ] Reuse `validate_stored_config_name` and add the design's supported-root
  allowlist. Reject external app files, secrets, traversal, unsupported
  extensions, and arbitrary new `system/` areas.
- [ ] Derive the protected provider path from the committed Assistant
  definition. Reject direct and forum-member protected paths, including
  byte-identical writes and creation beneath protected directories.
- [ ] Apply all changes to a copied `TextFiles` map. Leave every untouched row
  byte-identical, and load the complete candidate through the existing loader.
- [ ] Compare existing entity IDs and forum memberships with the candidate.
  Reject implicit removal, including an edit that causes an existing
  definition to be ignored. Permit valid new entities and memberships.
- [ ] Allow an empty check for a review of current configuration. Apply will
  require at least one requested change.

### Credential destinations

- [ ] Add one explicit helper that collects destination/key pairs from model
  providers, Jev, voice input, voice output, and web search.
- [ ] For model providers, use the actual scheme, host, and port selected by
  native request construction. For Jev and voice input, parse those URL
  components using the existing URL machinery. Normalize equivalent host
  spellings and default ports consistently with request construction.
- [ ] Use fixed service names for voice output and web search, with distinct
  names for distinct search services. Reuse their existing endpoint constraints.
- [ ] Resolve a legacy `api_key_env` name to the saved ID when possible, using
  the same precedence as the request path. Retain unresolved IDs and names as
  tagged written references, so an absent key cannot bypass the comparison.
- [ ] Reject any candidate pair absent from the base set with
  `credential_destination_protected`. Do not equate an unresolved name with an
  ID merely because their text matches.
- [ ] Compare configured references even when credentials are currently
  absent. Check disabled service configurations that can later use those
  references; do not make missing credentials an authorization exemption.
- [ ] Do not change manual settings behavior or add an approval override to
  this tool policy. Undo's restoration exception belongs in step 4.

### Diagnostics, diff, and effects

- [ ] Mark each warning with whether its path is in the batch. Keep warnings
  non-blocking in native code.
- [ ] Produce a deterministic text diff from old and new file contents.
  Share this formatter between check and apply; no patch application language
  is required.
- [ ] Compare the small set of resolved values to report before/after effects.
  Include changed character/provider values, presentation, forum/persona
  context, expanded prompts, and effective optional-service state. A text-only
  change can have an empty effect list.
- [ ] Identify all affected forums from resolved values, including changes
  reached through shared includes. Do not create a persistent dependency index.
- [ ] Enforce file/result limits before returning a preview. Return complete
  editable source or an error; never silently truncate it.

Validation:

- Protected Assistant files and its provider fail. Shared styles, voices,
  search settings, and ordinary forum defaults succeed when otherwise valid.
- Copying Assistant's provider to a new ID and assigning another character
  succeeds without changing Assistant's selected provider.
- New key/host, key/scheme, and key/port pairs fail through every applicable
  provider/service path. Existing pairs can be reused or removed.
- Missing future key IDs, unresolved legacy names, ambiguous names, key
  precedence, and equivalent endpoint spellings have explicit cases.
- Unknown-field warnings in changed files are marked. A misspelled optional
  setting produces no intended runtime effect.
- An empty check returns existing warnings, performs no writes, and does not
  refresh sessions. Invalid batches leave both rows and publication untouched.

Done when all native policies are independently tested and check gives an
accurate account of the batch's effect.

## 6. Step 4: atomic apply and native Undo

Primary files: `src/workspace/workspace_config_store.*` and its existing fault
injection tests.

Work:

- [ ] Add apply using the same candidate builder as check. Compare the supplied
  revision under the edit lock; do not merge or retry stale input.
- [ ] Allocate candidate publication, result/diff data, and any Undo contents
  before opening the commit boundary. Verify that a normal success result can
  fit within the caller's remaining result allowance before a write.
- [ ] Use the existing changed-row SQLite transaction and publication lock.
  Advance the revision only with a real committed change. A protected no-op
  remains an error; an ordinary no-op preserves revision and Undo.
- [ ] Return `committed: true`, the new revision, actual changed paths, native
  diff, warnings, effects, and Undo availability. Preserve the distinction
  between no change, rejected change, and committed-but-restart-required.
- [ ] Retain one Undo record only for a successful batch consisting entirely
  of replacements. Store prior contents and the resulting revision, not a full
  vault copy. A successful batch containing creation clears the record.
- [ ] Invalidate Undo on any later actual configuration change and on
  reopen/maintenance. A failed check/apply or a no-op must not invalidate it.
- [ ] Implement native Undo as replacement of the saved old contents at the
  exact recorded revision. Revalidate files, entities, and Assistant
  protection. Skip only the credential-destination comparison, as required
  for restoration of the original pairs. Clear the record after success.
- [ ] Do not expose a model Undo schema or any file deletion in the inverse.

Validation:

- Use existing validation, SQLite begin/write/commit, and publication fault
  seams. Inspect both stored rows and the published workspace after failures.
- A multi-file apply saves everything or nothing. A repeated apply with the
  old revision cannot commit twice.
- Replacement-only Undo restores exact bytes once; creation, stale state,
  maintenance, and restart make it unavailable.
- Undo can restore a credential pair removed by the forward batch without
  permitting a general destination-policy bypass.
- Pre-commit cancellation causes no write. Post-commit failure never claims
  that the database rolled back.

Done when native mutation behavior is complete without a model attached.

## 7. Step 5: application admission and runtime effects

Primary files:

- `src/app/application.*`, `application_internal.h`, and a focused
  `src/app/assistant_operations.*` if needed
- `src/app/workspace_operations.*`, `settings_operations.*`, and resource hooks
- `src/runtime/live_session_manager.*`, `live_session.*`, and session projection
- `src/storage/session_repository.*`
- Application, runtime, and provider-lifetime tests

Work:

- [ ] Add application operations that accept trusted character, forum/session,
  request, captured epoch, cancellation, and deadline values. Model arguments
  contain none of these authority fields.
- [ ] Check Assistant/Entrance/single-target eligibility when binding the
  callback and again at dispatch. Reads must also reject a stale epoch.
- [ ] Acquire the timed `lifecycle_mutex` through short `try_lock_for` attempts,
  checking cancellation, stopping, and the overall deadline between attempts.
  Do not use the typed editors' blocking lock from a provider worker.
- [ ] Preserve lifecycle-then-store lock order. Check admission after acquiring
  the lifecycle lock and before any access to the active store.
- [ ] Reuse the lifetime guarantee of application-owned provider supervision.
  Callbacks may refer to application-owned dependencies; they must not capture
  a raw `SessionController` or `LiveSession` pointer.
- [ ] Apply ordinary conversation invalidation, presentation refresh, service
  refresh, and forum registration from the candidate's resolved effect list.
  Release lifecycle/store locks before session-runtime or UI delivery work.
- [ ] Use the existing non-blocking shutdown/refresh paths for affected live
  sessions. Preserve the active Entrance management turn, including when
  shared styles, voices, or service defaults change. Its next request reads
  new shared settings and inventory.
- [ ] Carry epoch and committed revision in effects and save notices. Deliver
  notices in commit order; reject old-context work before it acts on current
  sessions. Cover concurrent tool/manual commits rather than assuming that
  provider workers finish in commit order.
- [ ] Publish a save notice directly from committed native work. Do not rely
  on an eventual model reply or on a still-active generation event handler.
  Include refresh failures without turning a committed edit into an apparent
  rollback.
- [ ] Propagate every actual configuration publication to the current Undo
  state and management view, including ordinary form and runtime edits.
  Do not call observers while holding the store's edit/publication locks.

Validation:

- A blocked callback sees cancellation while shutdown holds the lifecycle
  lock. Shutdown finishes within its existing grace period.
- Destroying or stopping the originating controller does not leave a callback
  with invalid pointers or allow a pre-commit cancelled write.
- Switching vaults during a call prevents access to the new vault. A late
  notice cannot invalidate a new vault's session with the same public IDs.
- Shared includes update every affected ordinary forum. Presentation-only
  edits avoid unnecessary cancellation, and Entrance can finish its reply.
- Cancelled generation, model failure after save, and ancillary refresh
  failure still produce an accurate native result.
- A vault without an export/modify directory can use the feature. Do not gate
  it on the existing `can_modify` export/import capability.

Done when deterministic application tests can perform the complete native
workflow, including cancellation and maintenance races.

## 8. Step 6: provider functions and bounded continuation

Primary files:

- `src/characters/model_context.h`
- `src/providers/model_backend.h`, `providers.*`, and `provider_client.*`
- `src/providers/tool_calls.*`, `responses_api.*`, and `chat_completions_api.*`
- Existing provider tests and mock HTTP fixtures

Work:

- [ ] Introduce the smallest request-owned description/dispatch structure that
  can carry web search and the five configuration functions. Keep workspace
  types and configuration mutations out of the protocol code, and preserve
  the character layer's dependency direction.
- [ ] Define strict schemas for the exact arguments in design section 6.
  Reject unknown functions, unknown arguments, duplicate paths, and wrong
  types. Configuration validation errors remain structured tool results.
- [ ] Serialize function definitions for both APIs and collect tool calls
  whenever either tool family is attached. Remove the current dependency on a
  nonempty `web_search_tool` callback for all tool handling.
- [ ] Execute calls sequentially. Preserve call IDs, protocol continuation
  items, reasoning continuation where required, and accumulated token usage.
- [ ] Keep web search's four attempts separate from 24 configuration calls.
  Exhausting one family must not prematurely remove the other family.
- [ ] Apply the fixed limits below to encoded arguments/results and stream
  assembly. Check the aggregate budget before dispatching a write and before
  appending its result. Reserve room for a bounded error/final response.
- [ ] Carry one overall deadline across provider rounds and native calls.
  Count malformed/unknown requests toward a bounded turn, and retain existing
  cancellation checks before each call.
- [ ] Classify output-limit termination in both streaming and non-streaming
  protocol paths. Do not execute partial calls. With configuration tools
  attached, use the actionable manual-edit message from the design.
- [ ] Keep info-level logs to outcomes and paths. Preserve existing debug
  payload logging/redaction; do not add a new log store or a blanket ban on
  debug tool payloads.

| Limit | Value |
| --- | --- |
| Listed entries | 500, then request a narrower prefix |
| One editable file | 64 KiB |
| Encoded arguments or result per call | 256 KiB |
| Total configuration results retained in one answer | 512 KiB |
| Configuration calls per answer | 24 |
| Web-search attempts per answer | Existing 4 |
| Native UI diff | 8 KiB, with an explicit indication if shortened |

Validation:

- Run tool/result/final-answer fixtures for both APIs, streaming and ordinary
  responses, with configuration only, search only, and both families.
- Cover several calls in one response, unknown calls, malformed JSON, exact
  byte boundaries, UTF-8/JSON escaping, total-result exhaustion, and cancellation.
- A large or output-truncated batch executes no partial edit. A previously
  committed earlier batch remains reported as saved if a later round fails.
- Request payloads never contain raw saved-key records, and ordinary
  transcripts never receive file text or tool protocol items.

Done when injected native callbacks pass the provider tests. Production
requests still do not receive write tools at this step.

## 9. Step 7: embed the operating guide and reference

Primary files:

- `docs/MaintainerGuide.md`
- New operating/credentials/recipes resources under `resources/`
- `CMakeLists.txt` and `cmake/embed_text.cmake`
- A small help-topic lookup next to the application tool implementation

Work:

- [ ] Prepare maintainer-guide sections 3–10 for embedding. Move filesystem
  and coding-harness workflow sentences to the guide's workflow sections.
  Preserve one authoritative description of configuration fields.
- [ ] Complete missing fields and defaults, especially
  `system/session/config.toml`, current search controls, provider routing,
  unknown-field warnings, and current built-in IDs.
- [ ] Embed the guide through the existing build mechanism. Use an explicit
  topic-to-heading map for the sections/subsections in design section 11.
  Fail a build/test when a required heading is absent or duplicated; no manual
  copied topic files or documentation generator is needed.
- [ ] Add only the new operating instructions, credential-destination
  explanation, and tool recipes. The Assistant topic can combine its guide
  excerpt with the operating guide's protection rules.
- [ ] Return an index and bounded topic text through `vault_config_help`.
  Keep workflow-only export/import/deletion instructions out of topic excerpts.
- [ ] Keep `resources/application-guide.md` unchanged. Add the separate
  operating guide only to the request-owned instructions for eligible
  Entrance requests at attachment time.
- [ ] Explain review versus edit, untrusted retrieved text, warning repair,
  expected effects, protected files, manual credential setup, and UI-only Undo.
  Explicitly require fresh reads and a new check for later-turn acceptance.

Validation:

- Every topic resolves to the intended current heading and fits the tool
  limits. Changing source guide text changes the embedded result on rebuild.
- Reference examples load with the same parser used by the store. A test fails
  when examples use obsolete IDs, nonexistent fields, or wrong defaults.
- Non-Entrance Assistant requests retain their existing guide and receive no
  maintenance instructions. Instructions do not claim runtime effects that
  native check cannot establish.

The maintainer-guide edits are part of the future implementation described
here. Writing this plan does not modify that guide or other existing docs.

Done when help and instructions have one maintained source and pass resource
and example tests.

## 10. Step 8: save results, diff, and UI-only Undo

Primary files:

- `src/runtime/protocol.*`, `session_projection.*`, and `session_output.*`
- `src/bridge/bridge_protocol.*`, `bridge_router.*`, and the relevant dispatcher
- `resources/dto.yaml` and generated `webapp/src/api/schema.d.ts`
- `webapp/src/api/{client,nativeClient,nativeEvents,guards}.ts`
- `webapp/src/useLiveSession.ts`, `components/ChatScreen.tsx`,
  `components/App.tsx`, and `state/entityUpdates.ts`
- Wire fixtures, bridge tests, session-output tests, and corresponding UI tests

Work:

- [ ] Define a transient native edit notice with originating session/request,
  epoch, committed revision, changed paths, bounded diff, warnings, Undo state,
  and post-commit refresh error. This is application state, not a transcript
  entry or database audit row.
- [ ] Prefer carrying notices and current Undo availability in Entrance's
  `SessionSnapshot`. A published snapshot is the save-result event through the
  existing bridge; a second transport or event bus is unnecessary.
- [ ] Retain results of the current/latest editing turn long enough for Stop,
  failure, background completion, and resubscription. Several commits in one
  answer must remain distinguishable; their count is bounded by the tool limit.
  Snapshot coalescing must not discard an acknowledged save's result.
- [ ] Ensure snapshot/append selection falls back to a fresh snapshot when
  notice or Undo state changes. Scope delivery by existing epoch/subscription
  rules, and retain save results independently of generation completion.
- [ ] Add one context-bound native Undo method accepting the expected
  committed version. Follow the existing method policy, request parsing, error
  envelope, and type-generation workflow. Do not add `vault_config_undo`.
- [ ] Show concise save status, an on-demand plain-text diff, and Undo when the
  canonical native state allows it. Do not render diff content as executable
  HTML or instructions. Do not add panel headings or explanatory legends.
- [ ] Refresh entity lists and selected details after a committed edit. Reuse
  existing bootstrap/detail loading and summary helpers; preserve historical
  transcript attribution and unsaved user drafts where possible.
- [ ] Clear or disable Undo after any later configuration revision. If a stale
  control is clicked, the native operation rejects it and the UI refreshes its
  state without pretending the old values were restored.
- [ ] Regenerate API types, update exact bridge protocol/version expectations
  where required, and update method-policy and native event fixtures together.

Validation:

- A saved change and diff remain visible when the following model round fails
  or the user presses Stop. A new vault never receives an old save notice.
- Snapshot coalescing and resubscription preserve the notices and their order.
- A replacement-only edit offers Undo; creation does not. Undo is absent from
  model tool definitions and works only at the recorded revision.
- Manual settings edits refresh the UI's Undo state. No-op/failed edits retain
  eligible Undo. Post-commit refresh errors still say the change was saved.
- Live entity names/settings update without altering stored speaker names or
  unnecessarily clearing current selections.

Done when a scripted native edit produces the complete visible workflow
without a live model.

## 11. Step 9: attach tools and verify the complete workflow

Primary files: `src/session/session_open.*`, `session_controller.*`,
`src/providers/providers.*`, and application/runtime request construction.

Work:

- [ ] Pass the application dispatch capability through the existing session
  creation/request path. Capture epoch and trusted request identity while
  that request belongs to the admitted context, not at later tool execution.
- [ ] Attach the five functions and separate operating guide only to eligible
  Entrance Assistant requests. A renamed ordinary character must not obtain
  them. Preserve existing web search behavior independently.
- [ ] Append instructions to the request-owned input without mutating a shared
  workspace or the Assistant definition. Do not save operating instructions
  back to the vault.
- [ ] Verify complete review, direct repair, proposal/acceptance, conflict,
  protected-file, destination-policy, and Undo workflows with a deterministic
  backend and the real store/application pipeline.
- [ ] Run one native host smoke test with a disposable vault; exercise real
  bridge delivery, diff display, Stop after save, and Undo. Use an optional
  live-provider smoke test only after deterministic coverage passes.

Required complete scenarios:

1. Review a character's search settings. Return findings and diagnostics with
   no configuration change.
2. Fix a character override. Check reports the intended before/after value;
   apply saves it, the UI shows the diff, and Undo restores it.
3. Copy Assistant's provider and assign another character to the copy. The
   existing destination/key pair is reused; Assistant's provider is unchanged.
   The creation batch has no Undo.
4. Attempt direct Assistant editing, membership editing, or a new credential
   destination, including following hostile instructions in a prompt/search
   result. Native policies reject each prohibited action.
5. Edit a shared style, voice, service default, or forum prompt. It succeeds
   when valid; affected ordinary sessions refresh while Entrance completes.
6. Propose a change, let the user accept in a later turn, then reread/check/apply
   from current state. Report the actual new diff, not the earlier proposal.
7. Make a manual edit after a tool read. The old batch fails as stale; a fresh
   read enables a new valid batch.
8. Stop or fail the model after a successful save. The native result remains
   visible and shows the correct Undo state.
9. Switch vaults or shut down during lock acquisition/validation. No stale
   access, late pre-commit write, deadlock, or use of destroyed controllers occurs.

Done when every acceptance requirement in design section 13 has a test or a
recorded native smoke-test result, and ordinary chat/search regression tests pass.

## 12. Validation commands and completion criteria

Build and run the affected suites after each implementation step. Existing
test executables are `cha_tests`, `cha_app_tests`, `cha_bridge_tests`, and
`cha_native_runtime_tests`; use their GoogleTest filters or CTest name filters
for focused checks.

```sh
make build
ctest --test-dir build/ninja --output-on-failure -R 'Workspace|ProviderClient|Responses|ChatCompletions|Providers|Application|LiveSession|SessionOutput|Bridge|NativeRuntime'
```

After changing wire types or UI code:

```sh
npm --prefix webapp run api-types
make web-check
make web-stage
```

At final integration, run `make test` and the relevant native host harness
under `tests/native/macos/` or `tests/native/windows/`. Check both supported
hosts before release; record a platform limitation if only one is available.
Run focused sanitizer checks when lifetime or race tests reveal unresolved
concerns, rather than repeating the whole suite after documentation-only edits.

The feature is ready when:

- Native path, entity, credential-destination, epoch, and transaction rules
  hold even when the model ignores its instructions.
- Check and apply report accurate source diffs, warnings, and runtime effects.
- Saves and Undo remain truthful through cancellation, concurrency, model
  failure, and post-commit refresh failures.
- The reference shipped in the application matches the loader and has no
  independently maintained duplicate configuration manual.
- Existing manual settings, import/export, ordinary conversations, web search,
  and vault switching continue to pass their relevant tests.
- Only intended source, generated contract, test, and reference changes are
  included. No schema migration or unrelated feature is required.
