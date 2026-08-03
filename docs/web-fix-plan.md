# Web/core boundary correction: implementation plan

Status: ready for implementation, 2026-08-03.

This plan implements [web-fix-design.md](web-fix-design.md). That document is
the source of truth for the intended architecture and semantics. This document
turns the design into a sequence of buildable changes that an LLM can execute
without having to rediscover the design while editing the code.

The work is intentionally split into independent implementation blocks. Each
numbered block should normally be performed in a separate Codex task. A block
must end with a compiling repository and the tests named in that block passing.
Do not start the next block merely to hide a failure in the current one.

One execution-level refinement is made explicit below: where the design says
the web registry owner factory returns `OpenedSession`, the production factory
does exactly that through the `OpenedSession` alternative of a web-local
`RegistryOwnerInput`. The second alternative contains a fake web controller and
exists only to preserve registry lifecycle tests that cannot be represented by
the concrete `SessionController`. It does not change the production ownership
or state model.

## 1. Decisions that must not be revisited during implementation

These decisions are already made by the design. An implementation task should
not reopen them unless code inspection proves that the design contains a factual
error.

1. `SessionController` and `Workspace` are already core. Do not move
   `WebSessionRuntime`, `SessionRegistry`, SSE, HTTP routes, browser lifecycle,
   or owner-thread queue policy into core.
2. TUI and console continue to own and call their controllers directly on one
   thread. They do not acquire a registry, mailbox, SSE sequence, or mandatory
   dedicated controller thread.
3. Introduce core session identity, descriptor, open-result, owning state,
   append, and semantic result types. Reuse these types from all frontends.
4. Keep web protocol DTOs separate from core state. JSON compatibility is a web
   concern, and explicit core-to-web mapping is intentional.
5. Keep the web-local controller seam used by runtime unit tests. Narrow it to
   core values; do not invent a generic core interface solely for the existing
   web fake.
6. Keep `SessionRegistry` in `cha::web`. A generic serialized host and a generic
   registry are deferred until a second non-web consumer has the same ownership
   requirements.
7. Split CMake targets so `cha_core` contains no concrete frontend. Keep the
   existing mixed `cha_tests` binary as an explicit test-only exemption; do not
   restructure all tests merely to prove the production link graph.
8. Preserve the public HTTP/SSE and JSON contract. In particular, JSON continues
   to spell `clear_input` exactly that way.
9. Preserve `SessionLease`, create-only semantics of `create_stored_session()`,
   reconnect behavior, bounded shutdown, mailbox sequencing, and current
   backpressure/coalescing behavior.
10. Use the repository's existing top-level namespace, `cha`, for new core
    types. The design sometimes uses `session::` descriptively; it does not
    require a new nested C++ namespace.

## 2. Intended production dependency graph

After Block 1, the production libraries must have this shape:

```text
cha_core
  util + transcript + agents + session

cha_ui_text
  -> cha_core

cha_ui_render
  -> cha_core

cha_console
  -> cha_core + cha_ui_text + cha_ui_render

cha_tui
  -> cha_core + cha_ui_text + cha_ui_render + curses

cha_web
  -> cha_core + cha_ui_text + httplib + JSON

chacon_app  -> cha_console       # output binary: chacon
cha_tui_app -> cha_tui           # output binary: cha
chaweb_app  -> cha_web           # output binary: chaweb
```

`uv_event_loop` may remain where it is if moving it has no bearing on the
frontend boundary. Do not mix an unrelated utility reorganization into this
work.

The source include rule is equally important:

- `src/util`, `src/transcript`, `src/agents`, and `src/session` must not include
  anything below `src/ui`.
- Concrete frontends must not include one another.
- `ui/text` and `ui/render` are shared presentation support libraries and may be
  used by more than one concrete frontend.

## 3. Execution rules for every block

Before editing a block:

1. Read this complete block and the referenced sections of
   `docs/web-fix-design.md`.
2. Run `git status --short`. Preserve unrelated user changes. Never clean or
   reset the worktree to obtain a convenient baseline.
3. Configure and build the current tree before changing it unless the previous
   task has recorded an already-known failure:

   ```sh
   cmake --preset ninja
   cmake --build --preset ninja
   ctest --test-dir build/ninja --output-on-failure -LE 'web_process|web_stress'
   ```

4. Search before renaming. Enumerate all definitions, call sites, tests,
   designated initializers, and documentation references with the repository
   search wrapper below.
5. Make the smallest coherent change that completes the block. Do not perform
   opportunistic renames or formatting outside the touched architecture.
6. Build after structural edits and again after tests are updated. Run the
   focused tests first, then the ordinary test suite shown above.
7. Re-run the block's static searches. Treat each unexpected result as code to
   inspect, not as proof that a blind global replacement is needed.
8. Record any design deviation in the implementation task's final message. Do
   not silently encode a different architecture.

### Repository search command

`rg` is not installed on every supported development machine, and the `grep`
binary on at least one machine is ugrep rather than GNU grep. Do not use either
program in this plan. At the start of each implementation task, define this
shell function:

```sh
repo_search() {
    git grep --no-index -n -E -- "$@"
    repo_search_status=$?
    if [ "$repo_search_status" -eq 1 ]; then
        return 0
    fi
    return "$repo_search_status"
}
```

All searches below use POSIX extended regular expressions and call this
function as:

```sh
repo_search '<pattern>' -- <path-or-git-pathspec>...
```

`git grep --no-index` searches tracked and untracked working-tree files and does
not invoke the machine's `grep` command. The wrapper converts Git's ordinary
“no matches” status 1 into success, while preserving real search errors. An
empty-gate check therefore succeeds silently; any printed match still requires
inspection. Source-only searches use Git glob pathspecs such as
`':(glob)src/**/*.h'` and `':(glob)src/**/*.cpp'` so deferred README changes do
not make intermediate code gates fail.

For changes to session ownership, append publication, registry shutdown, or
thread coordination, also run the process and stress labels before considering
the block complete:

```sh
ctest --test-dir build/ninja --output-on-failure -L web_process
ctest --test-dir build/ninja --output-on-failure -L web_stress
```

## 4. Block overview

| Block | Main outcome | Depends on | Intended task size |
|---|---|---|---|
| 1 | Honest CMake frontend/core targets | none | small |
| 2 | Shared identity, descriptor, and `OpenedSession` | 1 | medium |
| 3 | Core owning `SessionState` and pure web projection | 2 | medium/large |
| 4 | Core append proof and thin SSE mapping | 3 | medium/large |
| 5 | Core semantic results plus text/TUI/console migration | 4 | large |
| 6 | Web result migration and deletion of transition bridge | 5 | medium/large |
| 7 | Registry/runtime boundary cleanup | 6 | medium |
| 8 | Documentation, complete verification, and residue sweep | 7 | medium |

