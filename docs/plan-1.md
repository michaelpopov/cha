# Plan 1: Implement the workspace/discovery replacement

Status: implementation plan for [Part 1](part-1.md).

This plan covers C++ code and the C++ tests and documentation affected by that
code. The intended result is the design from Part 1:

- one immutable `WorkspaceModel` for static workspace data;
- one `SessionRepository` for mutable session storage;
- one free `open_session()` operation for controller construction;
- no `Workspace`, discovery wrappers, or separate Welcome-storage object.

The work is ordered so that the old production path remains intact until all
three replacements exist. The web routes and registry then switch together.
There is no period in which production discovery uses the new model while
production opening still reloads the old workspace.

## Implementation rules

Keep the change proportional to this application:

- Do not add a `SessionFactory`, repository interface, storage variant, PIMPL,
  loader hierarchy, service container, or compatibility facade.
- Do not add live reload, file watching, background indexing, session-list
  caching, startup benchmarks, or memory accounting.
- Keep `SessionCatalog`, `SessionLease`, `SessionController`, and the generic
  `SessionRegistry` callback contract intact.
- Do not change browser assets or client-side code; the web protocol remains
  unchanged.
- Preserve the current JSON shapes, HTTP statuses, catalog ordering, lease
  order, registry error mapping, and shutdown order.
- Keep provider-bearing `AgentDefinition` values private to the model. The
  only production caller allowed to copy them is `open_session()`.
- Prefer private helpers in the new `.cpp` files over new public helper types.
- Reuse `TestWorkspace`; add only small path helpers if repeated path spelling
  becomes distracting. Do not turn it into an application graph builder.
- Keep each phase buildable and run the C++ suite at every checkpoint.

## Final production files

Add these files to `cha_core`:

| File | Purpose |
| --- | --- |
| `src/application/workspace_model.h/.cpp` | Load and own all static workspace state. |
| `src/session/session_repository.h/.cpp` | Own persistent catalog access and the temporary Welcome database. |
| `src/application/session_open.h/.cpp` | Construct one controller from the model and prepared storage. |

The final implementation should not require any other production abstraction.
Small translation-unit-local structs and functions are expected.

## Phase 0: Establish the baseline

Before editing production code:

1. Configure, build, and run the current C++ suite:

   ```sh
   cmake --preset ninja
   cmake --build --preset ninja
   ctest --test-dir build/ninja --output-on-failure
   ```

2. Record any pre-existing failure rather than changing unrelated code as part
   of this redesign.
3. Use searches for the old headers and `SessionRegistry::from_workspace()` as
   the removal checklist. Do not use a search for the word `Workspace` alone;
   it also appears legitimately in user-facing text.

## Phase 1: Add `WorkspaceModel`

This phase adds the new static model without changing production routes or
session opening.

### 1. Add the construction value needed by the model

Create `src/session/session_repository.h` initially with the declaration of
`ForumSessionDirectory`:

- `forum_id` is the stable forum ID;
- `directory` is that forum's `sessions/` directory.

`WorkspaceModel` may include this header and return a vector of these values.
The header must not include `workspace_model.h`. The repository class and its
other values are added in Phase 2.

This small ordering detail keeps the final dependency direction from the first
build: application code may use a session-layer construction value, while
session code never depends on the application model.

### 2. Move workspace configuration to the model

In `src/application/workspace_model.h`:

- define `WorkspaceConfig`;
- declare `load_workspace_config()`;
- declare `ForumInfo` and `WorkspaceModel` with the public API from Part 1.

Remove the configuration definition and loader declaration from
`src/session/workspace.h`. While the legacy `Workspace` still exists, let that
header include `application/workspace_model.h` and continue accepting the same
`WorkspaceConfig` value. This is a temporary compile bridge, not a second API;
it disappears when `workspace.h` is deleted.

Move the implementation of `load_workspace_config()` to
`workspace_model.cpp`. Preserve its existing validation messages and relative
log-file resolution.

Avoid parsing `workspace.toml` twice. In `src/agents/config.h/.cpp`, extract the
existing provider-table parsing into one overload/helper that accepts an
already parsed TOML table plus its source path. The existing path-taking
`load_provider_config()` should parse and delegate, while
`load_workspace_config()` should parse once and call the table-taking form.
Do not introduce a general configuration parser object.

### 3. Implement the model as ordinary owned values

Use direct data members rather than a PIMPL. The private state should contain
only what the process needs after startup:

