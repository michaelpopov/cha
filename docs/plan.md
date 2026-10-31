# Technical implementation plan: header cleanup and Notes extraction

Date: 2026-10-31

This plan implements the initial work in [refactor.md](refactor.md): reduce
unnecessary header dependencies and give Notes behavior and operation state one
owner. The seven-component map remains the direction for later work. This plan
can finish with the existing `cha_lib`, storage layout, and test executables.

The steps below are implementation changes, not work performed by creating
this document. New file and method names are proposed. Each step should leave
the application buildable and preserve its current behavior.

## 1. Result and implementation limits

After this work:

- Provider model context no longer imports note persistence just to name a
  folder-store service. Speech declarations no longer import a repository just
  to name audio bytes.
- A concrete `NotesService` owns note rules, compression, summary reservations,
  folder-store operations, and note-audio worker controls.
- `Application` continues to expose the current operations. Its Notes methods
  delegate to the service, with host resources and global lifecycle coordination
  remaining in Application.
- `WorkspaceConfigStore` remains the typed backing store. Its connections,
  locks, schema setup, leases, and atomic operations retain their current owners.
- Notes tests can call the service directly where Application is not part of
  the behavior under test. Existing integration coverage remains available.

Keep the current wire names, JSON, error codes, user-visible messages, database
format, platform restrictions, retry policies, and cancellation semantics. Use
existing provider transports and `BackgroundJobs`. Do not combine the extraction
with a new library hierarchy, include checker, source inventory, job runner,
schema framework, or UI change.

## 2. Existing code that must move together

| Current location | Role in this plan |
| --- | --- |
| [application.h](../src/app/application.h) | Preserve public operation signatures; narrow declarations needed by Notes |
| [application_internal.h](../src/app/application_internal.h) | Replace Notes state fields with one service owner as each feature moves |
| [notes_operations.cpp](../src/app/notes_operations.cpp) | Move editing rules, compression, summary, and page/picture import behavior |
| [folder_store_operations.cpp](../src/app/folder_store_operations.cpp) | Move reservations, controls, upload/remove/delete workflows and status |
| [note_audio_operations.cpp](../src/app/note_audio_operations.cpp) | Move durable-attempt orchestration and worker controls; retain host-resource adaptation |
| [application.cpp](../src/app/application.cpp) | Construct the service; update summary cancellation, pending-status queries and shutdown |
| [vault_operations.cpp](../src/app/vault_operations.cpp) | Delegate feature cancellation/recovery at the same lifecycle points |
| [media_operations.cpp](../src/app/media_operations.cpp) | Preserve deferred summary cleanup on connection close |
| [workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp) | Keep existing note persistence and transactions as service dependencies |
| [notes_dispatch.cpp](../src/bridge/notes_dispatch.cpp) | Keep dispatch and envelope validation; continue through Application |
| [CMakeLists.txt](../CMakeLists.txt) | Register moved sources and service tests in existing targets |

The important ownership moves are:

| State currently in `Application::Impl` | New owner | Synchronization to preserve |
| --- | --- | --- |
| `CompressionControl`, `compressions`, `compression_mutex`, compression notification | `NotesService` implementation | Existing lifecycle mutex for map/retirement; existing timed mutex for serialization |
| `SummaryRun`, `summaries`, `next_summary_run` | `NotesService` implementation | Existing lifecycle mutex; existing run identity and connection/epoch checks |
| `background_audio`, `background_audio_mutex`, `background_audio_warned`, `audio_submission_mutex`, audio notification | `NotesService` implementation | Existing audio mutex, lifecycle checks, and submission serialization |
| `FolderStoreControl`, operation/phase enums, controls map, next operation ID and transport override | `NotesService` implementation | Existing lifecycle mutex, per-control wait mutex/CV and cancellation flags |

Move each state item once. Do not retain a shadow registry in Application.

## 3. Service shape and application integration

### 3.1 Files

Add a small `src/notes/` implementation area:

- `notes_service.h`: callable operations and lifecycle entry points.
- `notes_service.cpp`: construction and synchronous operations.
- `notes_service_internal.h`: private state shared only by Notes implementation
  files, if splitting those files requires it.
