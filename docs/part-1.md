# Part 1: Replace the workspace/discovery constellation

Status: proposed C++ redesign.

This proposal is intentionally limited to the current web application. It does
not introduce infrastructure for other frontends, multiple storage backends,
live reload, plugins, or future deployment models.

## Decision

Replace the current static-workspace constellation with two concrete stateful
components and one application operation:

| Part | One responsibility |
| --- | --- |
| `WorkspaceModel` | Load and own the process's immutable workspace configuration. |
| `SessionRepository` | List, validate, create, and prepare mutable session databases. |
| `open_session()` | Combine a model definition with prepared storage to construct a `SessionController`. |

After migration, remove `Workspace`, `WorkspaceSnapshot`,
`EffectivePersonas`, `WorkspaceInventory`, `WebDiscovery`, and
`WelcomeStorage`.

The central rule is simple:

> Static workspace files are read only during startup. Once `WorkspaceModel`
> is constructed, every discovery response and session open uses that model.

Session SQLite files remain dynamic and continue to appear without restarting
the server.

## Why change the current design

The current design has two problems that are easy to encounter and difficult
to explain.

First, one logical workspace is represented by several objects. `Workspace`
validates the filesystem, `WorkspaceSnapshot` reads much of it again,
`EffectivePersonas` adds Guest, `WorkspaceInventory` derives Assistant's
reference data, and `WebDiscovery` adds Assistant and Entrance. Routes and the
session registry must keep several of those objects alive together.

Second, discovery is a startup snapshot but opening is not. A character or
forum can be shown using the values captured by `WebDiscovery`, then opened
using files that were edited after startup. The browser and controller can
therefore disagree about display names, membership, defaults, providers, or
prompts.

There is also a smaller responsibility problem: `Workspace` mixes static
configuration with mutable session storage and controller construction. A
maintainer cannot tell from the class name whether an operation is a lookup,
filesystem scan, validation pass, database operation, or full controller
assembly.

The redesign fixes those issues without changing the session actor, database
format, HTTP protocol, or browser code.

## Scope and non-goals

This part does:

- establish one process-lifetime model for static workspace data;
- keep session database operations live and separate;
- remove wrappers that exist only to reshape workspace data for the web UI;
- make new session openings use the same definitions shown by discovery;
- preserve the current HTTP behavior, leasing, recovery, and shutdown model.

This part does not:

- add live reload or file watching;
- generalize storage into a backend or policy framework;
- redesign `SessionCatalog`, `SessionLease`, `SessionController`, or
  `SessionRegistry`;
- require a new parser hierarchy or a cache for every source file;
- introduce fixed startup-time or heap budgets;
- perform a broad agent/character terminology rename;
- change JSON response shapes or browser behavior.

The application is small enough that a straightforward in-memory model is the
right tradeoff. If startup or memory later becomes measurable trouble, it can
be profiled then rather than anticipated with extra abstractions now.

## Target dependency graph

```text
LobbyRoutes ----------> WorkspaceModel
LobbyRoutes ----------> SessionRepository

SessionRegistry callback
        |
        v
open_session(WorkspaceModel, SessionRepository) ---> SessionController
```

`LobbyRoutes` also reads catalog metadata from `WorkspaceModel`. The repository
does not depend on an application-layer type, and the model does not inspect
session databases.

The two objects are justified because their lifetimes and kinds of work are
different:

- `WorkspaceModel` is immutable and performs no I/O after construction.
- `SessionRepository` performs filesystem and SQLite work on mutable storage.

`open_session()` has no independent state, so it remains a free application
function. Wrapping it in a one-method factory class would duplicate the
registry's existing callback abstraction. Combining the two objects would
recreate the current mixed responsibility under a new name.

## `WorkspaceModel`

### Responsibility

`WorkspaceModel` is the authoritative static workspace for one server process.
It owns:

- the parsed `WorkspaceConfig`;
- the effective persona roster, including Guest;
- character metadata, including Assistant;
- character detail Markdown, with `application_guide()` stored as Assistant's
  detail;