The blocks are ordered to avoid simultaneously changing the build graph,
session model, incremental publication, input semantics, and shutdown behavior.
Do not combine Blocks 3 through 7 into one unreviewable patch.

## 5. Block 1: split production build targets

### Objective

Make the build graph express the architecture before moving APIs. This block is
CMake-only except for documentation needed to explain a target. Runtime behavior
must not change.

### Required changes

1. In the root `CMakeLists.txt`, restrict `cha_core` sources to:
   - `src/util`
   - `src/transcript`
   - `src/agents`
   - `src/session`
2. Create `cha_ui_text` from the existing `src/ui/text` sources and link it to
   `cha_core`.
3. Create `cha_ui_render` from the existing `src/ui/render` sources and link it
   to `cha_core`.
4. Create or correct `cha_console` so it owns only `src/ui/console` sources and
   links `cha_core`, `cha_ui_text`, and `cha_ui_render`.
5. Ensure `cha_tui` owns only `src/ui/tui` sources and links `cha_core`,
   `cha_ui_text`, `cha_ui_render`, and the curses dependency it already uses.
6. Ensure `cha_web` owns only `src/ui/web` sources and links `cha_core`,
   `cha_ui_text`, httplib, and the JSON dependency it directly uses. Do not rely
   on a concrete frontend to transitively provide another frontend.
7. Link each CMake executable target only to its frontend library:
   - `chacon_app` (output `chacon`) to `cha_console`;
   - `cha_tui_app` (output `cha`) to `cha_tui`;
   - `chaweb_app` (output `chaweb`) to `cha_web`.
8. Update test target links according to the sources actually compiled into
   each target. `cha_tests` may link several production libraries because it is
   the explicitly accepted mixed test binary. Keep the existing dedicated web
   test targets dedicated to `cha_web`.
9. Retain explicit executable dependencies used by process tests. Do not remove
   a dependency merely because it is not a link dependency.

Use normal CMake transitivity for include directories and compile features. Do
not add global include paths to make a missing target dependency disappear.

### Verification

Run:

```sh
cmake --preset ninja
cmake --build --preset ninja
cmake --preset console
cmake --build --preset console
ctest --test-dir build/ninja --output-on-failure -LE 'web_process|web_stress'
```

Inspect the source lists and link declarations manually. The following search
must show no concrete UI sources in the `cha_core` source list:

```sh
repo_search 'cha_core|src/ui/(console|tui|web|text|render)' -- CMakeLists.txt
```

Also verify the source boundary directly:

```sh
repo_search '#include[[:space:]]+[<"]ui/' -- \
  ':(glob)src/util/**/*.h' ':(glob)src/util/**/*.cpp' \
  ':(glob)src/transcript/**/*.h' ':(glob)src/transcript/**/*.cpp' \
  ':(glob)src/agents/**/*.h' ':(glob)src/agents/**/*.cpp' \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
```

The second command should have no results. If it does, stop and determine
whether the current tree already violates the design; do not paper over it with
a CMake dependency in this block.

### Completion conditions

- `cha_core` has no `src/ui` sources.
- All three applications and all tests build in the normal preset.
- The console-only preset builds.
- No runtime or public behavior was intentionally changed.

## 6. Block 2: shared session identity, descriptor, and open result

### Objective

Give every frontend the same core representation of session identity and the
same result from opening or creating a controller. Remove the web-only identity
and metadata-loading pass without moving the web registry into core.

Read the identity and open-result sections of `docs/web-fix-design.md` before
editing.

### New core values

Add small core headers under `src/session` for these values. A suitable layout
is `session_identity.h` for the first two structs and `opened_session.h` for the
owning result, but follow an existing repository naming convention if it is
clearer.

```cpp
struct SessionIdentity {
    std::string forum_id;
    std::string session_id;

    bool operator==(const SessionIdentity&) const = default;
    bool operator<(const SessionIdentity& other) const noexcept;
};

struct SessionDescriptor {
    SessionIdentity identity;
    std::string forum_display_name;
    std::string session_label;

    bool operator==(const SessionDescriptor&) const = default;
};

struct OpenedSession {
    SessionDescriptor descriptor;
    std::unique_ptr<SessionController> controller;
};
```

Implement ordering lexicographically by `(forum_id, session_id)`. Do not make
plain construction validate or authorize filesystem access. Validation remains
inside `Workspace` and `SessionCatalog`.

The descriptor is owning and suitable for presentation and structural log
context, but preserve the current privacy rule: do not start logging the
user-supplied session label merely because it is now readily available.

If the forward declaration of `SessionController` makes `unique_ptr` destruction
ill-formed in a header, either include the complete controller header or define
the move operations/destructor out of line. Do not replace the unique pointer
with shared ownership to avoid the issue.

### Workspace migration

1. Replace `CreatedSession` with `OpenedSession`.
2. Make both of these return `OpenedSession`:

   ```cpp
   Workspace::create_session(...)
   Workspace::open_session(const SessionIdentity&, ...)
   ```

3. Keep `create_stored_session()` create-only and returning its current summary
   value. It must not begin opening a controller.
4. Populate descriptors from data already validated by the workspace/catalog:
   - create: use the newly created forum/session data;
   - open: use the loaded `Forum` for display name and read the session label
     through the same `SessionCatalog` instance already constructed for
     `open_database_path()`.
5. Do not call `Workspace::session_summary()` from `open_session()`, because it
   constructs a second catalog for the same forum. It is acceptable for the
   existing catalog to perform the additional session-label read described by
   the design.
6. Preserve current lease acquisition, rollback, exception safety, and database
   open ordering. Construct the descriptor only from validated values; do not
   allow metadata convenience to move the lease later in the sequence.

### Frontend migration

TUI:

- Consume the returned `OpenedSession` directly for both create and open paths.
- Obtain labels and IDs from `descriptor`, not parallel local strings.
- Keep direct same-thread controller ownership.

Console:

- Replace `ConsoleSelection` if it only duplicates controller plus session ID.
- Return or carry `OpenedSession` through selection and startup.
- Keep the direct same-thread controller loop.

Web:

- Replace `web::SessionKey` throughout registry, routes, snapshots, tests, and
  logging with core `SessionIdentity`.
