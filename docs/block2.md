# Block 2: Runtime vault switching and persistence

## Task and prerequisites

Implement in-process vault switching in CHA. This is block 2 of four sequential
blocks. This is a self-contained implementation brief; no prior chat or block
file is required. Inspect the repository and verify these block-1 interfaces exist:

- `VaultDefinition` contains canonical `name`, normalized `data`, and optional
  `mirror`/`modify` paths. `ApplicationCommand` contains `config_directory`,
  the immutable sorted `vaults` registry, and its startup `vault`.
- `same_vault_name`/`find_vault` use ASCII case folding, preserving other UTF-8
  bytes. Directory discovery rejects duplicate names, database paths/filenames,
  unsafe modify overlap, and invalid configuration.
- `CurrentVault` provides mutex-protected `get`/`set` copies.
  `ApplicationRuntime::current_vault() const` returns the active definition.
- Startup reads `app.toml` from `--config=CONFIG_DIR`, then the selected vault
  database. Runtime maintenance reads the active definition inside its
  lifecycle lock. Console maintenance requires explicit `--vault`.
- `.env` loads once before runtime construction. One application-wide
  `OpenAiOAuth` uses `config_directory/openai-auth.json`. Neither is loaded
  from a database directory. The native capability getter queries current state.
- Runtime, process, and browser-server fixtures use the configuration directory.

If prerequisites are absent, report the missing earlier implementation rather
than silently implementing several blocks. Preserve equivalent interfaces
already implemented; names below describe the expected code, not a reason to
rename working alternatives.

At completion, `ApplicationRuntime::switch_vault(name)` works and is tested
directly against a running runtime. The HTTP switch route and all new protocol
fields are block 3, so the existing browser remains compatible in this block.
Native setup and packaging are block 4.

## Working rules and fixed behavior

Read applicable `AGENTS.md`/`CLAUDE.md`, inspect `git status`, preserve unrelated
changes, and use the smallest readable implementation. Do not edit `docs/`,
change the configuration format, migrate personal files, or commit databases,
credentials, `.env`, or generated runtime data. Smaller commits are optional.
`docs/vaults-design.md` is the design reference; the relevant contract follows.

There is exactly one active vault per process. Switching is global, in process,
and serialized with every other maintenance operation. Keep the HTTP listener,
port, installed routes, providers, OAuth owner, store, repository, mirror, and
live-session manager alive. Routes already capture these objects. No second
runtime, process restart to switch, background resource replacement, activation
counter, or per-browser selection is introduced.

The runtime registry is fixed for the process lifetime. `app.toml` contains the
required startup `vault` and existing web/logging tables; every other
direct-child `.toml` is a vault definition. A successful switch persists the
target's canonical name to `app.toml`. Formatting/comments need not survive.
Registry validation does not open nonselected databases, so a switch target
can be absent, invalid, or unavailable when selected.

The same process-wide `Providers` supervisor accepts requests whose character
definitions come from the active workspace. Draining sessions cancels their
requests. A slow cancelled provider transport can finish without destroying
the supervisor or delaying the switch through its destructor. OAuth login,
disconnect state, and a pending device-login attempt survive switching.

## Required lifecycle and failure behavior

Hold the runtime lifecycle mutex for the active-vault checks and operation.
Do not check whether the target is already active outside that serialization:
concurrent switches must compare against the selection they actually operate
on. Do not recursively acquire the lifecycle mutex through a helper.

Before draining sessions, resolve the target name, return success if already
active, require `inspect_workspace_session_database(target.data)` to report
`WorkspaceDatabaseState::valid_v2`, and require a configured mirror root to be
an existing directory. Keep this mirror pre-check; removing it would change
the agreed design. Do not use `WorkspaceConfigStore::open` for pre-checks: it
publishes a process-global workspace.

After successful pre-checks, use this sequence:

1. Reserve global live-session maintenance using `settings.shutdown_grace`.
   Close admission and stop all actors with `ShutdownReason::reloading`,
   ending their SSE streams. Use the existing one-deadline drain behavior.
2. Acquire the store maintenance guard, then the repository maintenance guard.
   Keep that lock order, checkpoint, and close the store's SQLite handle.
3. Acquire the target database's process lease before releasing the old lease
   or changing any path. Retarget the store and repository and record the
   target as current. Prepare ordinary validation and allocations before
   committing this change; do not insert a new filesystem validation failure
   between changing the store and changing the repository.
4. Reopen, validate database contents, rematerialize into the same private
   tree, publish through `loadws()`, and synchronize repository forums.
5. Rebuild the stable mirror at the new root, or make it inactive if no root
   is configured. A rebuild failure logs a warning and leaves it inactive.
6. Atomically rewrite only the selected `vault` value in the currently parsed
   `app.toml`. A persistence failure logs a warning; the completed switch still
   succeeds and the next launch uses the previously saved selection.
7. Release guards and the global reservation. Admission resumes only after
   resources, mirror state, and persistence attempt are complete.

