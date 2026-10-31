# Component boundaries and incremental refactoring

Date: 2026-10-31

This document describes the proposed component structure and the steps to reach
it. Target names, interfaces, and commands described as proposed do not exist
yet. Creating this document does not implement the refactor.

The next implementation work is header cleanup and Notes extraction. The
component map below is the direction for the wider codebase, not a requirement
to create all its build targets before Notes becomes useful. Use that first
extraction to refine the later work. Add enforcement tools or build machinery
only where they solve a demonstrated problem.

## Purpose

Make it possible to understand, change, review, and test a substantial part of
CHA without understanding the entire application.

The working rule is:

> A change can stay local when it preserves the component's observable contract
> and its assumptions about shared resources. A change to either expands review
> and verification to the affected consumers.

Observable contracts include behavior, errors, ordering, cancellation,
ownership, and persistence guarantees. They are not limited to function
signatures or JSON fields.

The intended benefits are:

- A feature has an identifiable owner and a limited set of dependencies.
- Most implementation changes involve fewer unrelated files.
- Public headers expose less implementation detail and cause fewer rebuilds.
- Developers can build and run the relevant component tests directly.
- Integration tests concentrate on behavior that actually crosses boundaries.

Keep the existing deployment model, native hosts, daemon, and browser clients.
Use ordinary C++ libraries, concrete classes, value types, and existing callback
seams. This work does not require separate services, a plugin system, a generic
dependency-injection framework, an event bus, or another database. It does not
change the UI, wire protocol, database format, or product behavior as a side
effect of moving code.

## Why the present structure is insufficient

The current directories describe responsibilities, but they do not enforce
component boundaries. The [native architecture overview](../src/README.md)
explicitly describes this as a directory convention. The shared native
implementation belongs to `cha_lib`, and its public include path exposes the
entire `src` tree.
The main native test executables combine several source areas.

Several interfaces are already useful:

- `Providers` accepts owned request input and returns events through a
  cancellable request handle.
- `LiveSessionManager` hides the runtime thread that owns live controllers.
- `Workspace` provides an immutable configuration snapshot.
- `SessionRepository` and `SessionJournal` provide typed persistence operations.
- `ChaClient`, `NotesClient`, and `NoteAudioClient` let browser tests replace
  application calls without starting C++.
- Shared wire fixtures check the C++ and TypeScript sides independently.

The problem is that these interfaces coexist with broad dependencies and
central objects that collect unrelated features. For example:

- [Application](../src/app/application.h) includes storage, workspace, provider,
  media, and runtime headers. Operations from several features share its
  implementation state.
- [WorkspaceConfigStore](../src/workspace/workspace_config_store.h) owns notes,
  folder-store operations, audio attempts, and events in addition to workspace
  configuration.
- [runtime/protocol.h](../src/runtime/protocol.h) combines many feature DTOs with
  internal runtime delivery types.
- [model_context.h](../src/characters/model_context.h) obtains
  `FolderStoreService` through a persistence header that imports notes storage.
  This makes an unrelated declaration affect provider and session builds.
- [voice_output.h](../src/providers/voice_output.h) imports the session
  repository to obtain audio value types.

The inspected Ninja dependency records showed that 88 unique project source
files, out of 177 non-generated project C++ sources in that build, included
`note_storage.h` transitively. This is a snapshot of the inspected build, not a
permanent repository metric. It supports reducing header dependencies. It does
not establish the size of the eventual speed improvement.

An implementation-file change already recompiles its own translation unit.
Separate libraries alone do not make that operation cheaper. The main build
benefits must come from narrower headers and from building smaller test targets
without unrelated application and host dependencies.

## Proposed components

Use the following seven native components as the working architecture, including
the composition layer, and keep the web UI as a separately built client. Their
proposed library targets support independent builds and tests as each boundary
becomes useful; creating seven targets is not a goal by itself. Component
membership is determined by responsibility, not by the current directory name.
A source can first move to a different build target without a large directory
rename.