- Replace `WebSessionMetadata` with core `SessionDescriptor` for runtime/domain
  metadata. Protocol `ForumSummary` remains a web DTO and is populated at the
  projection boundary.
- Replace `RegistryControllerFactory` with the concrete owner-input seam below.
  One invocation must obtain controller and descriptor together. The production
  path calls `Workspace::open_session()` once and selects the `OpenedSession`
  alternative.
- Remove `RegistryMetadataFactory` and its second workspace/catalog pass.
- Change the production runtime entry point to accept `OpenedSession` by value
  and move it onto the permanent owner thread. Keep that aggregate alive for
  the complete owner loop. A narrow production `WebSessionController` adapter
  should borrow its `SessionController`; it must not take ownership out of the
  aggregate merely to recreate the old arrangement.
- Keep the fake-controller entry point used by `unit_web_session_runtime.cpp`
  separate and visibly test-oriented. It may pair a fake web-local port with a
  core `SessionDescriptor`, but it must not be used by production registry
  startup or define another metadata/state model.
- Leave registry path generation and HTTP error mapping for Block 7. This block
  only eliminates duplicated identity/metadata acquisition.

### Concrete registry owner-input seam

Do not make all registry tests construct a real `SessionController`.
`SessionController` is concrete, while the existing tests deliberately fake
blocking shutdown, selected factory throws, and lease-like destruction. Keep
those tests cheap through one web-local variant:

```cpp
// Test/port-backed alternative. Production from_workspace() does not create it.
struct PortBackedSession {
    SessionDescriptor descriptor;
    std::unique_ptr<WebSessionController> controller;
};

using RegistryOwnerInput =
    std::variant<OpenedSession, PortBackedSession>;

using RegistrySessionFactory = std::function<RegistryOwnerInput(
    const SessionIdentity&,
    WakeNotifier&)>;
```

`SessionRegistry::from_workspace()` returns the `OpenedSession` alternative.
`unit_session_registry.cpp` adds a small `fake_session(key, controller)` helper
that creates a deterministic `SessionDescriptor` and the `PortBackedSession`
alternative. The existing 17 factory lambdas then wrap their fake controller
with that helper; their gate, throw-on-attempt, and destruction behavior remains
unchanged. Do not replace those tests with a real workspace or make
`SessionController` virtual.

The factory must run before the runtime can be constructed because the runtime
needs the returned descriptor, but `Workspace::open_session()` needs the wake
notifier. Resolve that ordering explicitly:

1. The owner thread creates a `std::shared_ptr<web::WakeNotifier>`.
2. It invokes `RegistrySessionFactory(identity, *notifier)`.
3. It constructs `WebSessionRuntime` from the resulting descriptor, mailbox,
   hooks, and the same notifier.
4. The runtime retains the shared notifier until after controller shutdown and
   destruction. Commands use that notifier to wake the owner loop.

Do not construct a placeholder descriptor and mutate it after publication. Do
not leave `WakeNotifier` as an embedded runtime value while passing a reference
to it before the runtime exists.

The production runtime lifetime then has this shape:

```cpp
void WebSessionRuntime::run(OpenedSession opened) {
    auto port = adapt_session_controller(*opened.controller); // non-owning
    owner_loop(*port);                                        // opened stays alive
}
```

The exact helper names may differ. The invariant is that the owner thread owns
the `OpenedSession`, the adapter cannot outlive it, and request threads never
obtain the controller pointer. The variant visitor uses the same owner loop for
`PortBackedSession`, but that alternative directly owns its fake port and is
only reachable through the injected registry/test factory.

### Tests

Add or update tests proving:

1. `SessionIdentity` equality and map ordering distinguish both forum and
   session IDs.
2. Workspace create returns the exact created identity, forum display name,
   session label, and a usable controller.
3. Workspace open returns the stored display name and stored session label.
4. Invalid forum/session IDs still fail at the same validation boundary.
5. Lease conflicts and rollback behavior are unchanged.
6. TUI and console startup tests use descriptor data rather than reconstructed
   IDs.
7. A web registry open performs only one combined owner-factory call; there is
   no metadata-factory call to count.
8. Concurrent registry opens still deduplicate to one owner.
9. Production runtime/controller destruction occurs on the owner thread and the
   `OpenedSession` lifetime encloses adapter shutdown.
10. Existing registry fake behaviors still work through `PortBackedSession`,
    including gated shutdown, a selected factory throw, and destruction-based
    lease tracking.
11. Rename the key fields in
    `SessionRegistry.RejectsUrlUnsafeKeysBeforeStartingAnOwner`, but keep that
    test at the registry boundary temporarily. Block 7 moves the unsafe-path
    assertion to `LobbyRoutes` when route validation becomes the stated owner.

### Static residue check

```sh
repo_search \
  'CreatedSession|ConsoleSelection|SessionKey|WebSessionMetadata|RegistryMetadataFactory' -- \
  ':(glob)src/**/*.h' ':(glob)src/**/*.cpp' \
  ':(glob)tests/**/*.h' ':(glob)tests/**/*.cpp'
```

Expected result: no production definition or call site remains. README and
design prose are deliberately excluded here and are updated in Block 8.

### Verification

Build all targets, then run at least:

```sh
./build/ninja/cha_tests
./build/ninja/cha_web_tests
ctest --test-dir build/ninja --output-on-failure -L web_process
ctest --test-dir build/ninja --output-on-failure -L web_stress
```

### Completion conditions

- All three production frontends obtain controllers through `OpenedSession`.
- `PortBackedSession` is confined to injected web tests; production
  `from_workspace()` always supplies the `OpenedSession` alternative.
- Web registry keys use `SessionIdentity`.
- Runtime metadata uses `SessionDescriptor`.
- The web open path has no separate metadata factory/pass.
- TUI/console threading and the HTTP contract are unchanged.

## 7. Block 3: core owning state and pure web projection

### Objective

Move the owning representation of controller state into core while retaining
web-specific protocol DTOs. Make full snapshot production a pure, testable
mapping whose only web inputs are presentation state.

### Add `SessionState`

Add an owning core value under `src/session` with the exact semantic content
required by all frontends:

```cpp
struct SessionState {
    std::vector<CharacterInfo> characters;
    ParticipantId default_agent_id;
    std::vector<TranscriptEntry> transcript;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};
    GenerationStatus generation;

    bool operator==(const SessionState&) const = default;
};
```

