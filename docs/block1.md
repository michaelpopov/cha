# Block 1: Configuration and logging foundations

Status: complete. Execute [block 2](block2.md) on this checkout.

Use [assistant.md](assistant.md) as the behavior specification and
[ass-plan.md](ass-plan.md) as the implementation checklist. Read both before
coding, together with the repository instructions. This block groups existing
work; it does not change the design or add requirements.

There are two blocks:

| Block | Plan coverage | Result |
| --- | --- | --- |
| 1 | Steps 1–4 and the logger foundation in step 5 | Tested configuration operations and memory logging, ready for native integration. |
| 2 | Remaining step 5 work and steps 6–11 | Working Assistant maintenance chat in desktop and ChaWeb, with final verification. |

Planning allowance: about 200,000 tokens for this block, including inspection,
implementation, tests, and fixes. Keep total session usage below 500,000 tokens;
reserve the final 50,000 for verification and handoff. These are estimates,
not measured guarantees. Keep work within the stated scope.

The split keeps store and logger changes independently testable. Block 2
connects the complete user flow, so no extra feature flag or temporary UI is
needed between blocks.

## Outcome and boundaries

Implement the shared loader behavior, versioned configuration reads, atomic
apply, one undo record, and the memory log sink. These are native operations;
do not attach maintenance tools to model requests yet. Existing chat, manual
settings, file logging, import, and startup must still work.

Block 2 owns `AssistantService`, request admission and lifecycle integration,
provider tools, prompts, save notices, frontend refresh, and ChaWeb access.
It also owns log filtering/redaction and bounded JSON results at the application
boundary, expiry checks on tool calls, and vault-switch/shutdown integration.
The raw buffer snapshot from this block is an internal API, not a model result.

Read the source in the affected areas before editing. Start with
`src/workspace/workspace.*`, `workspace_config_store.*`,
`workspace_config_editor.h`, `src/util/logging.*`, and their existing tests.
Use `CMakeLists.txt` to register any necessary source or test files.

## Work in this order

### 1. Shared loader compatibility and warning collection

Complete plan step 1 before adding Assistant-specific operations.

- Ignore harmless unknown, unused, and obsolete fields with warnings in the
  shared parsers. Keep validation of required active settings, syntax, and
  references. Do not create a more permissive Assistant-only parser.
- Add the optional per-load `(logical path, message)` collector. Paths use
  configuration row names. The helper records the path explicitly and writes
  the existing warning text unchanged. Pass it through normalizers too.
- Cover web search, session naming, Jev, voice input, and ignored-configuration
  warnings without inferring paths from message text.
- Add regression cases for startup, import/reopen, and manual settings saves
  as specified in the plan. Check that invalid active configuration still
  fails without changing committed data.

### 2. Configuration revisions and snapshot reads

Complete plan step 2 using the existing SQLite store and `TextFiles` map.

- Add one process-local configuration revision. All configuration writers
  advance it when bytes change; reopen advances it, and no-ops preserve it.
  Transcript writes do not affect it. Do not add a database migration.
- Implement consistent list/read snapshots. Read has no version argument;
  it returns the version of the files actually read. Preserve exact source.
- Implement path metadata, explicit missing-file results, lexical prefix
  listing, the 500-entry list limit, and the design's file/result size limits.
- For keys, return metadata only. Protected Assistant paths remain readable
  where specified but never expose raw credentials.

### 3. Atomic apply and native rules

Complete plan step 3. Reuse the store's transaction/publication path and
`Workspace::load`; keep runtime/session behavior out of the store.

- Accept complete-file create/replace batches. Validate canonical supported
  paths, uniqueness, preconditions, size limits, and the supplied revision.
- Protect Assistant files, member overrides, and its selected provider from
  the committed base, including identical writes. Preserve existing entity
  IDs and memberships for proposed edits.
- Compare credential destination/key-reference pairs before and after the
  candidate. Include unresolved and legacy references and the separate search
  provider and Firecrawl destinations. Use the explicit design rules.
- Load the complete candidate from memory and return path-aware warnings only
  for changed files. Preserve unrelated source bytes.
- Allocate result/publication/undo data before the atomic commit. Preserve
  existing pre-commit failure and post-commit restart-required semantics.
- Return commit state, revision, changed paths with old/new byte sizes,
  warnings, and undo availability. Creation has `old_bytes: null`. Compare
  contents rather than lengths. Do not implement a line diff.
- Retain a way for block 2 to check cancellation before commit. Native epoch
  admission and lifecycle locks belong to the application integration there;
  do not move application ownership into the store.

### 4. One undo record

Complete plan step 4 in the same store.

- Retain old bytes and the resulting revision for the latest eligible
  replacement-only batch. Creation clears undo; later configuration changes
  and reopen invalidate it. A no-op preserves it.