| Condition | Required result |
| --- | --- |
| Unknown target | `UnknownVaultError`; no runtime mutation. |
| Already active, including alternate ASCII case | No-op; no session drain or config rewrite. |
| Invalid target database or missing configured mirror root | Ordinary failure; old vault and live sessions untouched. |
| Drain deadline expires | Existing maintenance failure; old vault remains active, admission resumes, unfinished actors retain their normal stopping lifecycle. |
| Target lease busy | Old paths/selection unchanged; reopen old database, preserve stored sessions, and report failure. Live sessions have already been closed. |
| Reopen/validation/publication fails after retarget | `WorkspaceRestartRequiredError`; runtime unusable, later maintenance refused, old saved selection unchanged. Do not claim that the old page is still usable. |
| Mirror rebuild fails after reopening | Switch succeeds, mirror inactive, diagnostic logged. |
| Config rewrite fails | Switch succeeds, old complete config remains, diagnostic logged. |
| Old provider transport finishes late | No provider-owner replacement or unbounded cleanup during switching. |

Stored sessions in the old database are never deleted by draining. Switching
back makes them available again. Every later runtime import/export/upload/
download uses the current vault's paths. Normal config import/export remain
unavailable when `modify` is absent. R2 object naming does not change.

## Implementation work

### Store and repository retargeting

Add retarget support to `WorkspaceConfigStore::MaintenanceGuard` in
`src/workspace/workspace_config_store.h` and `.cpp`. Require a closed guard;
normalize/prepare the path and acquire `SessionLease` before replacing the
old lease/path. On lease failure the store remains pointed at the old database.
Reuse `reopen` validation and rematerialization. The private runtime tree,
`workspace_path()`, and `welcome_path()` remain stable.

Add retarget support to `SessionRepository::MaintenanceGuard` in
`src/session/session_repository.h` and `.cpp`. Use its exclusive
`operation_mutex_`; adjust constness and owned pointers where mutation requires
it. `ApplicationRuntime::Impl` owns a mutable `shared_ptr<SessionRepository>`
and may still pass a const view to routes. The workspace root and process-local
Welcome database remain intact. Reuse guarded `synchronize_forums`; archived
rows are purged at the target database's next normal repository startup, as
before. Validate/prepare the repository target before the store changes paths,
so the multi-object retarget does not fail halfway through an ordinary check.

`getws()`/`loadws()` publish one global workspace. Reusing the private tree is
intentional; never create a second live `WorkspaceConfigStore` for the target.

### Mirror lifecycle and lock constraints

In `src/web/session_mirror.h` and `.cpp`, add an inactive state using an optional
root and `retarget`. Always construct one stable mirror in the runtime,
including for a vault without mirroring. Retarget clears forum/session maps,
sets the new root, and rebuilds directory/path mappings and existing Markdown
projections. Inactive `add`/`update` are no-ops. Preserve the existing path
constructor by delegating if needed by callers/tests.

The existing constructor calls public `repository.list()` and `history()`,
which acquire a shared repository lock. **Do not call that code unchanged while
holding the repository's exclusive maintenance guard.** Provide the mirror
with guard-backed reads or prepared mirror input that does not reacquire that
mutex. Reuse the pattern of `MaintenanceGuard::synchronize_forums`, whose
implementation delegates to unlocked code under an already-owned lock. Keep
such access narrow; do not expose generally unsafe unlocked public methods.

Retarget holds the mirror mutex while replacing its maps. Ensure failure
leaves no partially active maps: the runtime's fallback to an inactive mirror
must be safe and not call back into the repository.

### Maintenance helper and coherent requests

Implement `void ApplicationRuntime::switch_vault(std::string_view name)` and
`UnknownVaultError : public std::runtime_error` in
`src/web/application_runtime.h` and `.cpp`. Share the existing maintenance
drain/reopen/failure mechanics with the transfer operations. Keep orchestration
explicit and local to `Impl`; do not build a generic maintenance framework.

The old `maintain_database` invokes `operation()` without arguments and uses
`auto result = operation()`. Its callers cannot access local guards, and that
form does not support a void-returning switch. Adapt the callback signature
and return handling deliberately, or factor a small private common sequence.
If a post-reopen callback is used, its lock state and exception handling must
be explicit. Mirror/config failures are caught locally; a reopen failure
retains the existing fatal-runtime behavior.

Keep runtime maintenance path reads inside the lifecycle lock, including
`modify` availability checks. A copy read before acquiring the lock could
cause maintenance to pause B but operate on A using an `already_held` lease.
Check all four operations, not just switching.

Audit `src/web/lobby_routes.cpp` against the existing repository/store locks.
Stable object pointers alone do not guarantee coherent compound operations:

- Bootstrap currently fetches a workspace before calling `sessions.recent()`.
  Keep workspace and repository data under one consistent repository access
  scope; block 3 will read the vault name inside that same scope.
- Create/rename routes perform repository work followed by mirror updates.
  Prevent a switch between those steps from mirroring A's result into B.