| Component | Proposed target | Responsibility | Main public surface |
| --- | --- | --- | --- |
| Base | `cha_base` | Small shared values and low-level infrastructure | IDs, records, utilities, storage primitives |
| Workspace | `cha_workspace` | Configuration loading, validation, edits, publication, saved credentials | Immutable `Workspace`, typed edits and credential access |
| Providers | `cha_providers` | Model requests, provider protocols, tool transport, authentication transport | Owned requests, events, results, cancellation |
| Sessions | `cha_sessions` | Conversation behavior, live runtime, session persistence | Session commands, snapshots, output, repository operations |
| Media | `cha_media` | Speech, dictation, audio processing, streams and resources | Audio requests, streams, status, cancellation |
| Notes | `cha_notes` | Notes, folder stores, imports, compression, summaries, durable note audio | Typed notes operations and operation status |
| Application | `cha_app` | Composition, vault lifecycle, cross-component coordination and external adapters | Application operations, native bridge and daemon adapters |

The existing `cha_lib` name can remain as an aggregate link target during and
after migration. It must not compile a second copy of sources already owned by
component libraries. Its consumers can continue to use the aggregate while
component tests link only their required libraries. Preserve the actual native
host and daemon executable boundaries.

### Base

Base contains code that several components need without needing one another's
implementation:

- Stable IDs, small shared transcript/content records, and simple value types.
- Domain-neutral text, filesystem, logging, synchronization, and encoding
  helpers that are actually shared.
- The low-level SQLite wrapper, transaction and lease primitives, and the
  shared vault-format creation and validation implementation.
- The existing small activity-event persistence helper, including the operation
  that appends an event inside a caller-owned transaction.

Base does not own active vault state, component services, provider execution,
configuration publication, or feature workflows. Live transcript mutation and
conversation policy belong to Sessions. Shared transcript records may remain
in Base where Providers and Sessions both need them.

Base must not become a destination for code that is merely difficult to place.
Each addition needs an actual shared consumer. Keep value headers separate from
SQLite, curl, and other implementation headers so importing an ID or record
does not import the corresponding infrastructure.

The shared vault schema is an existing requirement: Sessions, Welcome, workspace
initialization, and local fixtures need the same format. Schema-only helpers
belong in a narrow Base storage unit, separate from feature workflows and
queries. This is not permission to move Notes behavior into Base.

### Workspace

Workspace owns configuration rows, parsing, validation, typed edits, immutable
snapshot publication, character definitions, and saved API-key configuration.
`WorkspaceConfigStore` should eventually describe this responsibility accurately.

Move the database-backed `ApiKeyStore` facade with configuration ownership. Its
current placement under Providers does not make credential editing a provider
transport responsibility. Keep network authentication and token-refresh work
with Providers; preserve the current credential-file and locking behavior.
Voice configuration values belong with Workspace, while speech execution
belongs with Media.

Workspace exposes snapshots and explicit edits. It does not expose mutable
internal maps or a general database handle to ordinary consumers. Successful
publication follows a successful commit. Validation or commit failure leaves
both committed configuration and the published snapshot unchanged.

Global vault leases and maintenance coordination ultimately belong to
Application. Do not move them out of the current store until their lifetime,
locking, and shutdown behavior can be preserved in the same change.

### Providers

Providers owns model API clients, request preparation, protocol decoding, SSE,
generation events, provider workers, web-search/read transport, Jev execution,
and model-context projection. Provider-specific folder-store HTTP operations
also belong here; local folder-store state belongs to Notes.

Retain the existing request boundary:

1. The caller supplies owned request/configuration values.
2. Providers executes the request and emits semantic events or a result.
3. The caller can cancel through the request handle.

Keep `ProviderClientFactory`, `ModelBackend`, and transport replacement seams
that already support deterministic tests. Do not create a second execution
framework for component testing.

Put `FolderStoreService` in a small provider-owned value header. Model context
needs the service identity and remote-store references; it does not need local
notes persistence declarations. Likewise, avoid importing the whole workspace
model merely to name a small configuration value where a narrow header suffices.

Provider workers do not own notes state, session journals, or vault transitions.
Existing injected tool executors can call application-owned behavior, but their
lifetime, cancellation, and thread requirements remain part of the contract.

### Sessions

Sessions owns the controller, transcript mutation, recipient selection,
generation state, naming, the live-session runtime, session repository and
journal, Markdown export, and best-effort mirroring.

Its public surface includes:

- Listing, creating, opening, renaming, deleting, and exporting sessions.
- Submitting commands, stopping generation, and requesting snapshots.
- Receiving owning snapshots and output events across thread boundaries.
- The lifecycle operations needed for application maintenance and shutdown.

`SessionController` remains internal to the runtime-facing service boundary.
Its borrowed `ControllerView` remains valid only on the owner thread and only
until the next mutation. Adapters must not receive that view as a replacement
for an owning snapshot.