Use the existing core owning character, transcript-entry, participant, entry,
request, and generation types. Do not introduce a second set of core enums.
The continuity fields from `TranscriptView` are mandatory even if a UI does not
render them:

- `revision` distinguishes mutations;
- `open_entry_id` identifies the currently streamed answer;
- `history_epoch` distinguishes a cleared history from an append-compatible
  history.

Add an owner-thread-only `SessionController::state()` (or equivalently named)
method that constructs this value from the current controller, transcript, and
generation state. Keep existing borrowed view APIs used by TUI/console where
they are efficient; the new owning value does not require every frontend to
copy its state.

### Define the web projection boundary

Create a focused web projection module, for example
`src/ui/web/session_projection.{h,cpp}`. Its main operation should have this
shape:

```cpp
SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    SessionState&& state,
    const WebPresentationState& presentation);
```

`WebPresentationState` is web-local and contains only fields owned by the web
runtime, such as the current notice, lifecycle state, and shutdown reason. It
must not mirror transcript, characters, generation, revision, or identity.

Mapping remains explicit and handwritten:

- core character values to web character DTOs;
- core transcript kinds/statuses to protocol enums;
- core generation phase and active request to web generation DTOs;
- descriptor identity/display values to the existing web metadata DTO fields;
- web lifecycle/notice/shutdown data from `WebPresentationState`.

Do not merge web DTOs into core. Their separate enums and JSON spellings are an
intentional compatibility boundary.

### Enforce the one-full-copy rule

The controller necessarily creates one owning `SessionState` for the cross-
thread/web publication boundary. The mapper must consume `SessionState&&` and
move transcript text and reasoning strings into the web DTO. It must not copy
the complete transcript a second time.

Make the move visible in code review, for example by moving each owning core
entry into the corresponding web entry. A benchmark is not required, but unit
tests should use non-empty and multi-entry state so accidental field omissions
are visible.

### Runtime and fake migration

1. Change the web-local controller seam's full-state method to return core
   `SessionState`, not `web::SessionSnapshot`.
2. Make the production adapter delegate to `SessionController::state()`.
3. Keep production ownership as `OpenedSession` for the duration of the owner
   loop. Make `WebSessionRuntime` combine `opened.descriptor`, the returned core
   state, and its `WebPresentationState` using the pure mapper.
4. Change runtime fakes to build core `SessionState`. Tests that inspect JSON or
   SSE should continue to inspect the resulting web snapshot.
5. Retain the existing append-candidate mechanism temporarily. The production
   adapter may compare the controller's current state to the last published web
   snapshot until Block 4 replaces that proof. Do not redesign incremental
   publication in this block.
6. Keep the cached runtime value as the resulting web DTO. Do not cache both a
   complete `SessionState` and a complete `SessionSnapshot`.

### Mapping tests

Because explicit mapping omissions still compile, add direct projection tests
with every meaningful field non-default. Cover at least:

- two characters and a non-first default agent;
- every transcript kind and status used by the protocol;
- participant IDs, display names, addressed-to fields, request IDs, timestamps
  or other existing entry metadata;
- non-empty text and reasoning;
- active reasoning and answering phases;
- inactive/terminal generation;
- revision, open entry, and history epoch;
- descriptor IDs and labels;
- notice, lifecycle, and shutdown reason.

Assert fields individually or compare a fully constructed expected DTO. A test
that only serializes and checks for a few substrings is insufficient for this
mapping boundary.

Retain existing protocol serialization tests to prove JSON remains unchanged.

### Verification

```sh
cmake --build --preset ninja --target cha_tests cha_web_tests
./build/ninja/cha_tests
./build/ninja/cha_web_tests
ctest --test-dir build/ninja --output-on-failure -LE 'web_process|web_stress'
```

Inspect the production mapping for moves, and search for web DTO construction in
core:

```sh
repo_search 'SessionSnapshot|WebPresentationState|to_snapshot' -- \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp' \
  ':(glob)src/ui/web/**/*.h' ':(glob)src/ui/web/**/*.cpp'
repo_search '#include[[:space:]]+[<"]ui/web/' -- \
  ':(glob)src/util/**/*.h' ':(glob)src/util/**/*.cpp' \
  ':(glob)src/transcript/**/*.h' ':(glob)src/transcript/**/*.cpp' \
  ':(glob)src/agents/**/*.h' ':(glob)src/agents/**/*.cpp' \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
```

The second search must be empty.

### Completion conditions

- Core owns `SessionState` and constructs it on the controller owner thread.
- The production web owner loop retains one `OpenedSession`; its narrow adapter
  borrows rather than replaces that ownership.
- Full web snapshots are produced only by the pure web projection.
- Runtime test fakes provide core state, not a duplicate domain snapshot.
- Web protocol DTO/JSON compatibility is preserved.
- Only one complete owning transcript copy is made per full publication.

## 8. Block 4: core append proof and thin SSE adaptation

### Objective

Move incremental text-continuity reasoning into core. Keep mailbox storage,
SSE sequence assignment, event names, JSON encoding, and reconnect behavior in
web.

### Core append values

Add the following conceptual values under `src/session`, using the repository's
strong ID types:

```cpp
struct EntryTextTarget {
    EntryId entry_id;
};

struct ReasoningTextTarget {
    RequestId request_id;
};

using SessionTextTarget =
    std::variant<EntryTextTarget, ReasoningTextTarget>;

struct SessionTextAppend {
    SessionTextTarget target;
    std::string text;
};

// Internal projector result; the public append value stays transport-neutral
// and does not carry publication bookkeeping.
struct SessionAppendProjection {
    SessionTextAppend append;
    SessionStateCursor cursor;
};
```

`SessionStateCursor` is a small continuity token, not another state snapshot. It
should own only the identifiers and scalar facts needed to prove a text-only
extension, for example:

- transcript `revision`, `history_epoch`, entry count, and `open_entry_id`;
- default-agent ID;
- active request ID and generation phase;
- the currently appendable entry/reasoning text length.

Small IDs may be copied. Transcript entries, complete text, complete reasoning,
and character arrays must not be stored in the cursor.

Provide one helper that derives a cursor from a freshly built `SessionState`
before that state is moved into the web projection. Provide an owner-thread
controller operation with semantics equivalent to:

```cpp
std::optional<SessionAppendProjection>
SessionController::text_append_since(const SessionStateCursor& cursor) const;
```

It must inspect the controller's current borrowed state and return an append only
when continuity is provable. The returned cursor represents the new current
state and is installed only after web accepts/stores the append.

### Required proof rules