- the `WorkspaceConfig`;
- a `SharedPersonaRoster` containing Guest and custom personas;
- sorted character and forum vectors;
- stable-ID indexes into those vectors;
- cached character Markdown, including Assistant's application guide;
- pre-expanded `AgentDefinition` vectors keyed by forum ID;
- Assistant's inventory text;
- persistent `ForumSessionDirectory` values for custom forums.

Use a private, path-bearing local value such as `LoadedForum` while loading a
custom forum. It can hold `ForumInfo` plus the forum directory long enough to
load prompts and derive the sessions path. Do not put a path back into the
public `ForumInfo` merely to simplify loading.

The only definition accessor is a private
`copy_definitions_for(forum_id)`. Declare the free `open_session()` operation
as its friend. Do not add a public `make_agent_definitions()` or a public view
of definitions: full definitions may contain credentials.

### 4. Perform one all-or-nothing load

Implement `WorkspaceModel::load()` in this order:

1. Validate the workspace's `personas/`, `characters/`, and `forums/` layout
   using the current rules.
2. Load custom personas and character metadata. Read and retain every custom
   `CHARACTER.md` value.
3. Load custom forum metadata and membership into private path-bearing values.
4. Apply the existing ID, reserved-built-in, public-name collision,
   membership, and default-character validation.
5. Build the effective persona roster with Guest first and custom personas
   ordered by folded display name.
6. Build the inventory string from custom personas, characters, and forums
   using the current ordered JSON format. Guest, Assistant, and Entrance do not
   enter this inventory because they do not today.
7. For every custom forum, call the existing `load_agent_definitions()` with
   its defaults and member overrides, then run the existing
   `ForumCharacters` validation. Store the resulting definitions.
8. If a forum definition or prompt fails, add the forum ID and relevant path
   to the thrown startup error. Do not defer the failure until session create
   or open.
9. Add Assistant metadata, cache `application_guide()` as its Markdown, and
   build Assistant's definitions from the provider, inventory, and effective
   persona roster.
10. Add Entrance as a path-free `ForumInfo` whose only member and default are
    Assistant. Store its Assistant definitions under the Entrance ID.
11. Sort the published character and forum vectors by folded display name and
    build their stable-ID indexes. Preserve forum member ID order; route
    projection will continue sorting members by display name.
12. Publish the fully built model. No method performs later filesystem reads.

The directory-scanning and TOML glue currently private to `workspace.cpp` may
be copied into `workspace_model.cpp` during this migration. Keep it private and
delete the legacy copy with `Workspace` in the final phase. Do not create a
permanent shared loader layer solely to avoid this short-lived duplication.

### 5. Add focused model tests

Add `tests/application/unit_workspace_model.cpp` and its CMake entry. At this
stage, cover the new contract without yet copying every legacy test:

- a normal fixture loads custom data plus Guest, Assistant, and Entrance;
- Guest and all public catalogs have the specified order;
- Assistant detail equals the embedded application guide;
- forum membership/defaults match current behavior, and a fixture containing
  descriptions and tags can build Assistant successfully;
- a broken prompt in any configured forum makes model loading fail, even when
  that forum has not been opened;
- editing TOML or Markdown after loading does not change public model results;
- loading a second model sees the edit.

Do not add the controller-opening immutability regression yet. It depends on
the repository and `open_session()` introduced in the next phases.

### Phase 1 checkpoint

Build and run the full C++ suite. Production still constructs `Workspace`,
`WebDiscovery`, and `WelcomeStorage`; this is intentional.

## Phase 2: Add `SessionRepository`

Expand `src/session/session_repository.h` with the concrete values and class
from Part 1: `TemporarySessionSeed`, `SessionEntry`, `PreparedSession`, and
`SessionRepository`.

The header should include only session-layer and standard-library types. In
particular, it must not include `workspace_model.h` or `builtins.h`.

### 1. Store only paths and temporary-session ownership

The repository needs these members:

- an immutable forum-ID-to-sessions-directory map;
- the temporary session's identity and label;
- the private temporary directory and database path it owns.

Move the existing secure temporary-directory creation, database creation, and
best-effort destructor cleanup from `WelcomeStorage` into the repository. Use
generic temporary-session wording in this session-layer code. The application
composition root supplies the Entrance/Welcome IDs and label.

Do not keep a `WelcomeStorage` member and do not introduce a storage-kind
variant. A private helper that returns an operation-local `SessionCatalog` for
either the persistent forum or the temporary forum is sufficient.