The runtime remains the sole owner of live controllers. This refactor does not
add another runtime, queue, controller registry, or transcript copy.

Sessions needs selected note content and folder-search information. Reuse the
existing `NoteReader`, `ItemReader`, and `StoreReader` seams where they have the
right semantics. Put the required value declarations in narrow headers and
connect the readers in Application. A read callback does not authorize arbitrary
access to the Notes service or replace transaction-specific storage operations.

### Media

Media owns speech output, voice input, codecs, audio streams, temporary media
resources, and audio-download scheduling. Move speech transport from the
Providers directory into this responsibility when extracting its target.

Preserve `AudioDownloadManager`, its existing transport seams, and its resource
lifecycle. Session-entry speech and caching can use the public session
repository API. That is an explicit Media-to-Sessions dependency, not a reason
to expose session internals or duplicate history.

Small audio values such as bytes and content type must be available without
importing a repository implementation header.

Note-audio responsibilities are divided by behavior:

- Media performs synthesis, remote audio transport, decoding, and encoding.
- Notes owns the durable attempt, note association, acceptance, completion, and
  resulting note state.
- Application coordinates vault maintenance and shutdown across both.

This division does not introduce a second job registry or another copy of an
attempt's state. Move existing ownership; do not layer a new service over the
same mutable state indefinitely.

### Notes

Notes is the first full feature extraction because it currently crosses
Application, WorkspaceConfigStore, storage, media, bridge, and browser code.
Giving it a single owner can reduce review scope more than a broad storage
library alone.

Notes owns:

- Note and folder persistence, editing rules, references, and imports.
- Folder-store records, upload/delete workflows, and remote-outcome state.
- Compression, summary generation and commitment, and their operation state.
- Durable note-audio attempts and note-level audio behavior.
- Feature-specific errors, results, and contract tests.

A Notes service exposes typed operations for these behaviors. Use the existing
operations as the starting API. It need not be one large class, and a new
abstract interface is not required for every operation.

Summary generation reads session content through a public Sessions operation.
Folder-store transport uses Providers. Audio processing uses Media.
Configuration and credential selection use Workspace. Notes does not navigate
`Application::Impl`, borrow its internal maps, or retain a mutable Workspace.

Move operation state with the code that owns it. Keep application lifecycle
coordination explicit, using the smallest existing hooks that preserve the
current checks. Do not move the entire shared application implementation into
a new object named `NotesContext`.

### Application and clients

Application constructs the components and owns process-wide coordination:

- Active-vault identity, admission, context epochs, maintenance, and shutdown.
- Database replacement, vault switching, merge, backup, and R2 coordination.
- Operations that must commit or invalidate state across components.
- Native envelopes, connection management, dispatch adapters, daemon adapters,
  and host integration.

Application can expose component service accessors after those services have a
complete lifecycle contract. Retain forwarding methods while they still enforce
meaningful admission, error translation, or coordination. Their removal is an
optional cleanup, not a prerequisite for extraction.

Production adapters must not use methods marked as test-only to reach internal
owners. Replace such access with an ordinary result or immutable snapshot that
contains the required information.

The browser remains a separate build. Keep its typed client boundary and fake
clients. `NotesClient` already provides a useful feature interface. A screen
that needs only a few operations can use a narrow TypeScript type, including a
`Pick` of an existing interface. Avoid mandatory adapter classes for every screen.

## Allowed dependencies

The table defines the intended direct dependencies. A dependency must use the
other component's public headers. Transitive linkage does not grant permission
to include its private implementation.

| Consumer | Allowed direct dependencies | Reason |
| --- | --- | --- |
| Base | None of the other components | Shared values and low-level primitives |
| Workspace | Base | Configuration values, files, storage primitives |
| Providers | Workspace, Base | Request configuration, credentials, common values |
| Sessions | Providers, Workspace, Base | Generation, configuration, journal and shared values |
| Media | Sessions, Providers, Workspace, Base | Session-entry audio/cache, provider support, voice settings |
| Notes | Sessions, Media, Providers, Workspace, Base | Summaries, audio, remote stores, configuration, persistence |
| Application | All components | Construction, lifecycle, cross-component operations, adapters |

These are allowed edges, not required links for every source or test. For
example, an audio codec does not need the session repository merely because
Media's session-audio implementation does.