Reasoning append is valid only when all relevant continuity facts match:

- history epoch is unchanged;
- transcript revision/shape did not change;
- the same request remains active;
- generation remains in reasoning phase;
- reasoning text length only grew;
- the returned text is exactly the newly added suffix.

Answer append is valid only when:

- history epoch is unchanged;
- the same open entry and request remain active;
- transcript entry count is unchanged;
- generation remains in answering phase;
- answer text length only grew;
- the transcript revision advanced consistently with one or more answer chunks;
- the returned text is exactly the newly added suffix.

Return `nullopt` for any ambiguity: clear, edit, new/closed entry, request or
phase change, shorter/equal text, default-agent change, malformed controller
state, or cursor mismatch. Falling back to a full snapshot is correct behavior.
Do not attempt a heuristic append merely because the final strings have a common
prefix.

Document the controller invariants that make the compact cursor sufficient. If
inspection reveals an operation that can change non-text entry metadata while
all listed cursor facts remain equal, add the smallest missing continuity fact
to the cursor and a regression test.

### Web adaptation

1. Keep `SessionStateCursor` next to the runtime's cached web snapshot.
2. On full publication:
   - obtain `SessionState`;
   - derive its cursor;
   - move the state through `to_snapshot()`;
   - publish/cache the web snapshot and cursor together.
3. On a delta opportunity, ask the controller seam for a core append projection
   relative to the cached cursor.
4. Map `EntryTextTarget` and `ReasoningTextTarget` to existing web append target
   names/DTOs. Only the nested `SessionTextAppend` crosses that mapping; the
   returned cursor remains local continuity bookkeeping.
5. Let `SseMailbox` assign the SSE sequence only when it stores the event. Core
   must never know an SSE sequence or event name.
6. Advance the cached cursor only if the web publication operation succeeds.
7. Preserve mailbox append coalescing, snapshot replacement, subscriber replay,
   and reconnect behavior exactly.
8. Delete the old adapter-side comparison helpers, including duplicate entry-
   shape and generation-shape logic, once the core proof is active.

The web runtime may still update its cached web DTO with the appended suffix for
subsequent subscribers. That is transport cache maintenance, not a second domain
model.

### Core tests

Add focused tests for:

- one and several reasoning chunks;
- one and several answer chunks;
- exact suffix and target identity;
- cursor advancement after each accepted append;
- clear/history epoch change;
- new request, changed phase, changed open entry, closed entry, default-agent
  change, transcript shape change, equal text, and shorter text;
- multiple controller deltas drained before one append query;
- full-snapshot fallback followed by a valid append from the new cursor.

These tests belong with core session/controller tests and must not mention SSE,
mailboxes, or JSON.

### Web tests

Retain/add tests proving:

- each core target maps to the existing wire target;
- the mailbox assigns sequence numbers, not the controller;
- multiple append chunks still coalesce as before;
- an unprovable append causes a full snapshot;
- a reconnecting subscriber sees a coherent snapshot and later deltas;
- final drain/shutdown still publishes a coherent terminal snapshot.

### Static residue check

After migration, inspect:

```sh
repo_search \
  'same_domain_entry_shape|same_generation_shape|append_candidate|snapshot_revision_' -- \
  ':(glob)src/ui/web/**/*.h' ':(glob)src/ui/web/**/*.cpp' \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
repo_search 'Sse|Mailbox|sequence|event_name' -- \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
```

Old web-side proof helpers should be gone. The second search must not show
transport concepts in core append code; ordinary uses of the word `event` for
session events are not a violation.

### Verification

Run all unit tests plus both web labels because this block changes the most
timing-sensitive publication path:

```sh
./build/ninja/cha_tests
./build/ninja/cha_web_tests
ctest --test-dir build/ninja --output-on-failure -L web_process
ctest --test-dir build/ninja --output-on-failure -L web_stress
```

### Completion conditions

- Only core decides whether a state change is a safe text append.
- Core append values contain no web/SSE details.
- Web retains sequence assignment, mailbox behavior, encoding, and reconnect
  policy.
- Every failed proof falls back to a full snapshot.
- Old duplicate continuity logic is removed from the web adapter.

## 9. Block 5: semantic core results and terminal frontend migration

### Objective

Replace the UI-oriented `SessionUpdate` contract in core with semantic session
results, introduce an explicit text-input result, and migrate `ui/text`, TUI,
and console. Keep web compiling through a temporary web-local bridge that Block
6 must delete.

This block is deliberately atomic for core semantics: do not leave both
`SessionUpdate` and `SessionChange` as competing core APIs.

### Core result type

Add a core value such as `src/session/session_change.h`:

```cpp
struct SessionChange {
    bool state_changed{};
    bool input_consumed{};
    bool controller_ended{};
    std::optional<std::string> notice;
};
```

The fields have strict meanings:

- `state_changed`: controller state visible in `SessionState` changed. It does
  not mean that a particular UI should repaint.
- `input_consumed`: the controller accepted/handled supplied user text or handle
  and the caller may discard that draft. It must not be inferred from
  `state_changed`, notice presence, or command kind.
- `controller_ended`: the controller reached its terminal condition. It is not
  a generic frontend navigation request.
- `notice`: presentation-safe content produced by core. Each frontend owns how
  long it retains or where it prints the notice.

Migrate all `SessionController` operations and session event batches from
`SessionUpdate` to `SessionChange`, then delete the core `SessionUpdate` type.

### Audit every return path

Do not mechanically rename `render_needed` to `state_changed`. Inspect every
operation and set the new fields according to actual behavior.

At minimum preserve these semantics:

| Operation/result | `state_changed` | `input_consumed` |
|---|---:|---:|
| accepted prompt | true | true |
| unknown prompt author | false | false |
| batch staging/dispatch failure before acceptance | false | false |
| clear, information, agents, off-record text operations | varies by mutation | true |
| set default by textual handle, including empty/unresolved handle | false on failure | true |
| typed `set_default_agent_by_id` success | true | false |
| receive/provider progress | according to visible mutation | false |
| typed stop/request-stop | according to visible mutation | false |

Default-agent mutation is an important correction: it changes snapshot-visible
state and therefore must set `state_changed = true`. Existing web publication
may previously have happened incidentally because a notice was set; tests must
now assert the semantic trigger.

Controller command handlers must not decide widget clearing or `/exit`
navigation. Preserve error messages, but ensure unknown authors and rejected
prompts retain the user's draft by reporting `input_consumed = false`.

### Text-input result