- Avoid holding a lock and then calling a public repository method that
  acquires it again. Add only the narrowly needed guarded operations, reusing
  the existing repository/store lock domains and consistent lock ordering.

Do not introduce a new client-visible switching gate, runtime lease/epoch
protocol, or require every route to own a replacement runtime. Existing
session-manager admission continues to reject opening during maintenance;
drained session routes report `session_not_live`. Lobby operations can wait
on the existing maintenance locks and run on the current vault. The design
accepts stale-tab intent acting on that vault; it does not accept a compound
operation mixing two vaults. No new cross-tab coordination is required.

### Atomic selection persistence

Extract the existing `read_toml`, temporary sibling-path generator, and
`rewrite_config` behavior from `src/workspace/workspace.cpp` into
`src/util/toml_file.h` and `.cpp`. Suggested interfaces:

```cpp
toml::table read_toml_file(
    const std::filesystem::path& path, std::string_view kind);
template<typename Mutate>
void rewrite_toml_file(const std::filesystem::path& path, Mutate mutate);
```

Use a uniquely named sibling temporary file, flush, close and check the stream,
then rename over the destination. On failure remove the temporary and preserve
the original. The existing helper checks flush but needs its close result
checked to satisfy this contract. Do not truncate `app.toml` in place.

Update the workspace's read/rewrite callers to the shared helper, preserving
their behavior. Add the implementation and tests to `CMakeLists.txt`. The new
caller parses `config_directory / "app.toml"` at rewrite time, assigns the
target's canonical name to `vault`, and preserves other TOML values. Do not
rewrite from a stale startup table or extract unrelated TOML utilities.

## Tests and verification

Start with `make test` and record any existing failures. Use the block-1 runtime
fixtures, extending them to create a configuration directory with vaults `A`
and `B` backed by two imported databases with distinct forum data and unique
filenames. Use `test::import_test_database` and temporary directories. Tests in
this block invoke `runtime->switch_vault(...)` directly; no HTTP route or
`vault_name` response field exists until block 3.

In `tests/web/unit_application_runtime.cpp`, cover:

- A-to-B-to-A switches keep the same HTTP origin, publish the matching forums,
  close old live sessions without deleting stored sessions, allow new sessions,
  and persist the canonical selection. Verify active state with the runtime
  getter and existing bootstrap data.
- Same-vault no-op preserves live sessions and exact config bytes.
- Unknown/missing/corrupt/incompatible targets and a missing configured mirror
  root fail before draining the old session.
- A busy B lease leaves A selected and reopenable after its actors drain. Hold
  leases in another process using `tests/support/lease_test_process.h`, following
  `tests/session/unit_session_lease.cpp`; a same-process lock attempt is not a
  sufficient test.
- Drain timeout retains the old selection/config and safely retained stopping
  actors. Use the existing bounded-lifecycle test facilities; no unbounded
  sleeps or real provider traffic.
- Reopen failure, using `force_next_workspace_config_fault` with `restore`,
  reports restart-required and refuses later transfers. Preserve the existing
  `AFailedReopenIsFatalAndRefusesLaterTransfers` expectations.
- Mirroring switches roots, works when toggling enabled/disabled, and completes
  for a target with stored sessions (exercises the lock-reentrancy hazard).
- Mirror rebuild failure and config-write failure have their specified
  nonfatal outcomes. Use deterministic existing filesystem/fault seams.
- All four runtime maintenance operations use B after switching, with matching
  `data`/`modify`; test a concurrent switch/maintenance interleaving to catch a
  selection captured before the lifecycle lock.
- A concurrent bootstrap/compound lobby operation cannot mix A's workspace or
  session result with B's repository or mirror. Prefer barriers/latches over
  timing-dependent races.
- OAuth status and pending-login ownership survive a switch, and subsequent
  provider requests use B's definitions through the same application-wide
  owner. A cancelled slow provider tail does not block switching on teardown.

Add focused tests in `tests/application/unit_workspace_config_store.cpp`,
`tests/session/unit_session_repository.cpp`, and
`tests/web/unit_session_mirror.cpp` for retarget/reopen, old-lease release,
busy-target failure preserving paths, new-repository reads, inactive mirroring,
and rebuilding maps. Add `tests/util/unit_toml_file.cpp` for successful rewrite,
preservation of other values, unchanged original on mutation/write failure,
and temporary-file cleanup. Use an existing test seam for failures where
available; do not invent a broad fault-injection framework.

Run from the repository root:

```bash
make test
cmake --preset tsan
cmake --build --preset tsan
ctest --test-dir build/tsan --output-on-failure
```

TSan is required for the locking changes where supported. Explicitly report
toolchain/platform limitations; a normal test run is not a substitute for a
TSan result. Run browser checks only if browser/schema files were necessarily
changed; this block should normally leave them alone.

Finish with a handoff describing the switch API, current-vault getter, exact
guarded-access interfaces used by lobby/bootstrap, lifecycle/failure behavior,
and check results. Block 3 must be able to install a thin route calling the
tested operation without finishing retargeting, persistence, or synchronization.