The current code does not satisfy this table. In particular, Sessions and
Workspace contain storage operations that know about Notes. Explain those
dependencies beside the affected operations while extracting them. Do not
conceal them with unrestricted callbacks or move all the code into Base. A
separate repository-wide dependency inventory is not required.

The target graph must remain acyclic. If a proposed extraction cannot preserve
an existing transaction within this graph, first define the transaction boundary
described below. Do not force an unsafe extraction to satisfy the diagram.

## Explaining an interface

Use the existing source README and header comments to explain the interface
being extracted. Describe the behavior a caller must know, especially ownership,
threading, cancellation, and persistence when they apply. Reuse existing
documentation instead of imposing a new contract template on every component.

For Notes, make its callable operations and required dependencies easy to find
in its service header. State the lifecycle and transaction rules that affect
those calls. There is no requirement for a separate public-header register or
seven-topic document for each component.

A narrow interface is not necessarily an interface with very few methods. Notes
has several real operations. The objective is that a caller depends only on the
operations and values it needs, without understanding the service's internals.

Public headers must not transitively expose another component's private headers.
Use forward declarations and separate value headers where sufficient. Extend an
existing Pimpl boundary when needed to hide private layout; do not introduce
Pimpl or virtual interfaces into every class by rule.

Shared values have one canonical definition. Moving a declaration into a small
header should not create another independently maintained DTO or persistence
model. Only introduce a projection when the consumer requires a different,
smaller meaning, and make that conversion explicit.

## Database and lifecycle invariants

These requirements are migration constraints, not follow-up improvements.

### One vault file, preserved connection ownership

Keep the selected persistent vault in one SQLite file. Preserve the temporary
Welcome database and its current cleanup policy.

Do not replace the existing connection model with one raw handle shared by all
components. `SqliteDatabase` uses `SQLITE_OPEN_NOMUTEX`; session journals own
separate connections, repository operations use short-lived connections, and
the store serializes access to its connection with its mutex. Moving ownership
must preserve those access rules and the maintenance behavior that closes or
retargets connections.

Ordinary feature APIs do not expose a raw database handle. A narrow native
storage-coordination operation may accept a connection when it must participate
in a caller-owned transaction. That connection is borrowed synchronously; it
must not escape into a worker or outlive the operation.

### Transactions that cross feature ownership

Several operations currently update more than one feature atomically:

- Configuration changes can synchronize note/forum metadata and delete session
  rows before publishing the new Workspace.
- Folder deletion updates events, store records, session associations, and the
  note subtree in one transaction.
- Persistent folder attachment checks store readiness inside the same journal
  transaction that records the attachment.

Preserve those atomic operations. Splitting them into several independently
committing service calls is not equivalent.

For a cross-component transaction, the coordinating operation must own one
transaction on one connection. Component storage helpers may operate within
that transaction without committing it. Configuration publication still occurs
only after the complete commit succeeds and while its existing serialization
guarantees hold. If extraction needs a new coordination header, keep it small
and state its permitted caller and transaction requirements there. It is part
of the supported native interface. Do not add a separate consumer registry,
expose it through the browser, or turn it into a general transaction framework.

Application owns coordination that spans independent feature services. Move
such coordination there only when the component operations can participate in
the same transaction safely. Until then, retain the existing implementation
and explain its ownership in the affected code. The Notes extraction does not
require relocating every cross-feature transaction.

Keep the existing shared vault-format creation and validation implementation
during Notes extraction. The eventual Base storage unit is its proposed owner,
but moving schema helpers is not a prerequisite for this feature change.
Application owns when initialization and maintenance run. Sessions, Welcome,
and local fixtures must continue to use the same initializer. If a later library
split requires moving it, move only schema code; do not pull Notes workflows or
feature-service models into Base.

A feature owns review of its table changes even when schema code belongs to a
shared link target. Preserve foreign keys, initialization order, transaction
scope, idempotence where applicable, validation, existing-vault handling, and
Welcome/test-database creation. Do not introduce creator callbacks, a schema
registry, a second initializer, or a schema migration merely to change source
ownership.

### Folder readiness is a transaction contract

The existing readiness callback in `SessionJournal::add_folder` is used for
Welcome, whose journal is in a separate database. Persistent sessions check
readiness on their own journal connection, inside the attachment transaction.

Do not replace the persistent check with the current store-reading callback.
That could lose the same-transaction check and reverse lock order: a journal
could hold the SQLite write lock while waiting for the store mutex, while a
store operation holds that mutex and waits for the database write lock.