- forum metadata and membership, including Entrance;
- fully resolved `AgentDefinition` values for each forum;
- Assistant's inventory text;
- the persistent session directory for each workspace forum;
- stable-ID indexes used by routes and `open_session()`.

It performs no filesystem reads after successful construction.

### Public shape

The public interface should expose only the values used by routes and startup
composition:

```cpp
struct ForumInfo {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    std::vector<std::string> member_ids;
    std::string default_character_id;
};

struct ForumSessionDirectory {
    std::string forum_id;
    std::filesystem::path directory;
};

class WorkspaceModel final {
public:
    static WorkspaceModel load(
        std::filesystem::path root,
        WorkspaceConfig config);

    const SharedPersonaRoster& personas() const noexcept;
    std::span<const CharacterDefinitionMetadata> characters() const noexcept;
    std::span<const ForumInfo> forums() const noexcept;

    const CharacterDefinitionMetadata* find_character(
        std::string_view id) const noexcept;
    const ForumInfo* find_forum(std::string_view id) const noexcept;
    std::string_view character_markdown(std::string_view id) const;

    std::vector<ForumSessionDirectory> session_directories() const;

private:
    friend OpenedSession open_session(
        const WorkspaceModel&,
        const SessionRepository&,
        const SessionIdentity&,
        WakeNotifier&);

    std::vector<AgentDefinition> copy_definitions_for(
        std::string_view forum_id) const;
};
```

`ForumSessionDirectory` is declared with `SessionRepository` in
`session/session_repository.h`; it is shown here because the model returns that
plain construction value. The dependency therefore remains application code
using a session-layer value, never `session/` including `WorkspaceModel`.

`ForumInfo` deliberately has no filesystem path. Routes should not learn the
workspace layout. `session_directories()` returns the small path-bearing value
needed once by application startup to construct `SessionRepository`.

`AgentDefinition` can contain provider configuration, including inline keys.
It is therefore not exposed on the general model API. The one
`open_session()` function is a friend and may call the private copy operation.
A factory class, access class, capability header, or interface would add
ceremony without creating a stronger boundary in this codebase.

### Loading

Construction performs one all-or-nothing startup pass:

1. Load personas, characters, and forums.
2. Validate IDs, reserved built-ins, display-name collisions, forum membership,
   and default characters.
3. Build the Guest-inclusive persona roster.
4. Resolve every forum's effective definitions and prompts.
5. Build Assistant's inventory and definition.
6. Add Assistant and Entrance to the public catalogs.
7. Build stable-ID indexes and publish the completed model.

`load_workspace_config()` should parse `workspace.toml` once and supply the
result both to logging setup and model construction.

The implementation should reuse the existing parsing and overlay helpers. It
does not need a new public loader framework. Reading a small source twice inside
this one startup operation is acceptable when avoiding it would require a large
parser refactor; the important invariant is that no static source is re-read by
routes, creation, or opening after the model is published.

Model construction fails if any configured forum has an invalid default,
member override, or prompt, even when that forum has not yet been opened. This
is stricter than current behavior and should be stated in the release notes and
workspace documentation. The error should identify the forum and source file;
no new diagnostic subsystem is required.

### Stable observable order

Preserve the ordering already visible to the web application:

- Guest is first, followed by custom personas ordered by folded display name;
- characters, including Assistant, are ordered by folded display name;
- forums, including Entrance, are ordered by folded display name;
- forum member IDs retain their current stable-ID order, while the web member
  projection remains ordered by display name.

These rules should be implemented once during model construction rather than in
several route helpers.

### Restart semantics

Edits to `workspace.toml`, personas, character definitions, forums, member
overrides, or prompts take effect after restart. If a file is edited while the
server runs, both existing discovery responses and newly opened sessions keep
using the loaded values. Session database changes remain immediately visible.

Live reload is not approximated by selectively re-reading individual files.

## `SessionRepository`

### Responsibility