### 2. Implement the four operations

Implement the public methods with these exact semantics:

- `list(forum_id)` uses `SessionCatalog::list()`, converts each row to a
  `SessionEntry`, and reads one `last_write_time` for that row. The temporary
  forum returns its one row through the same result shape. Keep ordering by
  session ID. Keep invalid databases visible with their fallback label and
  non-empty `error`.
- `validate(identity)` uses the catalog's strict single-session validation.
  Missing forums/sessions retain `ForumNotFoundError` and
  `SessionNotFoundError`; malformed metadata continues to throw a storage
  error. It must not turn a tolerant list error into a successful open.
- `create(forum_id, label)` delegates only for persistent forums and converts
  the result to `SessionEntry`, including its timestamp. Unknown forums and
  the temporary forum throw `ForumNotFoundError`, which preserves the current
  404 for creating a session in Entrance.
- `prepare(identity)` first calls `open_database_path()`, then acquires the
  `SessionLease`, then calls `session()` again for the label, and finally calls
  `load_session_state()` while the lease is held. Return all four owned values
  in `PreparedSession`.

Do not optimize away the second metadata check in `prepare()`. This phase is
about ownership, not changing a safety-sensitive storage sequence. Likewise,
do not cache listings or timestamps; recent-session enumeration remains small
and dynamic.

All public operations are `const`. Construct a fresh `SessionCatalog` inside
each call and rely on `CatalogLease`, `SessionLease`, and the registry's
per-identity serialization. Do not add a repository-wide mutex.

### 3. Add repository tests

Add `tests/session/unit_session_repository.cpp` and its CMake entry. Cover:

- persistent create/list/validate/prepare;
- empty labels and current session-ID ordering;
- timestamps returned with rows;
- invalid metadata visible in `list()` but rejected by `validate()` and
  `prepare()`;
- unknown forum/session exceptions;
- the single temporary row, strict identity handling, and create rejection;
- restoration while leased and lease contention;
- release of a lease when a prepared value is destroyed;
- removal of the owned temporary directory on repository destruction;
- the existing concurrent catalog/open scenarios after they are rewritten to
  use the repository; do not add a separate stress matrix.

Adapt the existing Welcome-storage expectations rather than inventing new
temporary-storage features.

### Phase 2 checkpoint

Build and run the full C++ suite. The old production storage path remains in
use until the coordinated web cutover.

## Phase 3: Add the controller-opening operation

Create `src/application/session_open.h/.cpp` and add them to `cha_core`.
Declare and implement the free `open_session()` signature from Part 1.

The implementation is intentionally linear:

1. Look up `ForumInfo` by stable forum ID and throw `ForumNotFoundError` when
   absent.
2. Copy that forum's preloaded definitions through the model's private friend
   operation.
3. Call `SessionRepository::prepare()`.
4. Build `SessionDescriptor` from the forum and prepared-session values.
5. Call `SessionController::from_shared_definitions()` with the model's shared
   persona roster, forum default, prepared database path, lease, and restore.
6. Return the descriptor and controller as `OpenedSession`.

There is no Entrance branch: Entrance definitions are in the model and Welcome
storage is in the repository. There are no TOML, Markdown, directory, or
catalog reads outside the repository call.

Let exceptions propagate. If controller construction fails, normal stack
unwinding must destroy the moved/prepared lease. Do not translate exceptions
to registry or HTTP enums here.

Add `tests/application/unit_session_open.cpp` covering:

- a stored session descriptor, character roster, default, and restored state;
- the Entrance/Welcome descriptor and Assistant roster;
- `ForumNotFoundError`, `SessionNotFoundError`, and lease contention;
- lease release when controller construction fails, using an existing
  construction-failure path rather than a new production seam;
- the split-brain regression: load the model, change an observable definition
  such as a character display name or forum default on disk, then open through
  this function and verify the controller still reflects the originally
  loaded model. A second model load should reflect the edit.

The regression need only observe stable behavior already exposed by the model,
descriptor, and controller. Do not add backend-inspection APIs solely for this
test.

### Phase 3 checkpoint

Build and run the full C++ suite. The replacement graph is now complete but is
not yet wired into production.

## Phase 4: Cut over the web graph atomically

Treat this as one build checkpoint. Edit the registry, lobby routes,
composition root, and their tests together. Do not delete `WebDiscovery` or
`WelcomeStorage` before this phase is complete.