- Move the relevant implementation into `notes_operations.cpp`,
  `folder_store_operations.cpp`, and `note_audio_operations.cpp` under this area
  as the corresponding steps land. Keep the existing feature division rather
  than creating one file per method.
- `notes_types.h`: the existing folder-store result structs and notification
  callback type needed by both the service and Application.

Use one concrete `NotesService`, with its private state behind an implementation
object so consumers do not import worker maps and transport implementation
headers. Do not create an abstract service interface or a generic context object.
Keep storage and media implementation files in their present locations.

### 3.2 Public values and signatures

Start from the current Application signatures. Keep existing `NoteItem`,
`NoteContent`, `NoteImportEntry`, `NoteAudioClip`, JSON status values, and
`OperationReply`; this is not an API redesign.

Move `FolderStoreChoiceView`, `FolderStoreStart`, and
`FolderStoreOperationStatus` out of the nested Application declaration into
`notes_types.h`. Preserve their fields. Application can provide aliases with the
old nested names, so dispatch and existing callers do not require a broad edit.
Do the same for its `NoteAudioChanged` callback alias.

The service should not include `application.h` just to obtain `ApplicationError`
or `OperationReply`. Move those exact declarations into one lightweight shared
header, proposed as `app/application_result.h`, and include it from
`application.h`. Keep their namespaces and implementations unchanged initially.
They remain the same types; do not replace replies with another future or add
new completion state.

### 3.3 Dependencies

Pass the concrete objects already used by the moved code:

| Dependency | Purpose |
| --- | --- |
| `WorkspaceConfigStore&` | Snapshots, typed persistence, events and durable attempts |
| `ApiKeyStore&`, `OpenAiOAuth&` | The current per-request credential resolution path |
| `SessionRepository&`, `LiveSessionManager&` | Saved summary input and existing queued notices/pruning |
| `BackgroundJobs&` | Launch/cancel work through the existing application-owned runner |
| `const RuntimeSettings&` | Existing poll settings and deterministic test hooks |
| Existing lifecycle mutex and published epoch | Serialize operations and identify their admitted vault context |
| Admission and recovery-eligibility callbacks | Call the existing admission policy under its lock; preserve the existing atomic running/stopping checks used before scheduling recovery |
| Current-vault name accessor and native-host capability | Preserve folder-operation identity and native-only audio behavior |

Add each dependency when its feature moves. Small named callbacks are suitable
for admission, recovery eligibility and current-vault access; they must expose
only that operation. Do not pass `Application&`, `Application::Impl&`, all application
fields, or a callback that returns them. Do not copy mutable admission or vault
state into Notes.

`MediaResources` and `PendingMediaRegistry` stay in Application. For
`note_audio_source`, the service supplies the saved clip through the existing
typed store operation; the Application adapter creates the connection-scoped
resource while holding the same lifecycle lock as today.

### 3.4 Lock convention

Use two explicit forms only where callers require them:

- Ordinary service methods acquire the existing lifecycle mutex and perform
  admission checks. Thin Application forwarders call them without first taking
  that mutex.
- Methods ending in `_locked` require the caller to hold that mutex. They serve
  maintenance, internal status queries, and cross-operation coordination that
  already occurs inside a locked application operation. They do not acquire it
  recursively or bypass a check that their current implementation performs.

Workers lock that same mutex at the same validation/commit points as today and
release it for network work and waits. The admission callback requires the lock
and does not acquire it itself. Copy the existing policy; do not replace it with
a check of the epoch alone.

Keep note-audio interruption callable without taking the lifecycle mutex:
`interrupt_note_audio()` uses the existing audio-control mutex and signals the
existing flags. Stop paths currently rely on this property. Retain the existing
ordering between lifecycle, audio-control, store and per-control wait locks;
do not introduce a lock held across provider I/O or worker joins.

### 3.5 Lifetime and startup

Add one `std::unique_ptr<NotesService>` to `Application::Impl`. Construct it in
the constructor body after the repository and live-session manager exist and
before normal operation/callback registration can use it. The store,
credentials, runner, settings and lifecycle state must outlive the service.