`SessionRepository` owns the storage operations currently spread across
`Workspace`, `SessionCatalog`, `WelcomeStorage`, and route helpers. It belongs
in `session/` and receives plain paths and IDs rather than a `WorkspaceModel`.

The application has two real storage cases:

1. persistent per-forum catalogs; and
2. the one process-local Welcome session.

The repository should implement those two cases directly. A variant-based
storage-policy framework is unnecessary.

### Construction and interface

```cpp
struct TemporarySessionSeed {
    SessionIdentity identity;
    std::string label;
};

struct SessionEntry {
    SessionIdentity identity;
    std::string label;
    std::string error;
    std::filesystem::file_time_type updated_at;
};

struct PreparedSession {
    SessionIdentity identity;
    std::string label;
    std::filesystem::path database_path;
    SessionLease lease;
    SessionRestore restore;
};

class SessionRepository final {
public:
    SessionRepository(
        std::vector<ForumSessionDirectory> persistent,
        TemporarySessionSeed temporary);
    ~SessionRepository();

    SessionRepository(const SessionRepository&) = delete;
    SessionRepository& operator=(const SessionRepository&) = delete;

    std::vector<SessionEntry> list(std::string_view forum_id) const;
    void validate(const SessionIdentity& identity) const;
    SessionEntry create(std::string_view forum_id, std::string label) const;
    PreparedSession prepare(const SessionIdentity& identity) const;
};
```

At construction, the repository creates one private temporary database from
`TemporarySessionSeed`. Application startup supplies Entrance, Welcome, and the
Welcome label. The session layer does not include `application/builtins.h` and
does not hard-code those names. The repository removes the owned temporary
directory in its destructor, preserving the existing `WelcomeStorage`
behavior.

No general list of storage kinds is needed.

### Operation behavior

For a persistent forum, operations delegate to a short-lived
`SessionCatalog`. For the temporary session:

- `list()` returns the one Welcome row;
- `validate()` accepts only the Welcome identity;
- `prepare()` leases and restores the Welcome database;
- `create()` throws `ForumNotFoundError`, preserving today's 404 without an
  Entrance branch in the route.

Routes no longer contain separate Entrance listing or timestamp code.

`list()` retains today's tolerant catalog behavior: an identifiable database
with invalid metadata remains visible with a fallback label and non-empty
`error`. `validate()` is strict and throws for a missing or invalid selected
session. This preserves the existing difference between listing and
`Workspace::check_session()` without requiring separate result hierarchies.

`SessionEntry` includes `updated_at`, allowing the route to avoid a second
timestamp pass. The repository still reads one modification time per returned
database; this change only puts the work in one place. Persistent results remain
ordered by session ID, and recent sessions remain ordered by descending
timestamp in the route.

For persistent storage, `prepare()` keeps the current safety-sensitive order:

1. Resolve the database and perform the existing metadata check.
2. Acquire the existing `SessionLease`.
3. Read the selected session metadata again to obtain its label.
4. Restore state while holding the lease.
5. Return the path, label, restore, and owning lease.

The duplicate metadata read is small for this application and is retained so
the architectural refactor does not also alter storage-validation semantics.
The important existing rule remains that restoration occurs only after the
lease is acquired.

### Concurrency

A constructed repository supports concurrent const calls. It keeps only an
immutable forum-to-directory map, constructs catalog/database helpers per
operation, and relies on the existing catalog and session leases for mutation
and exclusion. `SessionRegistry` continues to serialize in-process opening of
the same `SessionIdentity`; no repository-wide mutex is added.

## Opening a controller

One free application function is the only production path that creates a
`SessionController`:

```cpp
OpenedSession open_session(
    const WorkspaceModel& model,
    const SessionRepository& sessions,
    const SessionIdentity& identity,
    WakeNotifier& notifier);
```

Opening is intentionally small:

1. Find the forum in `WorkspaceModel`.
2. Copy its preloaded definitions through the model's private friend access.
3. Ask `SessionRepository` to prepare storage.
4. Build the `SessionDescriptor`.
5. Construct the controller with the model's shared persona roster.