A safe extraction can use a narrow transaction-scoped storage operation supplied
at composition time. If it uses a callback, that callback must receive and use
the existing journal connection synchronously, acquire no store/lifecycle mutex,
and neither start nor commit a separate transaction. This is a different
contract from an ordinary `NoteReader`. Retain the present code until the
replacement preserves those properties and has relevant concurrency coverage.

### Admission and asynchronous work

A check at the router admits a request at one moment. It cannot prove that the
same vault remains active when a background result arrives.

Keep context identity attached to admitted work and preserve checks before
network retries, durable writes, and result publication where the current code
requires them. Existing compression and note-audio workers already make such
checks under the application lifecycle lock.

The component service must either retain the required checks or call a small,
explicit lifecycle operation that performs them under the existing lock. Do not
duplicate the active-vault state in each service. An abandoned reply does not
automatically roll back accepted work; preserve each operation's current
completion semantics.

The same protection must apply to native, daemon, and internal callers. It must
not depend on every caller passing through one particular bridge router.

### Shutdown and ownership

Preserve the current ownership sequence: stop admission, cancel/drain the
required work, release controllers and journal connections, perform maintenance
or shutdown, then release the resources those workers used.

Keep existing stop behavior, deadlines, delivery completion, and forced-exit
fallbacks. A service extracted from `Application::Impl` must not leave workers
holding pointers to an object destroyed earlier than before. Component
separation does not authorize a new worker, timer, registry, or cleanup policy.

## Wire contracts and feature ownership

Keep `resources/dto.yaml` as the existing portable schema source and retain
shared wire fixtures. Do not create another schema system as part of this work.

Split broad native DTO headers by feature as consumers are extracted. Keep
internal runtime handles, such as a subscription's live-session handle,
separate from portable request/result values. Provider and session core headers
must not import Application or bridge routing types.

A feature should own its request/result definitions, serialization expectations,
dispatch adapter file, and fixture coverage. Ownership does not require all
those files to compile into the feature's core library. A Notes dispatch adapter
can be maintained with Notes changes while compiling into `cha_app`; it calls
the Notes public API and uses common bridge envelopes. This avoids a dependency
from Notes back to Application.

Keep common envelope validation, connection state, acknowledgements, reply
delivery, and transport policy in the adapter layer. Move feature behavior out
of the router without copying that common machinery into each feature.

Retain the central method enum initially. Separate dispatch files and public
interfaces are sufficient for the first extraction. Replace the enum with
per-feature handler tables only if the remaining central edits demonstrate a
concrete maintenance problem. Do not add a runtime registry merely to avoid an
ordinary source edit.

## Build boundaries and enforcement

Start with the existing `cha_lib` and test targets. Header cleanup and moving
Notes behavior behind a service interface do not require a library split. A
separate target is useful when it lets developers build the relevant tests
without unrelated code; introduce that target when its dependencies can be
expressed plainly. Do not split all lower layers as a prerequisite for Notes.

Use existing CMake source lists and directories to show ownership. Do not add
a parallel file-to-component map, public-header register, or exception database.
Component-local CMake files are optional when they make the build easier to read.
The root remains responsible for composition and platform builds.

Configure external include paths and link dependencies only where required.
Use `PRIVATE` when a dependency is an implementation detail and `PUBLIC` when
the public interface actually requires it. Static-library consumers may still
need transitive linkage; this does not justify exporting unrelated headers.

Initially, review the changed includes and use ordinary searches to check the
boundary being extracted. If private-header imports repeatedly undermine that
boundary, consider a small check for that observed problem, using the existing
structure. An include checker is not a prerequisite or a completion criterion.

CMake target separation alone cannot prevent private-header imports while
`src` remains a shared include root. Neither build targets nor an include check
prove callback, SQL, lock-order, or behavioral correctness. Those still need
review and the relevant tests.

## Verification model

### Local component tests

Move existing tests rather than replacing the test framework. Notes service
tests should call the service directly and avoid starting Application or a host
when those are not part of the behavior under test. They can initially run in
an existing test executable linked to `cha_lib`. Focused execution is useful
even before independent compilation is available.

If a separate local test target is introduced to reduce build work, verify that
it actually avoids the unrelated application/host sources. Do not claim build
isolation merely because the tests have moved to another file.

Use real temporary databases for persistence and session behavior. Replace
provider/network execution with the existing scripted backends, injected
transports, or local fake servers. A fake repository for every test would add
maintenance work and remove useful persistence coverage.