The existing store object is retargeted during vault maintenance; do not cache
its raw database handle or the old database path in the service. Keep the current
per-operation/per-run copies where the code already requires an origin identity.

Keep the existing `BackgroundJobs` join before service destruction. On a shutdown
timeout, the application's existing `preserve_on_destroy` path must retain Notes
and its borrowed dependencies together. A Notes destructor must not introduce
another unconditional join. A direct service test fixture must also stop/join
its runner before destroying the service or dependencies.

## 4. Implementation sequence

### Change 1: Remove the demonstrated header dependencies

1. Add `providers/folder_store_types.h` with the existing
   `FolderStoreService` enum. Include it from `folder_store_storage.h` and
   `characters/model_context.h`; remove the persistence include from the latter.
   Preserve enumerators and namespace. Update other consumers only when their
   own declarations permit the same reduction.
2. Add `media/audio_data.h` containing the existing `EntryAudio` value. Include
   it from `session_repository.h` and `voice_output.h`. Forward-declare
   `EntryAudioLookup` in `voice_output.h`, where it is used by const reference;
   include the repository in the implementation that reads its fields. Keep
   the full lookup type where `AudioDownloadManager` stores it by value.
3. Move `ErrorCode` and its `to_string` declaration from `runtime/protocol.h`
   into `runtime/error_code.h`. Retain the definition of `to_string` in its
   existing implementation. Have the protocol and lightweight failure headers
   include the new declaration. This also lets `media/xai_socket.h` avoid the
   full protocol header.
4. Move the native-only `SubscribeResult` and its `LiveSession` forward
   declaration into `runtime/command_queue.h`, where the result variant is
   defined. Preserve its endpoint ownership and all completion behavior. Do not
   turn this small move into a rewrite of all command or presentation DTOs.
5. Introduce `app/application_result.h` as described above. Remove only those
   Application includes that are now unnecessary. Types stored by value or in
   instantiated templates still need complete definitions; do not rely on
   accidental transitive includes.

Build the existing native test/host consumers affected by these headers. Run
provider/model-context, audio, protocol, subscription and wire-fixture tests as
appropriate. There is no new runtime behavior to test merely because a type
changed files. Use searches to confirm each moved type still has one definition.

Done when these specific include chains are shorter and existing consumers
compile with identical types and serialization.

### Change 2: Introduce NotesService and move simple editing behavior

Create the service and shared Notes result declarations. Register new `.cpp`
files in `cha_lib`. Add the service owner and construction to Application.

First move `list_note_items`, `get_note`, `save_note`,
`save_note_scroll_position`, and `rename_note_item`. Preserve folder-store
summaries in listings and current note-error translation. Keep the current
public Application functions as thin forwarders.

Leave create/import/delete guards and read-only/audio interactions in their
present code until the related worker state moves. Moving them prematurely
would need callbacks into unrelated private application fields.

Introduce direct service tests in `tests/notes/unit_notes_service.cpp`, registered
in `cha_app_tests` initially. Reuse the existing real workspace/database fixture
and production initializer. Pass real existing dependencies and use the current
network seams when needed; do not duplicate the application startup machinery.

Move suitable editing assertions from full-stack tests only when equivalent
service coverage is in place. Keep dispatch, envelope and admission assertions
at their application/bridge entry points.

Done when these editing rules are exercised through the service and no new
Notes implementation needs `application_internal.h`.

### Change 3: Move compression state, execution and cancellation

Move `CompressionControl`, its map, serialization mutex, notification callback,
and compression helpers into the service with their worker code. Replace worker
captures of the application implementation with service ownership backed by the
application's existing runner/lifetime contract.

Preserve all current behavior:

- Ineligible/short-note skips occur before provider validation where they do
  today; event failures can still reject acceptance before network work.
- The same cancellation flag is registered with `BackgroundJobs` and the
  compression control. A cancelled predecessor cannot retire a replacement.
- Resolve the configured key before each actual request/retry. Do not snapshot
  a secret for the lifetime of queued work or substitute a newly named key.
- Recheck admission and source text at the existing points. Commit through
  `commit_compressed_note`, retaining its source comparison and atomic event.