Add a shared text-layer result under `src/ui/text`:

```cpp
struct TextInputResult {
    SessionChange session;
    bool clear_input{};
    bool exit_requested{};
};
```

Update the shared parser/dispatcher so:

- ordinary accepted text normally sets `clear_input` from
  `session.input_consumed`;
- rejected/unknown-author text leaves it false;
- an unknown slash command, a command with a disallowed argument, and other
  text-parser errors consume/clear the submitted line even though the
  controller never sees it;
- `/exit` is expressed only as `exit_requested` (and the intended clear-input
  value), not as `controller_ended`;
- notices remain in `SessionChange`.

Keep both `input_consumed` and `clear_input`; they answer different questions.

### TUI migration

- Apply controller/session events from `SessionChange`.
- For submitted text, apply `TextInputResult.clear_input` to the widget and
  `exit_requested` to navigation.
- Retain/display notices using TUI-owned state.
- Decide redraw based on TUI needs plus semantic changes; do not put a repaint
  flag back into core.

### Console migration

- Apply `SessionChange` from controller events and `TextInputResult` from parsed
  input.
- Ignore `clear_input` if the line-oriented console has no retained input widget.
- Print notices with the current output behavior.
- Exit on `exit_requested` or `controller_ended` as appropriate to the existing
  loop, without conflating the two fields.

### Temporary web bridge

Block 5 must leave all web targets compiling, but updating the large runtime
fake/test surface is deferred to Block 6. Create a clearly named, web-local
transition type (for example `WebSessionUpdate`) with only the old runtime-facing
fields needed by current web code. It must live below `src/ui/web`, never in
core.

The production web adapter performs the temporary translations:

- core `state_changed` -> legacy web `render_needed`;
- core `controller_ended` -> legacy web `end_session`;
- raw text `TextInputResult.clear_input` -> legacy web `clear_input`;
- typed operations use `SessionChange.input_consumed` only where their existing
  JSON contract reports consumption; typed default-agent selection continues to
  report `clear_input = false`;
- notice is copied/moved unchanged;
- core event batches are translated to the corresponding temporary web batch.

This bridge is only a compile-safe migration seam. Mark it in code with a short
comment naming Block 6/removal, and do not add new behavior or tests directly
against it beyond conversion correctness.

### Tests

Core/session tests must cover each row of the semantics table plus:

- default-agent success reports visible state change;
- notice-only outcomes need not report state change;
- terminal provider/controller paths report `controller_ended`;
- event batches combine changes without turning input consumption true.

Text tests must cover:

- accepted prompt clears;
- unknown author and undispatchable prompt retain input;
- empty/unresolved default handle follows the documented consumed behavior;
- `/exit`, local commands, and parse errors have explicit clear/exit fields;
- notice propagation.

TUI/console tests must cover draft clearing/navigation/notice behavior at their
own boundary. Existing web tests must at least compile and pass against the
temporary adapter.

### Static residue check

```sh
repo_search 'SessionUpdate|render_needed|end_session' -- \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp' \
  ':(glob)src/agents/**/*.h' ':(glob)src/agents/**/*.cpp' \
  ':(glob)src/transcript/**/*.h' ':(glob)src/transcript/**/*.cpp'
repo_search 'clear_input' -- \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
```

These source-only searches must be empty. The session README still describes the
old names until Block 8. `clear_input` remains valid in `ui/text`, web JSON, and
frontend code. `render_needed`/`end_session` may exist only in the explicitly
temporary web bridge at the end of this block.

### Verification

```sh
cmake --build --preset ninja
./build/ninja/cha_tests
./build/ninja/cha_web_tests
ctest --test-dir build/ninja --output-on-failure -LE 'web_process|web_stress'
```

### Completion conditions

- Core exposes only `SessionChange`, not `SessionUpdate`.
- `ui/text` exposes `TextInputResult`.
- TUI and console use semantic results directly.
- Input-retention behavior is explicitly tested.
- Any old result vocabulary exists only in the temporary web-local bridge.

## 10. Block 6: migrate web result handling and remove the bridge

### Objective

Make the web controller seam, owner loop, command responses, fakes, and tests use
`SessionChange`/`TextInputResult` directly. Delete every transitional type from
Block 5 while preserving wire behavior.

### Interface migration

Change the web-local controller seam so its operations return the closest core
or text-layer result:

- raw text dispatch returns `TextInputResult`;
- typed controller commands return `SessionChange`;
- provider/event draining returns the core session event batch containing
  `SessionChange`;
- full state and append APIs remain the core APIs established in Blocks 3 and 4.

The production adapter should now be a thin delegate plus only unavoidable
typed-route adaptation. Delete `WebSessionUpdate`, any temporary web event batch,
and all conversion helpers created only for Block 5.

### Runtime semantics

Update `WebSessionRuntime` deliberately rather than by field rename:

1. Publish a new snapshot when `state_changed` is true.
2. Also publish when web presentation state changes, such as installing or
   replacing the current notice, even if core state did not change.
3. End the controller loop when `controller_ended` is true.
4. For raw text, return JSON `clear_input` from
   `TextInputResult.clear_input`.
5. For typed routes, report consumption only from the typed operation's defined
   semantics. In particular, typed default-agent selection must not pretend a
   text widget was submitted and should keep `clear_input = false`.
6. Apply `exit_requested` only to the raw text route behavior already defined by
   the web application. Do not turn it into controller termination unless that
   is the existing externally visible behavior.
7. Retain notices in `WebPresentationState`; do not add notice storage to
   `SessionState`.
8. Preserve final snapshot, final drain, runtime shutdown reasons, exception
   containment, and request completion behavior.

The publication predicate should be visible in code, conceptually:

```cpp
if (change.state_changed || presentation_changed) {
    publish_full_snapshot();
}
```

Do not infer state changes from notice presence. Test the two causes separately.

### Test migration

Update web fakes and designated initializers to use the new types. Add/retain
runtime tests proving:

- state change without notice publishes;
- notice change without core state change publishes;
- unchanged state and unchanged presentation do not publish;
- default-agent success publishes even if its notice behavior changes later;
- unknown author/rejected raw input returns `clear_input: false`;
- accepted raw input returns `clear_input: true`;
- typed default-agent command returns `clear_input: false`;
- terminal change completes pending commands and final publication correctly;
- exceptions still become the same web error/shutdown outcomes.

Retain protocol tests that assert the exact JSON field name and value.

### Static residue check