Database fixtures reuse the existing shared production schema initializer;
they do not copy its SQL or require Application just to create a valid vault.
Component implementation tests may inspect their own internal headers. Tests
of a boundary use the supported interface so that they exercise the contract
available to consumers.

Keep cross-component tests in their existing application/bridge targets unless
a move has a concrete benefit. Test filters are sufficient initially. Add a
feature label or separate executable when it simplifies test selection or
reduces build work, not to make all components look alike.

### Integration tests

Keep a small deliberate set of tests for:

- Submit, stream, persist, close, and reopen.
- Cancellation and stop during generation or audio processing.
- Vault switch or maintenance while work is pending.
- Rejecting stale operations and preventing old workers from writing a new vault.
- Summary commit versus abandon, including connection ownership.
- Folder attachment/deletion, transaction rollback, and related session state.
- Configuration commit, rollback, and snapshot publication.
- Shutdown, resource release, and native/daemon delivery behavior.

Classify bridge tests by what they verify. A note-editing rule can move to Notes
service tests. An envelope, epoch, admission, connection, or delivery test remains
an adapter/integration test even when its method starts with `notes.`. Moving a
test file does not make a full-application fixture independent or faster.

### Selecting tests for a change

| Change | Development verification |
| --- | --- |
| Provider encoding/decoding | Providers tests with scripted responses or local transport fixtures |
| Workspace validation/editing | Workspace tests; transaction/publication integration cases when affected |
| Session behavior or persistence | Sessions tests with real temporary storage and fake provider output |
| Audio transport/codec | Relevant Media tests; session/note integration for cache or durable-state changes |
| Notes rules or persistence | Notes service/storage tests; affected boundary fixtures |
| UI interaction | Type checking and affected browser tests |
| Wire contract | Schema check, serializer/dispatcher tests, consumer tests and shared fixtures |
| Cancellation, locking, maintenance, shutdown | Affected component and integration suites; targeted sanitizer checks where useful |
| Shared database format or broad dependency change | All affected consumers, broader regression and relevant platform checks |

Private implementation changes can require integration tests when behavior
crosses a boundary. Conversely, a mechanical declaration move does not require
inventing tests that merely mirror file placement.

Keep full offline regression at integration/release checkpoints. Run live
external-provider checks explicitly when the change calls for them; they are
not the default component development loop.

### Current and proposed commands

Current native targets include `cha_tests`, `cha_app_tests`, `cha_bridge_tests`,
and platform-specific tests. Existing CTest labels distinguish `daemon` and
`native`. A GTest filter can select cases, but building its executable still
builds that executable's existing dependencies.

If a separate Notes target and labels prove useful, the following commands
illustrate the resulting workflow. These are conditional examples, not required
deliverables or commands supported by the current build:

```sh
cmake --build --preset ninja --target cha_notes_tests
ctest --test-dir build/ninja -L '^notes$' -LE '^integration$' --output-on-failure --no-tests=error
```

CTest selects execution; it does not build tests. Build the existing executable
that contains the selected integration cases before running them. A new
integration target is not required. If feature labels are added, preserve the
current platform labels as additional classifications.

On the browser side, `vitest run src/notes` selects only part of Notes coverage.
Relevant tests also include `components/Notes.test.tsx`,
`components/NotesScreen.test.tsx`, `components/NotesTree.test.tsx`, and API/wire
tests. Define an explicit feature test command or grouping that includes those
paths. Preserve whole-web TypeScript and schema checks for API changes.

## Migration plan

Each step leaves the application buildable and preserves behavior. Keep
structural moves separate from product changes and from changes to transaction,
connection, or lifecycle policy. The stages are not instructions to combine
all their work into one large change.

### Stage 1: Confirm the Notes boundary

Read the Notes operations, their existing dependencies, transaction boundaries,
and worker lifetimes. Identify the tests that cover the behavior being moved.
Use the interfaces and invariants described here; update them only where the
code establishes a concrete gap.

This is preparation for the affected feature, not a repository-wide inventory,
checker implementation, or benchmark project.

Exit condition: the next small Notes move has a clear owner, dependencies, and
appropriate verification.

### Stage 2: Remove unnecessary header dependencies

Start with declaration-only changes:

- Separate `FolderStoreService` from folder-store persistence declarations.
- Separate shared audio values from the session repository header.
- Remove implementation includes from public Application and DTO headers where
  forward declarations or narrow value headers are sufficient.