- Remove a completed control before best-effort notification/event handling.
  Failure to notify does not leave a completed job pending.

Update maintenance calls to `cancel_compressions_locked()`. Update existing
deletion code to call narrow service operations for collecting affected controls
before the store commit and retiring them after it. Keep the same traversal and
lock scope; do not add a deletion index or retire work after a failed delete.
These helpers become private when deletion moves into Notes in Change 6.

Keep `AssistantService` pending-status queries working through a service
`compression_status_locked` operation. Its caller already holds the lifecycle
lock, so it must not use the ordinary lock-taking status entry point.

Done when no application file accesses the compression map or control type and
the existing compression/cancellation/maintenance tests still pass.

### Change 4: Move summaries and document retrieval

Move `SummaryRun`, reservations, the run counter, summary source preparation,
generation, commit and abandon into Notes. Reuse `OperationReply` and the current
page/picture download functions. Compression and page formatting share the
existing provider-preparation helpers in Notes implementation code.

Keep the summary protocol as three existing operations: generate Markdown,
commit the converted document, or abandon the reservation. Preserve the source
filtering, saved-history input, label, `NoteSummaryTarget`, connection ownership,
readiness state and run/epoch checks. Do not introduce reservation expiry.

Update these external callers with the distinct operation they already need:

- Session deletion in `Application::delete_session` signals the matching run's
  cancellation flag; it must not accidentally replace that behavior with an
  unconditional reservation retirement.
- Maintenance/shutdown invokes the existing full cancellation/retirement path
  through `cancel_summaries_locked()`.
- Connection close in `media_operations.cpp` still schedules cleanup through
  `BackgroundJobs` before taking the lifecycle lock. Keep UI/router callers
  from waiting synchronously on that lock.

Every launched summary reply still settles if its reservation disappears during
generation or retry. After a successful `commit_summary`, failed session/audio
notifications must not convert the committed save into an error result.

Until note-audio ownership moves in Change 5, keep the post-commit audio cleanup
and notification in the Application adapter. For this intermediate step, that
adapter retains its existing lock scope and calls a service
`commit_session_summary_locked` operation, then performs the existing
best-effort audio effects before releasing the lock. Do not add a callback into
`Application::Impl` or release/reacquire the lock between commit and these
effects. Once audio has moved, the service performs those effects directly and
the ordinary Application method becomes a thin forwarder.

For page import, retain complete-article limits, truncation rejection, optional
YouTube/page formatting, credential checks and error sanitization. Picture
retrieval retains its current cancellation and reply behavior. Do not add a new
epoch fence to a pure download unless the current operation already needs one;
moving code is not a reason to change its semantics.

Done when summary/import behavior resides in Notes and Application/connection
cleanup code no longer inspects summary reservations directly.

### Change 5: Move durable note-audio orchestration

Move audio control maps, warning suppression, submission mutex, recovery and
worker orchestration into Notes. Keep synthesis/download/codec functions under
`src/media/`. Preserve all native-platform and ChaWeb restrictions.

The acceptance transaction and launch barrier are critical:

1. Hold the existing lifecycle lock while calling `accept_note_audio`.
2. Preserve its launch callback and the exact cancellation flag registered for
   that attempt. The worker cannot enter its guarded work until acceptance has
   either committed or failed.
3. If acceptance rolls back after launch, cancel/remove only that control. A
   late cleanup must not erase a replacement that reused the same attempt ID.

Keep the current distinction between safe upload retries, batch-creation
uncertainty, batch registration, polling, local finalization and recovery.
Registered batches resume from their saved ID. An unknown creation outcome
must not trigger an automatic second creation. Retain per-request credential
resolution and interruption checks under the current lock.

Application retains the order of calls in `pause_resources`, `resume_resources`,
epoch publication and vault reopen/merge. Replace direct field access with
service methods at those exact points, including clearing warning suppression
only where the existing code clears it. Recovery remains scheduled after the
store's maintenance guard is released. Callback registration keeps its current
recover/interrupt behavior; service construction does not start recovery.

Move the read-only transition's abandonment handling into Notes when it can
call the owned audio-control code directly. Keep resource creation in the
Application adapter as described in section 3.3.