```sh
repo_search 'SessionUpdate|WebSessionUpdate|render_needed|end_session' -- \
  ':(glob)src/**/*.h' ':(glob)src/**/*.cpp' \
  ':(glob)tests/**/*.h' ':(glob)tests/**/*.cpp'
```

Expected result: none in source or test code. READMEs are deliberately excluded
until Block 8. Also inspect every use of `input_consumed`, `clear_input`,
`state_changed`, and `controller_ended`; each should be at the correct layer.

### Verification

Run all unit, process, and stress tests:

```sh
cmake --build --preset ninja
./build/ninja/cha_tests
./build/ninja/cha_web_tests
ctest --test-dir build/ninja --output-on-failure -L web_process
ctest --test-dir build/ninja --output-on-failure -L web_stress
```

### Completion conditions

- The temporary bridge is deleted.
- Web uses core/text semantic results end to end.
- State publication is not notice-dependent.
- JSON and browser input-clearing behavior are unchanged except for the intended
  correction that rejected input is retained.

## 11. Block 7: narrow registry and runtime boundaries

### Objective

Finish the web-host cleanup: keep multi-session ownership in web, remove URL and
HTTP concerns from registry outcomes, and make runtime construction consume the
shared open result in one place.

This block does not extract a generic registry or serialized owner host.

### Registry responsibility

`SessionRegistry` remains responsible for:

- mapping `SessionIdentity` to live web owners;
- deduplicating concurrent opens;
- reattach/lookup;
- capacity accounting;
- starting and joining dedicated web owner threads;
- orphan/disconnect/shutdown coordination required by the web host.

It must not be responsible for:

- constructing `/s/...` URLs;
- choosing HTTP status codes or web protocol `ErrorCode` values;
- separately loading forum/session metadata;
- duplicating core session-state projection.

### Factory boundary

Consolidate production construction so a registry owner factory:

1. receives a validated/core `SessionIdentity` plus only the web ownership
   dependencies it actually needs (for example wake notification);
2. returns `RegistryOwnerInput`; the `from_workspace()` production factory
   always fills its `OpenedSession` alternative and calls
   `Workspace::open_session()` once, while registry tests retain the
   `PortBackedSession` alternative defined in Block 2;
3. keeps the resulting descriptor/controller together through runtime setup;
4. creates the web-local controller port/adapter and `SseMailbox` in the web
   runtime factory;
5. transfers them to the owner thread with explicit single ownership.

Do not reintroduce a metadata factory. Do not place a mailbox, notifier, fake
port, or thread in core `OpenedSession`.

The registry should no longer call `is_url_safe_identifier()`, but not because
that helper is web-specific: it is the shared utility declared in
`util/path_name.h` and is correctly used by `SessionCatalog` and `Workspace`.
The reason to remove this particular call is responsibility: lobby/session
route parsing validates URL components before registry use, and
`Workspace`/`SessionCatalog` remains the authority for stored-session
validation. Do not move or rename the helper as part of this work. Plain
`SessionIdentity` construction is intentionally not validation.

### Transport-neutral registry outcomes

Replace registry success values containing a URL/path with a small internal
result that describes registry state only. A suitable model is:

```cpp
struct RegistryReady {};

enum class RegistryOpenFailure {
    not_found,
    busy,
    stopping,
    limit_reached,
    open_timeout,
    registry_stopping,
    internal_error,
};

using RegistryOpenResult =
    std::variant<RegistryReady, RegistryOpenFailure>;
```

Adjust names to accurately represent existing outcomes; do not collapse errors
that currently map differently at the HTTP boundary. The critical constraint is
that the registry result contains no URL, HTTP status, JSON DTO, or protocol
`ErrorCode`.

In `LobbyRoutes`:

- validate parsed forum/session path components before invoking the registry;
- construct the existing `/s/<forum>/<session>` path from the successful
  `SessionIdentity`;
- map each registry failure to the existing web `ErrorCode`, message, and HTTP
  status;
- preserve redirects and response bodies exactly.

Session command routes continue to map runtime/command failures to web errors at
their existing route boundary.

### Ownership and shutdown audit

While touching construction, trace these paths and add comments only where the
ordering is not evident:

- owner thread starts successfully;
- open throws before the runtime is published;
- two callers race to open the same identity;
- caller times out while startup later completes;
- client disconnects and reattaches;
- orphan timeout requests shutdown;
- global registry shutdown begins during startup;
- final mailbox publication and owner join.

Preserve bounded shutdown and error containment. Do not simplify by detaching a
thread or making a shared controller callable from request threads.

### Tests

Registry unit tests should now assert domain-neutral outcomes, not paths. Route
tests should assert paths and HTTP/web error mappings. Cover:

- ready, already-ready/reattach, starting/busy, stopping;
- not found/open failure;
- capacity and startup timeout;
- registry shutdown during open;
- one factory call for concurrent duplicate opens;
- no separate metadata call;
- unsafe URL components rejected at the route boundary without starting an
  owner;
- `SessionRegistry.RejectsUrlUnsafeKeysBeforeStartingAnOwner` removed or
  rewritten as a `LobbyRoutes` test that proves the same no-owner-start effect;
- exact successful redirect path;
- exact existing HTTP status and JSON error code for every failure.

Run process/stress tests for startup, concurrent open, reconnect, and shutdown.

### Static residue check

```sh
repo_search \
  'RegistryMetadataFactory|WebSessionMetadata|path_for|redirect|ErrorCode|http' -- \
  src/ui/web/session_registry.h src/ui/web/session_registry.cpp
repo_search 'Workspace::open_session|open_session\(' -- \
  ':(glob)src/ui/web/**/*.h' ':(glob)src/ui/web/**/*.cpp'
```

Inspect each result. The registry implementation should contain no route/HTTP
mapping. There should be one production workspace-open path for runtime owners,
not a controller pass plus a metadata pass.

### Verification

```sh
cmake --build --preset ninja
./build/ninja/cha_web_tests
ctest --test-dir build/ninja --output-on-failure -L web_process
ctest --test-dir build/ninja --output-on-failure -L web_stress
```

### Completion conditions

- Registry remains web-local and keyed by `SessionIdentity`.
- Production owner construction consumes one `OpenedSession`.
- Registry outcomes contain no URL or HTTP/protocol error representation.
- Routes alone construct paths and map errors.
- Shutdown, reconnect, capacity, and deduplication behavior remain intact.

## 12. Block 8: documentation and final verification

### Objective

Make the documented architecture match the implemented one, remove transitional
residue, and run the complete verification matrix.