- Separate internal delivery handles from portable wire values.

Do not include schema relocation or folder-readiness callback changes in this
mechanical stage. Those need transaction-aware work of their own.

Exit condition: public declarations have fewer unrelated transitive includes,
all affected builds pass, and no behavior or wire values change.

### Stage 3: Extract Notes behavior and ownership

Move Notes rules and operation state into a Notes service. Begin with synchronous
operations whose contracts are simple, then move compression, summaries,
folder-store workflows, and note audio one at a time.

Keep Application forwarding and lifecycle coordination initially. Service tests
call the extracted operations directly, while integration tests continue to
exercise the application entry points. Do not weaken asynchronous epoch checks
to simplify the new constructor or service API.

Use WorkspaceConfigStore's existing typed operations as the backing store while
moving Notes behavior. Preserve its mutex, connections, leases, and atomic
operations. Do not add a second store or move schema, credentials, speech
transport, or global maintenance merely to complete a directory diagram.

Move note persistence later only if the existing access still causes a concrete
boundary problem and its transaction ownership is understood. The constraints
above apply to that work; a complete WorkspaceConfigStore split is not required
to finish the initial Notes extraction.

Exit condition: Notes owns its state and behavior, ordinary Notes changes do not
require editing shared application internals, and the relevant transaction and
background-work guarantees remain covered.

### Stage 4: Verify the initial Notes result

Keep the existing Notes dispatch adapter, client interface, and wire fixtures.
Narrow the native interface and move DTO declarations only where required by
the extracted service. Remove duplicated state and obsolete forwarding that
the extraction actually makes unnecessary.

Run the focused Notes tests and relevant adapter/lifecycle integration cases.
Use existing test filters first. Check whether a representative internal Notes
change can now be reviewed without navigating shared application internals.

If compilation remains a material cost, identify the dependency responsible and
consider a focused test/library split. Check a representative before/after case
for that proposed change rather than requiring all the targets in the map.

Exit condition: a normal internal Notes change can be understood and verified
from Notes plus its documented public dependencies. Changes to shared behavior
still select the required integration tests.

### Later work: Improve the remaining boundaries

The wider objective remains useful boundaries across the codebase. Use the
component map and the Notes result to choose the next boundary that causes real
review or verification difficulty. Apply the pattern there, with a concrete
benefit and a small implementation step. Do not turn the working map into an
automatic sweep through every directory.

Narrow Application to composition and genuine cross-component operations as
those extractions justify it. Remove forwarding methods only when doing so
does not remove checks or coordination.

Group browser files or split additional client interfaces when that improves
feature ownership. Avoid a large rename-only sweep mixed with functional
extraction. Defer method-table redesign unless there is demonstrated remaining
benefit.

The initial Notes work can finish without completing these later extractions.
Their order and exact build targets should reflect demonstrated needs while
preserving the codebase-wide direction.

## Measuring whether the change helped

First assess the change that motivated the work: can an internal Notes change
be understood from the Notes service and its existing dependencies, and can its
tests be selected directly? This does not require a measurement framework.

If a later build change is justified by speed, compare one representative edit
or focused test build before and after under comparable conditions. Record the
commands, build configuration, and elapsed time in that change's validation.
Use more scenarios only if the result is unclear. A five-scenario benchmark and
recurring measurement process are not required.

Distinguish build time from test execution time, and do not call summed Ninja
command durations CPU time. Avoid speedup claims that have not been measured.

## Completion criteria

The initial header-cleanup and Notes work is complete when:

- The identified unnecessary include chains have been reduced without changing
  the affected values or behavior.
- Notes behavior and operation state have a clear owner, and ordinary internal
  Notes changes no longer require navigating `Application::Impl`.
- The Notes interface uses explicit existing dependencies and does not merely
  hide the whole application implementation behind a new context object.
- Configuration publication, cross-feature transactions, vault identity,
  cancellation, and shutdown retain their guarantees.
- Notes tests and the relevant integration cases can be selected using existing
  filters or another equally simple mechanism.
- Shared wire fixtures continue to check the affected native/browser contract.
- The extraction's duplicated state and obsolete code have been removed.

Completing all seven targets, relocating every source, adding an include
checker, and running a benchmark program are not acceptance conditions for this
step. The component map remains the direction for subsequent improvements;
each further change must earn its maintenance cost. The purpose remains a small
application that is easier to change.