### 1. Simplify `SessionRegistry`

In `src/ui/web/session_registry.h/.cpp`:

- delete the `Workspace`, `WebDiscovery`, and `WelcomeStorage` declarations and
  includes;
- delete `SessionRegistry::from_workspace()`;
- delete `open_welcome_session()` and the Entrance branch;
- retain the existing public constructor taking `RegistrySessionFactory`.

Do not change registry ownership, thread lifecycle, reattachment, deadlines,
or limits. Keep the current exception mapping in `owner_main()`:

| Exception | Registry result |
| --- | --- |
| `SessionBusyError` | `busy` |
| `SessionNotFoundError` or `ForumNotFoundError` | `not_found` |
| other ordinary exception | `internal_error` |
| `std::bad_alloc` | terminate as today |

Most registry unit tests use the injected callback and should remain
unchanged. Tests of `from_workspace()` should instead construct the real model
and repository and pass a lambda calling `open_session()`.

### 2. Rewrite `LobbyRoutes` dependencies

Change `LobbyRoutes` to receive:

- `std::shared_ptr<const WorkspaceModel>`;
- `std::shared_ptr<const SessionRepository>`;
- an `InitialSelection` value declared in `lobby_routes.h`;
- the existing registry and settings.

Store the two shared pointers and capture them by value in installed route
lambdas. Store/capture `InitialSelection` by value. The route layer no longer
includes `builtins.h`; startup supplies Guest and Entrance/Welcome.

Replace each old use directly:

- bootstrap personas, characters, and forums come from `WorkspaceModel`;
- forum summaries resolve member metadata through `find_character()` and keep
  sorting projected members by folded display name;
- character detail always uses `character_markdown()`, including Assistant;
- session listing calls `SessionRepository::list()` and overlays registry
  running state; its current JSON shape remains unchanged, so the repository's
  diagnostic `error` is not newly exposed;
- recent sessions loop over model forums, call repository `list()` for each,
  and keep the current descending timestamp sort;
- creation calls repository `create()`;
- open first keeps the current registry-only reattach check, then calls strict
  repository `validate()` for every identity, including Welcome, before
  calling `registry.open()`.

Keep route catches and response mapping unchanged. In particular, missing
forums/sessions are 404, a damaged selected database is not silently treated as
missing, and creating under Entrance remains 404 through
`ForumNotFoundError`.

### 3. Replace the composition in `web_main.cpp`

After configuration and diagnostic logging are initialized:

1. Load one `shared_ptr<const WorkspaceModel>`.
2. Construct one `shared_ptr<const SessionRepository>` from
   `model->session_directories()` and a temporary seed containing the built-in
   Entrance/Welcome identity and label.
3. Construct `SessionRegistry` with a lambda capturing both shared pointers by
   value and returning `RegistryOwnerInput{open_session(...)}`.
4. Construct `InitialSelection` from Guest and Entrance/Welcome.
5. Install `LobbyRoutes` with the model, repository, and selection.

Preserve declaration and destruction order inside the existing inner scope:

1. the HTTP server is destroyed first and releases route captures;
2. the registry stops/joins owners and releases its callback captures;
3. the repository and model are destroyed;
4. diagnostic logging is shut down after leaving the scope.

Update the existing lifetime comment to name the new objects. Do not add a
lifetime manager.

### 4. Migrate web-facing tests

Update the existing local test fixtures rather than adding a production graph
builder:

- `unit_lobby_routes.cpp` should construct model/repository shared pointers and
  pass an explicit `InitialSelection`;
- the real-opening cases in `unit_session_registry.cpp` and
  `unit_session_routes.cpp` should use the real `open_session()` callback;
- fake registry-factory tests remain fake and unchanged;
- `stress_web_sessions.cpp` should use the repository for create/list and the
  new callback for opens;
- direct route composition in `process_web_server.cpp` should use the new
  graph;
- black-box process tests should retain the same HTTP assertions.

Pin the behavior most vulnerable to accidental drift: Guest-first bootstrap,
catalog order, recent-session order, Assistant detail, Welcome listing/open,
Entrance create rejection, tolerant damaged listings, strict damaged opens,
reattachment, and registry failure responses.

### 5. Document the one operational change

Update `README.md` and `docs/linux-webapp-package.md` with two concise facts:

- static workspace edits require a server restart;
- startup now validates every configured forum, so an invalid unused forum can
  prevent startup after upgrade and the reported error identifies that forum
  and source.

