# Block 3 — Approved UI, screen refresh, and full validation

Status: Not started. Implementation instructions for the third and final
sequential agent session; requires completed blocks 1 and 2.

## Suggested session prompt

```text
Implement docs/block3.md using the current code, specs/d1-alt-design.md,
and specs/d1-alt-plan.md. Verify blocks 1 and 2 and their handoffs first.
Follow AGENTS.md and the recorded UI approval boundaries. Complete the
feature and validation. You may update the Execution record section of
docs/block3.md with final results and remaining validation limits. Leave
other docs/ files unchanged. Ask if an actual conflict needs a decision.
```

## 1. Objective and prerequisites

Expose the completed D1 backend through the native bridge and approved Settings
screen, make incoming changes visible through the existing client paths, and
validate the complete feature. This covers the remaining phase 7 work and all
phase 8 work in [the plan](../specs/d1-alt-plan.md). Fix integration defects in
their owning layer; do not create a replacement backend or redesign the feature.

Read [AGENTS.md](../AGENTS.md), [CLAUDE.md](../CLAUDE.md), the complete
[design](../specs/d1-alt-design.md), and the plan. Read the execution records in
[block 1](block1.md#execution-record--to-be-filled-by-the-implementing-session)
and [block 2](block2.md#execution-record--to-be-filled-by-the-implementing-session).
Inspect the current diff and implemented public interfaces. No previous chat is
needed, and neither this file nor a previous handoff is proof that code works.

Verify the prerequisite backend behavior:

| Contract | Required behavior |
|---|---|
| Credential get | Returns account/database identifiers and token presence without exposing the token. |
| Credential save | Accepts unchanged valid values, retains an omitted token, creates the remote table before saving locally, then starts the Worker. Failure leaves stored credentials unchanged. |
| Disconnect | Stops Worker access, removes local credentials and pending queue/flags, then tries the remote drop once. Remote failure does not prevent offline local disconnect. |
| Incoming application | Notes/configuration/chats apply through real code with no outgoing echo. Notes produce an application-level notification for a changed receive group. |
| Chat reservation | Stops/waits/applies/releases one record; deferred reservation leaves the record in D1. Reopening reads new data and counters. |
| Lifetime | Startup/periodic/final-send paths, R2 refusal/cleanup, daemon table recovery, and native waiting for the full final send attempt are implemented. |

Use the actual implemented method/type names. Resolve missing prerequisites
before building a screen over a stub. Carry forward exact live feasibility
results and platform gaps; do not rerun a successful expensive probe merely
because this is a new session.

Plan for 250K–375K tokens including source reads, UI work, tests, integration
fixes, and final review. This is an estimate, not a guarantee. Use the public
backend contracts and targeted code reads rather than loading the full backend
and all tests again. Keep substantial room below the 500K context window for
cross-layer failures.

## 2. Approved behavior and scope limits

The completed design and its recorded screen copy are approved. Implement them
without asking for approval again. If a new layout, control, label, or other
visible state is actually needed beyond that approval, present it visually and
obtain approval under AGENTS.md before implementation. Loading/busy text is not
already approved merely because a similar R2 screen has it. Use existing approved
labels where possible; do not invent additional UI to fill out the plan.
Save and Disconnect can retain their approved labels while their controls are
disabled during a request. Network activity does not require new busy wording.

Keep these decisions:

- One person uses one machine at a time. Accepted duplicates, partial delivery,
  registration gaps, and ID collisions are not bugs to solve with new machinery.
- No status dashboard, title/legend at the top of panels, extra helper copy,
  conflict UI, delivery tracking, cleanup marker, or new polling subsystem.
- Incoming writes never enqueue records or set flags. D1 credential edits do
  not synchronize. Import/merge and received configuration preserve local D1
  values and pending work. A key-path collision moves only the local D1 key,
  preserving ordinary key IDs and references.
- The note save algorithm is unchanged. Incoming content may replace unsaved
  editor text. Reuse editor reload and its existing save-state reset.
- ChaWeb has no D1 Settings, R2 transfer, or vault-copy UI. Its notes show changes
  when reopened. Its open chats must show incoming changes, including while idle.
- Closing waits for the full final send attempt, with existing request timeouts
  and no overall drain timer. Unsent records remain after failure.

The suggested session prompt explicitly includes updating this guide's execution
record. Leave all other `docs/` files unchanged. Preserve unrelated working-tree
changes.

## 3. Code entry points

| Area | Start here |
|---|---|
| Backend contract | Block 2 application credential operations, Worker callback, and error results |
| Native routing/policies | `src/bridge/bridge_protocol.{h,cpp}`, `src/bridge/bridge_router.cpp`, `src/bridge/settings_dispatch.cpp`, related dispatchers |
| DTOs and clients | `resources/dto.yaml`, `webapp/src/api/{client,nativeClient}.ts`, generated `webapp/src/api/schema.d.ts`, `tests/fixtures/wire` |
| Settings model and routes | `webapp/src/components/R2StorageSettings.tsx`, `Settings.tsx`, `ApiKeySettings.tsx`, `webapp/src/state/route.ts` |
| Desktop notes | `webapp/src/components/NotesScreen.tsx`, `NotesTree.tsx`, `App.tsx`, `webapp/src/notes/useNoteSave.ts`, `webapp/src/state/view.ts` |
| Desktop chat | `webapp/src/useLiveSession.ts` and its existing tests |
| ChaWeb | `webapp/src/chaweb/useChaweb.ts`, `client.ts`, and existing behavior/conversation tests |
| Runtime and native checks | `tests/runtime`, `tests/bridge`, `tests/native`, `packaging/host/runtime_bridge.cpp`, native host entry points |
| Integration | `tests/integration`, `CMakeLists.txt`, `Makefile`, `webapp/package.json` |

## 4. Implementation scope

### A. Bridge, schema, and clients

Expose D1 get/save/disconnect through the existing settings application layer,
native method names/enums/policies, request parsing, DTO definitions, typed
clients, response guards, and wire fixtures. Wire the block 2 operations; do not
duplicate their validation, persistence, or Worker lifetime rules in the bridge.
Preserve epoch checks and the existing secret-detail/update conventions.

Wire the application note-change callback into one `notes.changed` event per
affected receive group. Use the existing event delivery and epoch filtering
pattern. Keep configuration-driven note-tree changes included.

After DTO edits regenerate, rather than manually editing, declarations:

```sh
npm --prefix webapp run api-types
```

### B. D1 Settings screen

Add `D1SyncSettings.tsx` using the current R2 Settings layout and controls. Add
the `D1 sync` row under Settings → API Keys and the matching desktop route.
Fields are Account ID, Database ID, and API token. Buttons are Reset,
Save D1 credentials, and Disconnect from D1 when connected. Disconnect has a
confirmation dialog.

Use these exact approved texts:

| Element | Text |
|---|---|
| API Keys row | D1 sync |
| Row description | Configure synchronization between machines |
| Account ID placeholder | Paste account ID |
| Database ID placeholder | Paste database ID |
| API token placeholder when absent | Paste API token |
| API token placeholder when present | Leave blank to keep the current token |
| Confirmation title | Disconnect from D1? |
| Confirmation message | Disconnect this vault from D1 on this machine? Its D1 queue and its unsent changes will be deleted. |
| Confirmation buttons | Cancel; Disconnect from D1 |
| R2 upload refused | Changes are not sent to D1 yet. Try again when D1 can be reached. |
| Save: D1 request failure | D1 could not be reached. Check the account ID, database ID, and API token. |
| Save: missing machine name | This machine has no name. Add machine to app.toml, then save again. |
| Save: local persistence failure | D1 credentials could not be saved. |
| Credential load failure | D1 credentials could not be loaded. |
| Disconnect: local failure | D1 credentials could not be removed. |

Save remains available for unchanged valid credentials because it also recovers
a missing remote queue. Do not copy R2's dirty-only Save condition. An omitted
token keeps the saved token. Reset and cancellation follow the existing form
patterns. Display failures through the established R2-style locations. A remote
drop failure after local disconnect is a log warning, not a failed local action.

### C. Incoming note refresh

On `notes.changed`, reload the desktop note tree and open note once for the
group, ignoring old-vault/epoch events. Use the existing `noteRequest` reload:
`NotesScreen` replaces the editor and `useNoteSave` already resets pending save
state when the editor changes. Do not add version comparisons, dirty-content
merging, another save buffer, or a synchronization-specific save protocol.

Incoming content may discard unsaved text as approved. Verify that the existing
reload/reset path does not create an outgoing edit merely by showing incoming
content. ChaWeb note pages keep their reopen-to-refresh behavior.

### D. Chat refresh on both clients

Implement the smallest production adjustments established by block 1's probes
and block 2's reservation behavior. Desktop uses its existing `reloading`
stream recovery path. ChaWeb uses its own read/recovery loop, which currently
stops scheduled reads when generation ends. Extend that existing path so an
idle open chat also shows incoming changes; do not add a second polling loop.
Use the same timer with a 30-second delay after a successful idle read and the
existing 1-second delay while a reply is active. This cadence was approved on
November 9, 2026. Keep hidden-page polling paused, immediate reads on opening
or returning to the page, and existing failure-retry behavior. Add no user
setting or separate idle scheduler.

Cover reconnect/read attempts made while the reservation is held, subsequent
release, and running replies cancelled by incoming application. Reopening must
load new entries and counters. Reuse existing visibility, stale-response,
failure, and retry handling. Do not assume that a desktop test proves ChaWeb.

## 5. Focused validation

Add tests to existing bridge, DTO/wire, Settings, NotesScreen, desktop chat,
and ChaWeb suites. Check observable behavior rather than private helper names.

Required cases:

- Method policy/routing/DTO agreement; token presence and omission; credential
  load/save failures; missing/invalid machine name; unchanged Save; Reset;
  confirmation cancellation; successful offline disconnect.
- Correct event delivery and old-epoch rejection; one group refresh; incoming
  note replacement with an unsaved edit; no echo from reload; tree changes from
  incoming configuration; ChaWeb notes refreshing on reopen.
- Desktop and ChaWeb idle/generating chat updates; a failed read/reconnect while
  reserved followed by recovery; ChaWeb's 30-second idle and 1-second active
  delays, hidden-page pause, and immediate visibility refresh using one timer.
- Native screen/navigation smoke checks using the approved copy and controls.

Use targeted tests while editing. After they pass, run the complete regression
commands below. Broaden or repeat checks only for a new change, failure, or
unresolved concern. Register shared tests so the daemon subset includes them.

## 6. End-to-end acceptance

Use two disposable vault instances with distinct machine names. Exercise the
actual application/backend/bridge/client path where applicable. Include a
protected-vault run. Use scripted D1 transport for deterministic failures, then
isolated live D1 checks for behavior that requires the service.

| Scenario | Required result |
|---|---|
| A edits notes, configuration, and chats, then closes/switches; B opens | B has the changes after its first loop. All record kinds have both outgoing and incoming coverage. |
| Notes import and multi-row configuration import/merge | Parent ordering, managed folders, and removal of multiple forums/chats work. Destination D1 values or their absence and pending work are preserved. Include an ordinary-key/D1-ID collision locally and on receipt: only D1 moves, ordinary references work, and receipt creates no echo. |
| Large/split records, page boundaries, incomplete set, partial send, retry | Large saves remain permitted; complete records apply in order; unhandled pieces remain; accepted replay does not break the loop. |
| D1 unavailable while A changes content | Content and pending work persist; R2 upload is refused. Sending after access returns clears the block. |
| Incoming update to an open note or idle/generating chat | Approved replacement/cancellation behavior is visible on the relevant client, without outgoing echo. |
| Clear or delete the last turn, restart, then edit | Shared-chat eligibility remains based on existing counters. Deleted-source chat note copies remain omitted. |
| Own D1 table missing with local pending work | Sending stops first. Desktop explicit recovery and daemon startup recreation follow their separate rules; daemon recreation retains local work. |
| R2 new download/replacement and desktop local vault creation from another vault | Queue/flags are clean before use; content/credentials remain. Protected replacement works; unsupported passwordless protected-new-download remains refused. |
| Switch, failed maintenance, and disconnect | No Worker uses an old database/password or a cleared/replaced queue; offline disconnect succeeds locally. |
| Final send takes longer than 10 seconds | Both native hosts wait for the full attempt. A failure ends sending and preserves the remaining queue on reopen. No overall drain timer is added. |

Confirm that audio and event history are absent from change records, ordinary
API/R2 keys are included, and D1 credential changes do not emit records.
Exactly-once delivery and reconciliation of accepted collisions are not
acceptance conditions.

The existing daemon has no Settings screen. Establish its test credentials
through the prepared disposable vault/backend setup; do not add a production
configuration or copy UI merely to make an acceptance test convenient.

## 7. Build, platform, and live checks

Run the repository's normal macOS regression path after focused checks pass:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make web-check
make itest-local
```

On Linux, run `make test-daemon` and `make itest-daemon`. On Windows, follow the
repository's current build/package instructions and run available CTest and
frontend checks; `make itest-local` is not supported there. Exercise Settings,
refresh/reconnect, and final-send shutdown in both native hosts when available.
Record any unavailable platform and the exact unperformed checks. Do not claim
cross-platform execution from a local build alone.

Keep live D1 acceptance separate and opt-in, using a designated disposable
database and temporary vaults. Use the recorded block 1 feasibility results;
repeat a probe only if a changed assumption or failure warrants it. Do not run
the broad live-provider/R2 suite merely to test D1. Never expose credentials or
use real vault queues for size-limit/eviction tests. If access is missing, ask
for the missing resource and continue independent validation; record the
remaining live check as incomplete.

## 8. Completion and final handoff

The feature is implemented when every record kind, application lifetime path,
approved UI operation, and required refresh behavior works through production
code, and the relevant checks pass. No required code may be left as a test stub.
Unperformed platform/live checks are explicit validation limits, not passes.
Resolve actual failures before declaring completion.

Review the complete feature diff for unnecessary state and machinery. Confirm
that no cleanup flag, slash-vault parser, chat-reservation batching, extra save
protocol, or Linux delivery feature was introduced. Remove temporary probe
artifacts and unused scaffolding. Preserve unrelated user changes.

Fill in the execution record below. In the final response, report what works,
the validation performed, platform/live limits, and any remaining blocker.
Link the changed files or relevant code revision. No fourth implementation
block is planned; integration fixes belong in this session unless a real
external blocker prevents completion.

### Execution record — to be filled by the implementing session

- Block 3 implementation is complete in the working tree based on HEAD
  `b2d3ae2b`. The full-capacity D1 probe and cross-platform acceptance
  remain open.
- The native bridge exposes D1 credential get, save, and disconnect through
  the application operations. The DTO and generated client types carry token
  presence. Save can recreate a missing queue with unchanged credentials.
  Save and disconnect use D1's transport timeouts and keep their approved
  labels while disabled.
- The bridge queues one `notes.changed` event per affected receive group and
  filters events from earlier vault epochs. The desktop refreshes the note
  tree and reloads the open note through `noteRequest` without changing the
  active split panel. The existing save hook discards pending edits when
  it replaces the editor. ChaWeb reloads notes on reopen.
- Desktop chat uses its existing `reloading` recovery. ChaWeb uses the same
  read timer for the 30-second idle delay and 1-second reply delay, pauses
  while hidden, and reads immediately on return. Tests drive both clients
  through a failed reservation-time request and recovery.
- Scripted D1 acceptance includes two independent temporary vaults on
  different machine names, with a plain run and a protected run. The sender
  writes and closes, and the receiver opens with the saved note, API and R2
  keys, and chat turn. The receiver produces no outgoing echo. A loopback
  R2 test verifies refusal while D1 is unavailable and a successful upload
  after the pending records are sent. The 11-second shutdown test and the
  failed-send queue retention test ran at the application layer.
- Blocks 1–2 tests cover the storage, import/merge, collision, piece handling,
  chat eligibility and counters, protected vaults, queue recovery, and R2
  delivery cleanup paths. No production Linux delivery path or second
  polling scheduler was added.
- macOS arm64 validation includes schema generation and checks, TypeScript,
  1,715 frontend tests, 24 local integration tests, and 15 daemon HTTP
  integration tests. CTest passed 1,782 tests and skipped two opt-in provider
  tests on a full rerun at `-j4`. The initial `-j8` run had two transient
  session-test failures that passed individually and on the full rerun.
- The D1 Settings check passed in the macOS WebKit test host at 1,200 and
  430 pixels. The broader UI smoke run timed out. The daemon tests on macOS
  exercise the shared code and nginx flow. Linux and Windows build and
  package checks remain unperformed.
- Live REST checks used the operator's disposable `cha-d1` database.
  One batch stored and returned an exact 1,900,000-byte UTF-8 value in
  three recipient tables. A batch with an invalid middle statement returned
  HTTP 400 and left both target tables unchanged. The empty queue query
  returned zero records and reported one row read.
- Live application handoffs passed with plain and password-protected vaults.
  The receiver opened with the note, managed forum folder, ordinary API and
  R2 keys, and completed chat turn from the sender. The plain run also
  round-tripped a split note and recovered a missing queue with unchanged
  credentials. The protected run verified missing-queue recovery in daemon
  mode and online disconnect. The handoffs used the production curl
  transport and sync worker on macOS. The probes removed their temporary
  tables and vaults.
- Full-capacity eviction remains unverified on Cloudflare. The account plan
  and a dedicated database for the capacity probe were unconfirmed.
  Packaged-host refresh and final send past 10 seconds remain unverified.