Done when no application file reads the audio maps/warning set or owns the
submission mutex, while acceptance, recovery and vault-switch tests still pass.

### Change 6: Move folder stores and finish editing ownership

Move folder-store controls, counters, phases, injected transport, reservation
checks and workers into Notes. Keep provider HTTP helpers in Providers and
durable records in the existing store. Forward the current transport test setter
from Application to the service.

Preserve separate forward-work and cleanup cancellation flags, learned remote
IDs, uncertainty records, terminal status retention, explicit retry/close and
delete-anyway behavior. An uncertain allocation must not become a retryable
create merely because the code moved. Keep polling waits, lifecycle checks and
per-control wait-mutex ordering unchanged.

Now move `create_note_item`, `import_note_book`, `delete_note_item` and remaining
editing/abandonment rules into the same service. They can call folder reservation,
compression and audio helpers directly without callbacks into Application.

For deletion, capture affected folder IDs and controls before committing. Keep
`delete_folder_subtree_locally` as the single atomic store operation for events,
store records, session associations and note rows. Retire controls only after
the commit and retain existing best-effort Welcome pruning behavior. Do not
replace the session journal's same-transaction folder-readiness check with a
Notes callback; that storage path is outside this extraction.

Done when Notes editing and background state have one owner and the three
Application operation files contain adapters/forwarding rather than duplicated
feature implementation.

### Change 7: Remove obsolete access and validate the boundary

Search all callers before removing each old field/helper. In particular, check
`application.cpp`, `vault_operations.cpp`, `media_operations.cpp`, and the
AssistantService pending-status callback. Keep Application's global lifecycle
state, shared runner, credentials, sessions and host resources.

Preserve meaningful Application forwarding and error translation. Remove only
forwarders/helpers that became unnecessary; no public `notes()` accessor or
bridge method-registry redesign is required.

Confirm that `src/notes/` does not include `application_internal.h` or retain an
Application implementation pointer. Review its explicit dependencies and
worker captures. Run the selected integration cases and the normal native
regression once after the final ownership changes.

If the remaining build cost calls for an independent Notes test target, propose
that small follow-up from the actual dependencies. It is not a gate for this
plan, and it does not authorize extracting all seven libraries.

## 5. Caller changes that must not be missed

| Caller | Required service interaction |
| --- | --- |
| Application constructor | Create Notes after its borrowed dependencies; pending-notes callback queries service status under its existing lock |
| Application note operations | Forward to lock-taking service methods; avoid recursive acquisition |
| `delete_session` | Signal cancellation of the matching summary with current semantics |
| `pause_resources` / `resume_resources` | Interrupt and schedule audio recovery at current points |
| Vault maintenance drain | Cancel compression/summaries and interrupt folder operations before the existing maintenance transition |
| Epoch publication | Interrupt old audio controls before the existing epoch bump |
| Vault reopen/merge | Reset audio warnings at current successful database transitions |
| Connection release | Queue summary cleanup before taking the lifecycle lock |
| Shutdown | Signal the existing runner, preserve interruption that does not take the lifecycle lock, and join dependencies in the existing order |
| Host callback registration | Forward Notes callbacks without starting recovery earlier than today |

Do not collapse these into one new generic `reset()` method: they deliberately
have different effects on durable attempts, reservations and notifications.

## 6. Verification by change

Use existing tests first. Add focused service tests only for behavior moved to
the service or an uncovered lifetime boundary. Do not write tests that merely
assert a declaration's new filename.

| Change | Existing coverage to retain/select |
| --- | --- |
| Header cleanup | Provider/model-context and audio tests; live-session subscription tests; `WebWireFixtures` |
| Editing service | `NotesWireContract`, document-size/managed-folder/import cases; storage tests and new direct service assertions |
| Compression | Bridge `*Compression*` cases; vault cancellation cases; AssistantService pending-status query |
| Summaries/imports | Bridge `*Summary*` and YouTube/page cases; session deletion, disconnect, cancellation and vault-maintenance cases |
| Note audio | `NoteAudioBackground.*`; media note-audio/codec tests; note-audio bridge validation |
| Folder stores | `FolderStoreOperationsTest.*`; bridge delete/retry/disconnect cases; referenced-folder session tests |
| Final integration | Vault maintenance, application shutdown, protocol/fixtures, bridge, daemon and relevant native-host tests |