This operation does not read TOML or Markdown. Completion-client initialization
and provider discovery still occur when the controller is constructed, just as
they do today.

Entrance and Welcome require no branch in this function. Entrance is an
ordinary forum in the model with Assistant as its definition, and Welcome is an
ordinary prepared session returned by the repository.

`SessionRegistry` keeps its existing lifecycle, owner threads, startup waiters,
limits, reattachment, and error mapping. Its existing production callback
captures the model and repository and calls `open_session()`.

## Built-ins and initial selection

The current built-ins remain explicit application facts:

- Guest is part of the model's effective personas.
- Assistant is part of the model's characters and forum definitions.
- Entrance is part of the model's forums.
- Welcome is the repository's temporary session, supplied by startup.

Assistant's character detail uses the cached `application_guide()` just like a
workspace character uses cached `CHARACTER.md`. The Assistant special case is
therefore removed from `LobbyRoutes`.

The initial persona and session are small startup values, not model state:

```cpp
struct InitialSelection {
    std::string persona_id;
    SessionIdentity session;
};
```

For the current application, startup initializes this with Guest and
Entrance/Welcome and passes it to `LobbyRoutes`.

## Composition and lifetime

The composition root becomes approximately:

```cpp
auto model = std::make_shared<const WorkspaceModel>(
    WorkspaceModel::load(application.workspace, workspace_config));

auto sessions = std::make_shared<const SessionRepository>(
    model->session_directories(),
    TemporarySessionSeed{
        {std::string(entrance_id), std::string(welcome_id)},
        std::string(welcome_name)});

SessionRegistry registry(
    settings,
    [model, sessions](
        const SessionIdentity& identity,
        WakeNotifier& notifier) -> RegistryOwnerInput {
        return RegistryOwnerInput{
            open_session(*model, *sessions, identity, notifier)};
    });

const InitialSelection initial{
    std::string(guest_id),
    {std::string(entrance_id), std::string(welcome_id)}};

LobbyRoutes(model, sessions, initial, registry, settings).install(server);
```

The existing inner scope in `web_main.cpp` remains important. The HTTP server
releases route captures, the registry joins and destroys session owners, then
the repository and model are released before diagnostic logging is shut down.
No new lifetime manager is needed.

## HTTP behavior to preserve

This redesign does not introduce new web errors or response shapes.

- Missing forums or sessions remain 404.
- Direct creation for Entrance remains 404.
- A damaged database remains visible in listings but fails strict validation or
  opening as an internal storage error.
- Lease contention remains `session_busy`.
- Existing registry stopping, limit, timeout, and server-shutdown responses are
  unchanged.
- Reattaching to an already running session remains registry-only and does not
  revalidate its database.

Existing route and registry tests are the contract. The repository and opening
function should propagate the current domain exceptions rather than translating
them into HTTP concepts.

## Files and removals

New production files:

```text
src/application/workspace_model.h
src/application/workspace_model.cpp
src/application/session_open.h
src/application/session_open.cpp
src/session/session_repository.h
src/session/session_repository.cpp
```

`WorkspaceConfig` and `load_workspace_config()` move from
`session/workspace.h/.cpp` into the model files because they are application
startup configuration. Small parsing helpers may stay as private free
functions or translation-unit code; they should not become new public services.

Remove after consumers migrate:

- `Workspace` and `Forum`;
- `WorkspaceSnapshot`;
- `EffectivePersonas`;
- `WorkspaceInventory`;
- `WebDiscovery`;
- `WelcomeStorage`;
- obsolete CMake entries and abstraction-specific tests.

`SessionCatalog`, `CatalogLease`, `SessionLease`, database restoration, and
`TestWorkspace` remain. `TestWorkspace` is a useful filesystem fixture, not an
architectural dependency.

## Testing

Tests should follow the two objects and the opening operation.