### Documentation updates

Review and update at least:

- `src/README.md`
- `src/session/README.md`
- `src/ui/web/README.md`
- `src/apps/README.md`
- any console/TUI README that describes session startup or result handling
- `docs/web-fix-design.md` status/implementation notes

Document these final facts:

1. Core owns identity, descriptor, `OpenedSession`, `SessionState`, append proof,
   and `SessionChange`.
2. Shared `ui/text` owns `TextInputResult` and text-command clearing/navigation
   policy.
3. TUI and console directly own their controller on one thread.
4. Web uses a dedicated owner thread, queue, registry, mailbox, and SSE policy
   because of its concurrent transport.
5. Web protocol DTOs are projections, not a second domain model.
6. `SessionRegistry` is a web host registry, not a core session abstraction.
7. Production target dependencies match Section 2 of this plan.

Do not describe deferred generic abstractions as implemented. Mark the design
implemented only after all completion checks pass.

### Final source audit

Run and inspect all of these searches:

```sh
repo_search \
  'CreatedSession|ConsoleSelection|SessionKey|WebSessionMetadata|RegistryMetadataFactory' -- \
  ':(glob)src/**/*.h' ':(glob)src/**/*.cpp' \
  ':(glob)tests/**/*.h' ':(glob)tests/**/*.cpp'
repo_search 'SessionUpdate|WebSessionUpdate|render_needed|end_session' -- \
  ':(glob)src/**/*.h' ':(glob)src/**/*.cpp' \
  ':(glob)tests/**/*.h' ':(glob)tests/**/*.cpp'
repo_search \
  'CreatedSession|ConsoleSelection|SessionKey|WebSessionMetadata|RegistryMetadataFactory|SessionUpdate|WebSessionUpdate|render_needed|end_session' -- \
  ':(glob)src/**/*.md'
repo_search '#include[[:space:]]+[<"]ui/' -- \
  ':(glob)src/util/**/*.h' ':(glob)src/util/**/*.cpp' \
  ':(glob)src/transcript/**/*.h' ':(glob)src/transcript/**/*.cpp' \
  ':(glob)src/agents/**/*.h' ':(glob)src/agents/**/*.cpp' \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
repo_search '#include[[:space:]]+[<"]ui/(tui|web)/' -- \
  ':(glob)src/ui/console/**/*.h' ':(glob)src/ui/console/**/*.cpp'
repo_search '#include[[:space:]]+[<"]ui/(console|web)/' -- \
  ':(glob)src/ui/tui/**/*.h' ':(glob)src/ui/tui/**/*.cpp'
repo_search '#include[[:space:]]+[<"]ui/(console|tui)/' -- \
  ':(glob)src/ui/web/**/*.h' ':(glob)src/ui/web/**/*.cpp'
repo_search 'Sse|Mailbox|EventSource|http|JSON|ErrorCode' -- \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
repo_search 'SessionSnapshot' -- \
  ':(glob)src/session/**/*.h' ':(glob)src/session/**/*.cpp'
```

Expected outcomes:

- old identities, metadata factories, and result types are gone;
- source-tree architecture READMEs no longer describe the removed names;
- core does not include UI or transport headers;
- concrete frontends do not include one another;
- core state/append code contains no SSE, HTTP, JSON, or web protocol details;
- core does not construct `SessionSnapshot`.

Also inspect `clear_input`: it should occur in `ui/text`, concrete frontend/UI
code, tests, and web DTO/JSON code, but not in core session results.

### Complete build and test matrix

Run from a configured tree:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure

cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure

cmake --preset console-asan-ubsan
cmake --build --preset console-asan-ubsan
ctest --test-dir build/console-asan-ubsan --output-on-failure

cmake --preset console-tsan
cmake --build --preset console-tsan
ctest --test-dir build/console-tsan --output-on-failure
```

If a sanitizer preset intentionally contains fewer targets/tests, use its
configured set; do not alter the preset merely to claim parity. Record any
environmental sanitizer failure separately from a product test failure and
include the exact command/output in the handoff.

Run the repository integration target if it is not already part of the CTest
matrix:

```sh
make itest
```

Finally inspect the patch:

```sh
git diff --check
git status --short
git diff --stat
git diff -- CMakeLists.txt src tests docs
```

Do not include unrelated user changes in a commit or revert them during this
audit.

### Final completion checklist

- [ ] `cha_core` contains no concrete or shared UI implementation sources.
- [ ] Production frontend libraries have the dependency graph in Section 2.
- [ ] `SessionIdentity`, `SessionDescriptor`, and `OpenedSession` are core and
      used by TUI, console, and web production startup.
- [ ] Workspace open/create populate descriptors without a second web metadata
      factory pass.
- [ ] `SessionState` is the sole owning core state representation.
- [ ] Web DTO mapping is pure, explicit, exhaustive in tests, and consumes state
      by move.
- [ ] Append eligibility is proven by core and contains no SSE concepts.
- [ ] SSE sequencing, coalescing, reconnect, and encoding remain in web.
- [ ] Core exposes `SessionChange`; `ui/text` exposes `TextInputResult`.
- [ ] `SessionUpdate`, `render_needed`, and core `clear_input` policy are gone.
- [ ] TUI, console, and web each apply semantic results at their own boundary.
- [ ] Web registry remains web-local and returns no URL/HTTP value.
- [ ] Lobby routes alone construct session URLs and map registry failures.
- [ ] No generic host/registry or mandatory frontend controller thread was
      introduced.
- [ ] Ordinary, process, stress, integration, and applicable sanitizer tests
      pass.
- [ ] Architecture documentation matches the final code.

## 13. Explicitly deferred work

The following ideas are outside this plan. Do not add them to “complete” the
refactor:

- a generic core `SerializedSessionHost` or actor framework;
- a core/global multi-session registry;
- mandatory controller threads for TUI or console;
- exposing `SseMailbox`, subscriber IDs, sequence numbers, event names, HTTP
  errors, reconnect tokens, orphan timers, or browser lifecycle in core;
- replacing explicit web protocol DTOs with core structs;
- replacing manual protocol mapping with reflection/code generation;
- splitting every test into a perfectly layered test-library graph;
- unrelated event-loop, provider, transcript-storage, or command-language
  redesign;
- changing HTTP endpoints, JSON field names, or SSE event compatibility.

A later extraction of a generic serialized owner or registry should require a
real second consumer with the same queue, lifecycle, capacity, and shutdown
semantics. The current web implementation alone is not sufficient evidence.