Particularly useful existing regression cases include:

- `NoteAudioBackground.AcceptanceCommitRollbackCannotSubmitOrCleanUpTheImmediateRetry`.
- `NoteAudioBackground.VaultSwitchesRecoverOnlyTheCurrentAttemptWithOverlappingIds`.
- `FolderStoreOperationsTest.DrainDoesNotJoinAWorkerWaitingForTheLifecycleLock`.
- `FolderStoreOperationsTest.LostAllocationResponseStopsWithoutAutomaticReplay`.
- `BridgeRouterTest.SummaryRetiredDuringRetryWaitStillSettlesItsReply`.
- `BridgeRouterTest.SummaryConnectionCloseDoesNotWaitForTheLifecycleLock`.
- `BridgeRouterTest.CompressionCannotSaveIntoAChangedVault`.

Keep tests that exercise Application, routing, connection ownership, epochs or
maintenance in their current integration fixtures. For direct service tests,
reuse real temporary databases and the current transport/test hooks. Use worker
barriers and injected waits already available in `RuntimeSettings::TestHooks`
for races; do not depend on long sleeps or a live external service.

### Commands using the existing build

Run these from the repository root. Configure only if the build is not already
configured or CMake inputs changed. Build the executable containing the selected
tests before running it:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_tests cha_app_tests cha_bridge_tests
./build/ninja/cha_app_tests --gtest_filter='NoteAudioBackground.*:FolderStoreOperationsTest.*'
./build/ninja/cha_bridge_tests --gtest_filter='BridgeRouterTest.*Compression*:BridgeRouterTest.*Summary*:BridgeRouterTest.NotesWireContract'
ctest --test-dir build/ninja -R '^WebWireFixtures\.' --output-on-failure --no-tests=error
```

These filters are examples, not the full affected-test list. Select additional
cases from the table for the code being moved. GTest filters run inside the
existing mixed executables; they do not establish compile isolation. When test
names move, check discovery/list output so a stale filter cannot silently select
zero tests. Platform-specific audio cases still require a supported host.

At the final integration point, build the normal configured targets and run:

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure --no-tests=error
```

Use relevant existing sanitizer presets if a lock/lifetime concern remains.
Run focused checks again after a fix; do not repeat the whole suite without a
new change or unresolved concern. Live-provider tests remain an explicit choice
when a change actually requires them.

### Browser and wire checks

Native extraction should preserve the current schema and browser client. If
wire declarations/serialization are touched, run both C++ fixtures and the
existing schema, type and API tests:

```sh
npm --prefix webapp run api-types:check
npm --prefix webapp run typecheck
npm --prefix webapp run test -- src/api/wireFixtures.test.ts src/api/nativeClient.test.ts src/api/nativeBridge.test.ts
```

For affected Notes interaction behavior, include
`webapp/src/components/Notes.test.tsx`, `NotesScreen.test.tsx`,
`NotesTree.test.tsx`, and the relevant `webapp/src/notes/` tests. Selecting only
the latter directory misses screen coverage. Do not regenerate the schema or
change fixtures simply to make an unintended protocol change pass.

## 7. Acceptance and stopping point

The initial implementation is complete when the demonstrated include chains
are reduced, Notes behavior/state are owned by the service, and ordinary Notes
implementation changes no longer require editing `Application::Impl`. The
application's remaining Notes knowledge should be construction, forwarding,
global lifecycle coordination and host-resource adaptation.

The affected tests must protect the existing admission, transaction, identity,
retry, completion and shutdown behavior. No duplicate operation maps, copied
database initializer, second store, or obsolete extraction scaffolding remains.
The service's dependencies and required lock ownership must be clear from its
header/comments and ordinary call sites.

Finish here even if storage wrappers still live in WorkspaceConfigStore or
tests still link `cha_lib`. Use a later concrete review/build problem to select
the next component improvement. A dedicated library, include checker, benchmark
program and extraction of the remaining components are follow-up decisions,
not completion conditions for this plan.