Also update the C++ ownership descriptions in `src/README.md`,
`src/application/README.md`, `src/session/README.md`, `src/apps/README.md`, and
`src/ui/web/README.md`. Describe the three final parts; do not add a migration
framework or an operations guide.

### Phase 4 checkpoint

Build and run the full C++ suite. Confirm manually that both discovery and the
registry callback now capture the same model and that no production call to
`SessionRegistry::from_workspace()` remains.

## Phase 5: Remove the old constellation

Only after the Phase 4 checkpoint is green, delete these production files and
their CMake entries:

- `src/session/workspace.h/.cpp`;
- `src/application/workspace_snapshot.h/.cpp`;
- `src/application/effective_personas.h/.cpp`;
- `src/application/workspace_inventory.h/.cpp`;
- `src/application/web_discovery.h/.cpp`;
- `src/application/welcome_storage.h/.cpp`.

Update `src/application/builtins.h/.cpp`:

- retain built-in IDs/names, `builtin_guest()`, `application_guide()`, and
  `builtin_assistant_definitions()`;
- remove `builtin_entrance()` because the model now constructs `ForumInfo`
  directly;
- remove the include of `session/workspace.h`.

There should be no alias for `Workspace`, `Forum`, or `SessionSummary` and no
deprecated overload left behind.

### Migrate and consolidate old tests

Move assertions to the component that now owns the behavior, then remove tests
whose only subject was a deleted wrapper:

| Existing area | Final owner |
| --- | --- |
| workspace layout, persona/character/forum validation, defaults, prompts | `unit_workspace_model.cpp` |
| snapshot ordering, effective personas, and built-ins in discovery | `unit_workspace_model.cpp` and the retained built-ins tests |
| exact `WorkspaceInventory` object-shape tests | remove with the object; retain the built-ins test that Assistant embeds supplied inventory, without adding a test-only model accessor |
| workspace create/list/check and Welcome storage | `unit_session_repository.cpp` |
| workspace controller opening | `unit_session_open.cpp` |
| direct `SessionCatalog` behavior | keep in `unit_session_catalog.cpp` using explicit fixture session paths |
| concurrent workspace opens | keep the concurrency intent but use model + repository + `open_session()` |

Remove the obsolete CMake entries for `unit_workspace.cpp`,
`unit_persona_loader.cpp`, `unit_workspace_snapshot.cpp`,
`unit_workspace_inventory.cpp`, `unit_effective_personas.cpp`, and
`unit_welcome_storage.cpp` once their relevant cases have moved. Avoid keeping
the same assertion in both an old and new file.

For remaining special cases:

- tests that used `Forum::directory` only to construct `SessionCatalog` should
  use `fixture.root() / "forums" / forum_id / "sessions"`;
- `unit_protocol.cpp` should move its workspace-loading assertion to the model
  tests or use `WorkspaceModel` directly;
- the checked-in-workspace integration test may load `WorkspaceModel` for
  forum/persona metadata and call existing agent loaders with explicit fixture
  paths when it needs mutable definitions for its mock provider; do not expose
  model definitions publicly for that test;
- keep `TestWorkspace` as a filesystem-writing fixture.

Update `CMakeLists.txt` so `cha_core` contains the three new `.cpp` files and no
deleted source, and the test target contains the three new unit-test files and
no deleted test source.

## Final verification

Run the clean C++ validation again:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure
```

Then perform these source checks:

1. No old include, type, or composition entry point remains:

   ```sh
   rg 'session/workspace\.h|application/(workspace_snapshot|workspace_inventory|web_discovery|effective_personas|welcome_storage)\.h|from_workspace\(|builtin_entrance\(' src tests CMakeLists.txt
   ```

   The command should produce no matches.

2. `src/session/` does not include an `application/` header.
3. `LobbyRoutes` does not read TOML, Markdown, character directories, forum
   directories, or session database paths directly.
4. `session_open.cpp` obtains definitions only through private model access and
   storage only through `SessionRepository::prepare()`.
5. No public model method returns `AgentDefinition`, `ProviderConfig`, or a
   workspace layout path other than the explicit startup-only session
   directory values.
6. The web composition owns exactly one model and one repository, and their
   captures are released before logging shutdown.

The implementation is complete when the full C++ suite passes, all existing
HTTP and storage behavior is preserved, the stricter startup behavior is
documented, and the old constellation has been deleted rather than wrapped.