`WorkspaceModel` tests cover loading, validation, built-ins, ordering,
Assistant's guide and inventory, and static immutability. The key regression
loads a model, edits a character or forum on disk, verifies the model is
unchanged, opens a session through `open_session()`, and verifies the original
loaded definition is used. A newly loaded second model should see the edit.

Tests that currently expect `create_stored_session()` to discover a broken
prompt move to `WorkspaceModel::load()`. After a successful load, editing that
prompt does not make repository creation or opening re-read it.

`SessionRepository` tests cover listing, strict validation, creation,
timestamps, temporary Welcome storage, cleanup, leasing, and concurrent calls.
They retain the distinction between an error-bearing list row and a strict
validation failure.

Opening tests cover stored and Welcome sessions, descriptor construction, use
of the shared persona roster, absence of static file reads during open, and
lease release when controller construction fails.

Existing C++ route, registry, process, and stress tests continue to cover HTTP
behavior and owner-thread concurrency. Their fixtures should construct the new
model/repository graph and registry callback.

Tests currently using `Workspace` only to reach `Forum::directory` should
construct `SessionCatalog` from an explicit fixture path. Workspace-loading
tests move to `WorkspaceModel`; storage tests move to `SessionRepository`; open
tests move to `open_session()`. Do not recreate `Forum::directory` on the new
path-free `ForumInfo` merely to keep an old test shape.

## Migration sequence

Each step must build and pass the C++ suite.

### Step 1: Add `WorkspaceModel`

- Move `WorkspaceConfig` and its loader into the model files.
- Add the model and reuse existing loading helpers.
- Add model validation and model-local immutability tests.
- Keep the current production `Workspace`/`WebDiscovery` path intact.

At this point, do not claim production openings are immutable; they still use
`Workspace::open_session()`.

### Step 2: Add storage and opening components

- Add `SessionRepository` with persistent catalogs and the one temporary
  session.
- Add `open_session()` and its end-to-end immutability test.
- Keep `WebDiscovery`, `WelcomeStorage`, and the current registry composition
  until the production cutover.

### Step 3: Cut over routes and registry together

- Construct the model and repository in `web_main.cpp`.
- Move all discovery/detail routes to `WorkspaceModel`.
- Move list/create/validate operations to `SessionRepository`.
- Change the registry's production callback to `open_session()`.
- Move Welcome into the repository and remove route/registry built-in branches.
- Migrate affected route, registry, process, and stress fixtures.
- Update restart and stricter-startup documentation in the same change.

Only after both routes and registry use the new graph should
`WebDiscovery`, its supporting wrappers, and `WelcomeStorage` be deleted. This
avoids an intermediate build with two competing discovery/open paths.

### Step 4: Remove `Workspace`

- Move remaining direct `Workspace` tests to their new owners.
- Change catalog tests to use explicit fixture paths.
- Delete `Workspace`, old value types, obsolete tests, and CMake entries.
- Run all C++ unit, process, and stress tests.

No compatibility facade remains after this step.

## Acceptance criteria

The redesign is complete when:

- `chaweb` constructs one `WorkspaceModel`;
- routes and newly opened controllers use that same model;
- no static workspace file is read by a route, session creation, or session
  opening;
- session databases created after startup remain immediately discoverable;
- `SessionRepository` depends only on plain IDs and paths, not application
  classes or built-in constants;
- Welcome lists and opens through the repository, with no Welcome branch in
  routes or the registry;
- Assistant detail comes from the model's cached application guide;
- the public model API does not expose `AgentDefinition` or provider secrets;
- current HTTP behavior, persistence, leasing, concurrency, and shutdown
  behavior remain covered by tests;
- the old workspace/discovery classes and compatibility paths are removed;
- all C++ tests pass.

## Result

After this change, the architecture can be summarized without reconstructing a
constellation of wrappers:

- `WorkspaceModel` answers what this process knows about the workspace.
- `SessionRepository` answers what session storage exists now.
- `open_session()` opens one of those sessions using the loaded model.

That is the whole design. Additional abstraction should require an additional
real application requirement.