- Restore only native saved bytes after the version check, normal candidate
  loading, and atomic commit. Do not repeat protection, credential, or entity
  comparisons for undo. This must allow undo of a repaired provider/style
  that the loader previously omitted.
- Clear the record after success and return the same result shape as apply.
  Add no redo, deletion, persistence, or automatic undo after failed verification.

### 5. Memory logger foundation

Implement the sink and state portion of plan step 5.

- Use one small sink derived from `spdlog::sinks::base_sink<std::mutex>`.
  Hold at most 2,000 entries with native severity, formatted text, and numbers
  assigned under the sink mutex only for accepted messages.
- Add locked snapshot and clear operations. Return entries and latest number
  together; clear preserves numbering. Attach the sink once and never swap
  the logger's sink list while writers run.
- Preserve existing file format, path, rotation, and configured level. Keep
  file and buffer thresholds independent, including file logging off.
- Route `log_debug_payload` exclusively to the file sink. Keep
  `debug_logging_enabled()` tied to file payload logging.
- Keep one monotonic `verbose_until`: info by default, debug for five minutes,
  repeated enable extends expiry, and disable restores info. Enforce expiry
  inside the sink before accepting a message; rejected messages get no number.
- Expose the snapshot, current effective level/expiry, enable/disable, and
  clear operations needed by block 2. Use no timer, scheduler, capture IDs,
  file-log reader, or dependency from `util/` to sessions.

## Verification

Add meaningful cases to the existing suites named in plan steps 1–5. Cover:

- Startup, import, and manual-editor compatibility with harmless fields;
  warning attribution; retained validation of invalid active settings.
- Consistent reads, all-writer revisions, stale writes, identical no-ops,
  multi-file atomicity, failed publication, protected paths, entity rules,
  credential destinations, and exact source preservation.
- Creation/replacement byte-size results and eligible/stale/unavailable undo,
  including restoring a broken provider or style.
- Independent file/buffer levels, payload exclusion, concurrent insertion,
  snapshot/clear, numbering, eviction, and lazy expiry with a controlled clock.
  A log message with CR/LF remains one entry.

Configure and build from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -N
```

Use the discovered test names to run the affected workspace, application,
settings, and logging tests during development. Then run the C++ suite:

```sh
ctest --test-dir build/ninja -j8 --output-on-failure
```

Use the existing `tsan` preset for the touched store/sink concurrency cases
where the host supports it. No frontend test stack, live provider, or paid
service is required for this block. Report unavailable checks accurately.

## Completion and handoff

The block is complete when steps 1–4 and the logger foundation compile and
their required checks pass, while the tools remain unattached. Fix regressions
introduced here before handing work to block 2.

Mark only verified items in [ass-plan.md](ass-plan.md). Leave the application
parts of step 5 unchecked. Update this block's status and append a short
completion note listing the actual APIs/files added, commands and results,
and any environment limitations. Block 2 must be able to continue from these
files without access to this session's chat. Do not claim success for unrun
tests or leave required implementation behind a TODO.

If an unexpected blocker prevents completion within the session ceiling,
leave a precise unfinished-work note and finish this block before starting
block 2; a partial block is not a completed dependency.

## Completion note

Native APIs added:

- `LoadWarning` / `LoadWarningCollector` and optional collector arguments on
  `Workspace::load`.
- `WorkspaceConfigStore::config_revision`, `list_config`, `read_config`,
  `apply_config`, `undo_config`, with `WorkspaceConfigCancelCheck` for block 2.
- Logger buffer APIs: `snapshot_diagnostic_log`, `diagnostic_log_state`,
  `clear_diagnostic_log`, `set_diagnostic_log_verbose`, and
  `set_diagnostic_log_clock_for_test`.

Files changed: `src/workspace/workspace.h`, `src/workspace/workspace.cpp`,
`src/workspace/workspace_config_store.h`,
`src/workspace/workspace_config_store.cpp`, `src/util/logging.h`,
`src/util/logging.cpp`, and tests in `tests/workspace/`, `tests/app/`, and
`tests/util/unit_logging.cpp`.

Commands:

```
cmake --preset ninja
cmake --build --preset ninja --target cha_tests cha_app_tests
ctest --test-dir build/ninja -j8 --output-on-failure
```

Results: 1050 C++ tests passed; 2 live-provider tests skipped
(`OpenAiOAuthLive.LoginAndRefresh`,
`ProviderClientLive.SubscriptionStreamedRequest`).

ThreadSanitizer: `cmake --build --preset tsan --target cha_tests` failed on
this host. CMake could not regenerate `build/tsan` because
`FETCHCONTENT_FULLY_DISCONNECTED` is set and
`build/tsan/_deps/curl-src` is missing. Concurrent insertion, snapshot, and
clear tests passed in the ninja build.

Tools remain unattached. Block 2 should use the store and logger APIs above.

COMPLETED
