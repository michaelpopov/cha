# Native WebView Bridge Implementation Plan

Implement [redesign.md](redesign.md) in the **10 blocks** below. The design defines
required behavior; this plan defines implementation order, bounded reading scope,
and acceptance checks. The end state is a macOS/Windows application with no CHA
HTTP listener, server asset delivery, or browser-facing SSE. React, C++, provider
networking, database format, and existing product behavior remain.

All blocks start **not started**. Paths describe the current repository; follow
moves recorded by earlier blocks rather than recreating old files. New target
and file names are illustrative until their creating block records actual names.

For standalone execution, provide the agent with the linked `blockN.md` brief
from the sequence table. Each brief includes its prerequisites, relevant design
contracts, steps, checks, and execution record; it does not require this plan or
the design proposal as additional instructions.

## Execution and context budget

Run one block per fresh agent context by default, continuing on the accumulated
implementation. Each block has one outcome and acceptance gate; its numbered
steps are implementation order within that block, not separate agent tasks.

Each block is scoped for a **400K-token context window**. The table gives planning
allowances for working context, including source reads and implementation. Reserve
at least 80K for final debugging, verification output, and handoff. These are
estimates, not measured guarantees; context reuse within a block is what makes
related operations cheaper than separate full investigations.

1. Read repository instructions, this section, the current block, the preceding
   handoff, and its referenced design sections. Inspect the diff and preserve
   unrelated changes, including the document rename.
2. Confirm prerequisites and platform availability. Do not count compilation on
   one OS or browser/fake-bridge tests as proof of another native host. Record
   missing required evidence as pending; do not pass the gate without it.
3. Search first and read only the listed entry points and direct dependencies.
   Avoid loading generated schemas, vendored sources, build directories, complete
   logs, or unrelated operation groups. Within a block, reuse the same contract,
   fixtures, and test commands across related operations.
4. Implement numbered steps in buildable increments. Move existing behavior and
   assertions instead of replacing them. Run focused checks after each meaningful
   change, then the block's integration checks once the pieces work together.
5. Review the diff for scope, ownership, duplicated policy, and transport leakage.
   Update the inventory and handoff below with exact commands and results.
6. If occupied context approaches 320K before completion, checkpoint a buildable
   step and record its precise continuation. Resume the same block rather than
   creating more blocks or silently treating a context boundary as acceptance.

Keep a reviewable diff or checkpoint commit according to the user's workflow;
separate branches or pull requests for each block are not required.

### Rules throughout the migration

- Keep the HTTP application usable during coexistence. Select HTTP or native
  mode explicitly at launch; never fall back to HTTP for an unsupported native
  operation or replay a mutation after timeout. An incomplete native mode is a
  development milestone, not a release replacement.
- Use ordinary functions and existing owners. Do not add a generic service/RPC
  framework, event bus, reflection router, replay ledger, or scheduler architecture.
- Preserve the owner-thread invariant, semantic validation, public errors,
  product limits, immutable workspace publication, leases, SQLCipher, mirroring,
  and final persistence. Native UI callbacks must not block on application work.
- Move behavioral assertions with their implementation. Delete HTTP framing tests
  only after accounting for useful behavior embedded in the same suites.
- Retain outbound provider HTTP/SSE, R2, and deterministic provider test servers.
  They are distinct from CHA's internal application server.
- Use temporary vaults and dedicated test resources. Live provider/R2 checks need
  explicitly configured test credentials/resources, never a user's working vault
  or an ordinary cloud object.
- Update this plan's records as work proceeds. Do not casually rewrite the design
  or unrelated documentation; record a necessary design deviation before work
  depending on it.

## Sequence and gates

Execute in order. Each block requires the preceding blocks to have passed.
Platform checks can be run on their respective machines against the same revision;
keep their results in one handoff rather than repeating common implementation.

| Block | Outcome | Required execution environment | Working-context allowance |
|---|---|---|---|
| [01](block1.md) | Inventory and both-platform feasibility | macOS and Windows/WebView2 | 220–300K |
| [02](block2.md) | Listener-free Application and session engine | Native compiler + Node | 250–320K |
| [03](block3.md) | Common bridge and native frontend | Native compiler + Node | 220–300K |
| [04](block4.md) | Both native hosts and lifetime/concurrency proof | Both hosts + native tests | 240–320K |
| [05](block5.md) | Maintenance, vaults, transfers, and native actions | Native compiler + both hosts | 240–320K |
| [06](block6.md) | Complete session and workspace operations | Native compiler + Node + host runners | 220–300K |
| [07](block7.md) | Providers, credentials, OAuth, and settings | Native compiler + Node + both hosts | 180–280K |
| [08](block8.md) | Local resources, speech, audio jobs, and voice input | Both hosts + deterministic providers/microphones | 250–320K |
| [09](block9.md) | Final contract, development workflow, and package parity | Both hosts + Node | 220–300K |
| [10](block10.md) | Remove HTTP/SSE and verify simplification | Both hosts | 200–300K |

The principal gates are:

- **After 01:** packaged origins, microphone/media behavior, and real native
  automation work on both platforms. No broad extraction before this proof.
- **After 04:** both native hosts run create/open/submit/stream/Stop/reload, and
  document replacement, delivery bounds, actor retirement, and shutdown pass.
- **After 05:** all subsequent operations can use a proven maintenance/context
  gate; native menus and React share lifecycle policy.
- **After 09:** every retained operation and package/upgrade assertion has
  replacement coverage. Only then remove the old transport.
- **After 10:** neither production nor package verification needs `chaweb`, and
  the net code/dependency/configuration changes are measured.

## Verification commands

Existing commands, from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure

npm --prefix webapp run check
npm --prefix webapp run build
npm --prefix webapp run e2e

cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --test-dir build/asan-ubsan --output-on-failure
```

Use Node/npm versions from `webapp/package.json` and `webapp/.node-version`.
Install locked dependencies with `npm ci` in `webapp` when needed. List native
tests with `ctest --test-dir build/ninja -N`; use `-R` for relevant discovered
names. Use the `tsan` preset for focused concurrency checks where supported,
recording limitations rather than weakening assertions or adding broad suppressions.

Current native targets include `cha_tests`, `cha_web_tests`,
`cha_web_stress_tests`, and conditionally `cha_web_process_tests`. Record the new
app/bridge test names as tests move. The existing `e2e` command starts `chaweb`;
Block 01 establishes native runners and Block 09 finalizes their entry points.

Existing package commands are:

```sh
./packaging/macos/package.sh <test-version> <temporary-output-parent>
```

```powershell
./packaging/windows/package.ps1 -Version <test-version> -OutputParent <temporary-output-parent>
```

Use an actual test version and isolated output directory. Read the scripts before
running them; their current server-based verification is replaced in Block 09.
Do not bypass a failing package assertion by deleting it. Preserve same-revision
passing evidence and rerun checks only for new changes, failures, or missing
coverage. Record commands, platform/runtime versions, results, and concise logs.

## Block 01 — Inventory and prove both native platforms

**Read:** redesign Sections 1–4, 12–15, and 18–20; `CMakeLists.txt`, `Makefile`,
`src/web/application_runtime.*`, route installers, `src/web_main.cpp`,
`webapp/src/api/{client,events}.ts`, `webapp/playwright.config.ts`, both desktop
hosts/package scripts, and `webapp/src/{voiceInput,textToSpeech}.ts`.

**Steps**

1. Record the starting revision, dirty paths, baseline test results, supported
   OS/runtime versions, and native unlock/menu behavior. Establish a repeatable
   code-size baseline separating handwritten production, generated code, tests,
   packaging, and documentation; exclude vendored/build output and record the
   exact file set/command for Block 10.
2. Populate the operation inventory below from actual client methods, installed
   routes, direct frontend fetch/resource/download/file use, and native runtime/
   menu entry points. Record result/error, cancellation/context behavior,
   destination block, and retained verification for every entry.
3. Classify affected source and test files as keep/move/split/delete. Include
   useful behavior in `protocol`, `route_support`, `sse_mailbox`, shutdown,
   runtime, and audio files. Map each E2E/package/upgrade assertion to a native,
   common, browser/fake-bridge, or explicit transport-only destination. Retain
   deterministic provider setup independently of its CHA server launcher.
4. Capture representative wire examples from real C++ serializers: bootstrap,
   snapshots, text/reasoning targets, CommandResult, entity DTOs, and errors.
   Establish a small check against frontend expectations; do not hand-maintain
   two independent fixture sets or include real secrets.
5. Add an explicit non-shipping feasibility mode to each existing host. It loads
   built React assets without starting ApplicationRuntime's listener. On macOS,
   prove the restricted `WKURLSchemeHandler` candidate using public APIs; consider
   restricted file loading only if necessary. On Windows, use HTTPS virtual-host
   folder mapping for immutable assets and controlled dynamic resource handling.
6. In both hosts prove module paths, missing-asset behavior, used storage APIs,
   fragment navigation/back/forward/reload, and effective CSP before page scripts.
   Verify blocked script/resource/frame/navigation cases; copying HTML does not
   preserve the current HTTP CSP header. Keep only the proven loader choices.
7. Install an early test receiver and prove a native/JavaScript round trip with
   safe structured data, including script-looking strings. Validate actual
   main-frame/document trust and deny remote navigation/popups. This is a small
   probe, not the final dispatcher.
8. Prove microphone permission/capture and WebRTC setup with the existing voice
   protocol and native credential-bearing exchange. Also prove local audio
   fetch, Blob/object-URL playback, seek/resume, cancellation, release, and cache
   behavior. HTML loading alone does not establish fetch or microphone support.
9. Establish native automation: a small macOS test-host runner using public
   WKWebView JavaScript evaluation for DOM actions and bounded assertion polling;
   a Windows Playwright fixture attaching through `chromium.connectOverCDP` to
   a test-only port with isolated user data. See the
   [WebView2 automation guide](https://playwright.dev/docs/webview2). Each runner
   must demonstrate a passing assertion and a failing exit on assertion/timeout.
10. Run the probes on supported platform versions, including the minimum macOS
    version. Separate deterministic setup tests from real microphone/provider
    evidence. Verify ordinary release launch enables no test/debug entry points.
    Record exact build/runner commands and discard unused spike alternatives.

**Acceptance:** the inventory/baseline exists and both platforms pass origin,
voice/media, trust/CSP, and native-automation probes. Missing real-platform or
provider evidence remains pending. A feasibility failure blocks broad extraction;
do not disable voice, use private APIs, or keep production HTTP to pass this gate.

**Handoff:** inventory, baseline/failures, wire fixtures, chosen loaders/public
APIs, platform evidence, runner commands, and probe code to reuse or remove.

## Block 02 — Extract Application and the session engine

**Read:** redesign Sections 2, 4–10, and 16–18; `src/web/{application_runtime,
application_config,web_settings,protocol,json,current_vault}.*`,
`src/web/{live_session,live_session_manager,command_queue,owner_wake_signal,
browser_connection_state,sse_mailbox,sse_stream,session_projection}.*`, bootstrap
helpers, `server_shutdown.*`, and their focused runtime/owner/mailbox tests.

**Steps**

1. Establish app/bridge build boundaries without introducing empty service
   classes. Separate retained DTOs/domain errors and serializers from SSE-only
   payloads and HTTP status mapping. Keep old adapters using shared definitions
   and preserve OpenAPI type-generation/checks during coexistence.
2. Separate application options from server options. Preserve meaningful session,
   queue, prompt, command, and shutdown limits. Native configuration ignores
   obsolete `[web]` values with warnings, including wrongly typed unused fields;
   validate server settings only in explicitly selected temporary HTTP mode.
3. Move actor/manager/command ownership into the application boundary. Keep one
   owner per controller, the manager-to-actor lock direction, bounded admission,
   and manager-owned joins. Move browser attach/disconnect policy to the temporary
   adapter; native lifetime must not depend on orphan timers or SSE drain waits.
4. Add asynchronous owner-command completion with the actual CommandResult when
   the short command executes, before generation finishes. Preserve notices,
   draft rules, queue limits, and unknown timeout outcomes. Only a temporary HTTP
   worker may synchronously wait; native execution must not occupy a pool waiting
   for owner replies.
5. Implement selected-session ownership and retirement: keep the selected actor
   over reload; retire deselected idle actors; let existing background generation
   finish, then retire unless reselected. Coordinate selection/open/deletion,
   preserve selection on failed open, reap eligible idle actors before limit
   errors, count starting/stopping actors, and never silently cancel busy actors.
6. Introduce the narrow owner-output seam and extract mailbox merging, snapshot
   fallback, and one-in-flight behavior with their tests. Leave SSE writer,
   heartbeat, takeover, and final-drain policy only in the legacy adapter.
7. Atomically establish subscriptions and initial snapshots on the owner. Scope
   output by connection/context/subscription/full session identity. Assign native
   sequence numbers after coalescing when output is committed: initial snapshot
   zero, one shared sequence for snapshots/appends, no reset on later snapshots.
8. Bound pending append bytes; on incompatible/oversized updates mark the
   projection dirty and capture fresh state on the owner when delivery resumes.
   Preserve the snapshot/following-append boundary and capture terminal state
   before owner destruction. Use consumer-driven draining, one scheduled drain,
   and no periodic timer. Preserve old SSE sequence semantics in its adapter.
9. Split Application composition from the optional HTTP runtime: workspace,
   repository, providers, credentials, actors, audio dependencies, and mirroring
   open without a server, port, cookie, or listener thread. Move existing vault/
   transfer implementations with their ownership, leaving native bindings for
   Block 05. Running state must not depend on binding a port.
10. Extract the minimal bootstrap/session create/open/submit/stop/snapshot/close
    operations for the first native flow. Provide selected-session state and an
    initial context epoch; check captured context under the lifecycle guard.
    Preserve protected-vault discovery/unlock, semantic validation, and errors.
11. Establish safe ordinary shutdown and early-failure cleanup with correct
    owner/provider/store dependency order and final persistence. Expose nonblocking
    host shutdown initiation; Block 04 stress-tests it before wider migration.
12. Run headless Application tests, reused owner/mailbox/manager/projection tests,
    and affected HTTP regression tests. Add races for initial snapshot, terminal
    capture, reselect/retire, async completion, deletion, and visiting more idle
    sessions than the actor limit. Verify obsolete native configuration handling.

**Acceptance:** headless Application creates/opens, submits/stops, snapshots,
persists, and shuts down without a listener. App/core has no inbound HTTP or UI
API dependency. Native event/retirement invariants pass; legacy HTTP still uses
the same domain behavior through temporary adapters.

**Handoff:** actual targets/types/APIs, ownership and completion rules, configuration
split, event sequence/bounds, focused test commands, and temporary compatibility
code whose removal belongs to Block 10.

## Block 03 — Implement the bridge and native frontend

**Read:** redesign Sections 4, 7–12, and 15–16; extracted app/serializer/mailbox
APIs, `webapp/src/api/{client,events,guards}.ts`,
`webapp/src/components/{App,ChatScreen}.tsx`,
`webapp/src/state/{route,sessionRecovery,bootstrap,view}.ts`, and focused tests.

**Steps**

1. Define fixed native request/reply/error/event envelopes and exact
   `bridge.info` version checking. Add envelope types to contract checks while
   keeping existing authoritative DTOs and real C++ serializer fixtures.
2. Create native-owned document connection state. Validate envelopes, method
   allowlists, required/unknown fields, safe numbers, IDs, encoded sizes, parameter
   shapes, and context before effects. Reject duplicate outstanding request IDs;
   a matching JSON connection ID does not establish actual sender trust.
3. Bind only the first-flow operations from Block 02. Decode, call Application,
   and encode approved results/errors. Preserve CommandResult and timeout meaning;
   do not acknowledge mere queue admission or automatically replay mutations.
4. Bound admitted calls and ordinary work. Preserve separate admission for Stop,
   unsubscribe, acknowledgement, disposal, and shutdown. Do not block session
   completion behind provider work. An over-limit sender gets bounded reporting/
   invalidation rather than an unbounded queue of rejection replies.
5. Retain reply capacity until consumption or connection invalidation. Batch
   replies, bounded session output, and coalesced lifecycle notices through one
   outstanding native-to-JS delivery. Use connection/batch-scoped consumption
   acknowledgement outside RPC admission, with no retransmission/heartbeat ladder.
6. Track the latest requested subscription before owner work is enqueued. Guard
   all completions with checked lifetimes; delayed subscribe/unsubscribe cannot
   replace or remove a newer subscription. No raw WebView pointer enters common
   application/bridge work.
7. Inject a native `ChaClient` and separate `SessionEventsConnector` at frontend
   startup. Keep platform globals out of React. Install the receiver early,
   correlate promises, check real serialized data, and reject local promises on
   disposal; `invoke<T>` casts are not runtime validation.
8. Allocate subscription IDs/install handlers before subscribe. Accept initial
   snapshot and subscribe reply in either order. Discard stale/duplicate scoped
   output, require consecutive appends, and allow valid newer snapshots to set
   the baseline. After a gap/unknown target, invalidate that subscription and
   replace it; do not splice a standalone snapshot into the live stream.
9. Acknowledge delivered batches in a finally path after synchronous dispatch,
   including stale messages; receiver failure triggers recovery without holding
   the slot forever. Preserve StrictMode cleanup and stale-navigation guards.
10. Use fragment-only native routes with the existing validated grammar. Update
    initial parsing, all history writes, Welcome resets, and back/forward; a hash
    change keeps the same connection. Retain old path routing/network recovery
    only in temporary HTTP mode.
11. Mark unmigrated native capabilities explicitly unavailable in development,
    never silently successful or forwarded to HTTP. Add common bridge/client/
    projection/component tests for malformed input, scope/sequence/ID races,
    timeout/late completion, saturation, and stalled acknowledgement; run schema,
    TypeScript, and frontend build checks.

**Acceptance:** common bridge and native client/projection work with bounded
admission/delivery and verified C++ wire examples without platform libraries.
Native routing/recovery tests pass while the temporary HTTP frontend still works.

**Handoff:** protocol version/contracts, limits, injection points, callbacks/
acknowledgements, native capability gaps, and exact focused test commands.

## Block 04 — Integrate both hosts and prove lifetime behavior

**Read:** redesign Sections 6–12, 15, 17, and 20; Block 01's probes/runners,
`packaging/macos/main.swift`, `packaging/macos/runtime_bridge.{h,cpp}`,
`packaging/windows/main.cpp`, and the common APIs from Blocks 02–03.

**Steps**

1. Replace probe messaging with the shared Application/dispatcher, first on
   macOS and then Windows. Extend the existing C boundary with explicit string,
   callback, and handle ownership. Windows currently also compiles the runtime
   bridge under `packaging/macos`; move shared code once if useful, never fork it.
2. Preserve native protected-vault prompting, early errors, and runtime checks.
   Start native Application and packaged assets without port/cookie setup; keep
   temporary HTTP launch explicitly available for migration verification.
3. Marshal WebKit/WebView2 calls to the UI thread and application work off it.
   Install the receiver before React, validate real sender/main-document scope,
   and block unauthorized navigation/popups. Use structured JSON/arguments for
   delivery, never interpolated executable strings.
4. Bind each real document connection, bounded delivery, and acknowledgement.
   WebView2 post success is not consumption. Invalidate old deliveries before
   replacement and guard queued callbacks after view disposal. Hash navigation
   does not create a new document connection.
5. Wire WK process termination and WebView2 `ProcessFailed` to document recovery
   while preserving Application/selected actors. Limit repeated failure to an
   actionable reload/restart state instead of an endless retry loop.
6. Extend each runner to actual UI create/open/submit/visible-stream/Stop/reload/
   resubscribe/restart flows with temporary vaults and the deterministic provider.
   Assert transcript and persisted state; page-load success is insufficient.
7. Exercise subscribe/snapshot races, dirty snapshot replacement, gaps/duplicates,
   delayed subscription cleanup, and StrictMode. Compare final projection with
   authoritative state using deterministic scheduling controls, not arbitrary sleeps.
8. Stall JS consumption while providers produce bursts. Assert bounded pending
   bytes/calls/drains, one renderer delivery outstanding, continued owner/persistence
   progress, and responsive Stop/disposal under ordinary-work saturation.
9. Reload with requests pending, reuse numeric IDs in the new connection, and
   deliver old replies/acks. Test actual renderer failure and view disposal on
   both hosts; assert no replay, cross-document resolution, or destroyed-object
   access. Recheck retirement/lease release after repeated navigation.
10. Quit with generation, queued work, providers, and stalled presentation active.
    Use one overall grace, no UI-thread joins, persistence after state settles,
    and safe provider/store destruction. Preserve controlled failure when a worker
    cannot finish; never detach work that still accesses destroyed objects.
11. Measure long-conversation snapshot capture/serialization/transfer/render and
    Stop latency. Fix demonstrated unbounded work; do not add truncation,
    pagination, or timers without evidence. Run focused race/sanitizer checks and
    record both host results, keeping automation hooks out of shipping builds.

**Acceptance:** both hosts pass the complete first native flow and lifetime/
saturation/shutdown tests without a CHA listener. Actors, persistence, and quit
never wait for renderer acknowledgement. Subsequent blocks extend these same
checks to their new maintenance, provider, and media work.

**Handoff:** ABI/host ownership, runner evidence, reproducible races, measurements,
sanitizer results, and remaining operation capabilities to migrate.

## Block 05 — Maintenance, vaults, transfers, and native actions

**Read:** redesign Sections 5, 8, 14, and 17; retained runtime/vault code,
`src/web/{vault_routes,r2_database_transfer}.*`, workspace/repository maintenance
and manager guards, both hosts' menus/dialogs, and runtime/transfer/upgrade tests.

**Steps**

1. Establish running/maintenance/stopping/unavailable states and one gate shared
   by bridge calls, native menus, and temporary HTTP operations. Do not duplicate
   lifecycle policy in each host.
2. Bind admitted work to its context epoch. Share exclusion between final context
   validation and mutation/resource acquisition; checking then using an unguarded
   pointer is insufficient. Reject conflicting new work instead of deferring it
   to whichever vault is current later.
3. Reserve maintenance and invalidate queued old-context work, subscriptions,
   resource access, and presentation. Settle/cancel executing users before closing
   handles. Preserve store-before-repository lock order, owner reservations,
   checkpoint, close/reopen, workspace publication, and error precedence.
4. Publish a fresh epoch after safe reopen, even for recovery in the same vault.
   If reopening fails, enter unavailable while bootstrap/status/quit work without
   touching closed handles. Keep existing audio pause/cancel hooks and provide
   the narrow invalidation hook Block 08 will use.
5. Complete the initiating maintenance call despite its old-context invalidation,
   reporting the resulting epoch/state. Coalesce context-change notifications
   for menu actions. Frontend rejects stale promises, clears scoped state, stops
   playback/capture when present, then bootstraps/reopens; a full reload is fine.
6. Migrate vault list/create/update/delete/switch/merge and protection/password
   operations. Preserve source-vault errors, registry/path persistence, mirroring,
   and session restoration. Do not change the database format.
7. Migrate R2 vault listing/download, database upload/download, and configuration
   import/export through existing native implementations. Preserve configured
   directories, transfer deadlines, safe replacement/cleanup, and reopen behavior.
   R2 settings UI migrates in Block 07; use configured test fixtures here.
8. Connect native menu capability/enabled state to Application. Run blocking
   operations asynchronously, with the same validation and maintenance gate as
   React. No second host-specific password/transfer policy.
9. Implement controlled native open/save actions where user selection is required.
   Open dialogs on the UI thread, revalidate context after selection, cancel
   without effects, and preserve temporary-file/atomic-replace behavior. Do not
   expose arbitrary JS filesystem paths or hold database locks during dialogs.
10. Account for existing file inputs/downloads. Preserve small bounded text uploads
    and verify the WK open-panel delegate and Windows picker. Provide the native
    save action that session export will use in Block 06; large exports must not
    become base64 RPC. Audio resource/read behavior follows in Block 08.
11. Test queued session/settings work across switch-away/back and overlapping IDs,
    failure before/after handle closure, failed reopen, self-completion of the
    initiating call, menu/UI agreement, and quit during maintenance/dialogs.
    Add automated vault switching and persisted-state restoration in both hosts.
12. Run protected-startup, wrong-password/cancel, real native-dialog, transfer,
    and upgrade checks with temporary data; restrict live R2 verification to
    dedicated test objects. Record pending dependent UI bindings explicitly.

**Acceptance:** the shared maintenance/context gate and vault/transfer/menu
operations pass common and real-host checks. No stale work acts on a retargeted
or closed database. Native file actions are available for subsequent callers;
remaining session/media bindings are explicitly assigned to Blocks 06/08.

**Handoff:** lock/admission protocol, operation coverage, dialog/transfer evidence,
failure-injection tests, context handling, and reusable native save API.

## Operation migration procedure for Blocks 06–08

For each operation: extract existing domain behavior; make the temporary route
call it; add fixed bridge decoding/serialization and the typed client/action;
update authoritative types, critical guards, and real C++ fixtures together;
then migrate the caller and retained tests. Preserve canonical results, context
checks, semantic constraints, errors, and immediate UI updates. This procedure
does not require a separate class or harness per operation group.

## Block 06 — Complete session and workspace operations

**Read:** redesign Sections 4, 6–9, 14, and 16; `src/web/{lobby_routes,
session_routes,text_input,text_command,text_mention,text_multicast,
session_markdown}.*`, `src/workspace` editing APIs, session/character/persona/forum
client methods and UI, and associated domain/component/E2E tests.

**Steps**

1. Complete session list/create/rename/delete/open/close and standalone snapshot
   reads. Preserve labels, Recent ordering, Welcome behavior, canonical IDs, and
   running state. Reserve deleted identities and release journal leases before
   removing stored data.
2. Migrate cover/uncover, delete-turn, default-character, and all submit/input
   commands. Preserve mentions, multicast, persona attribution, reasoning,
   notices, draft-clearing, and provider/cancellation transitions. Use full
   snapshots for structural changes that lack a safe append representation.
3. Keep sidebar/catalog/selection truthful after rename/delete and rapid navigation;
   late results cannot restore a deleted/unselected session. Extract session-export
   content and wire it to Block 05's save action, retaining native dialog/error
   behavior without exposing arbitrary paths.
4. Migrate character/persona get/create/update/delete and character-definition
   edits. Preserve generated IDs, reserved/built-in/read-only rules, canonical
   settings, descriptions, and reference/use restrictions.
5. Migrate character Markdown file CRUD and persona Markdown replacement. Retain
   traversal/filename checks, allowed files, encoded/text-size limits, and public
   validation errors. Keep appropriate small text content in bounded DTOs.
6. Migrate forum get/create/update/delete, forum-file CRUD, and membership/defaults
   edits. Preserve reference validation, overrides/order, built-in constraints,
   and selected/recent-session behavior when definitions change.
7. Use WorkspaceConfigStore's existing materialize/validate/transaction/publish
   path for every edit. Never mutate the imported source directory or publish
   partially validated state. Preserve affected-session invalidation, including
   actors still opening, through application operations rather than route helpers.
8. Bind typed clients/forms/file actions and canonical results to immediate roster,
   sidebar, and detail updates. Use the native picker/save path already tested;
   do not add a generic filesystem API or duplicate HTTP/native business rules.
9. Move validation/storage/rollback/reference/controller assertions to application
   tests and retain focused wire/client/UI checks. Automate native create/reload,
   rename/delete, rapid Recent navigation, streamed commands, workspace/member/file
   edits, failed validation, and restart persistence. Preserve useful assertions
   from old takeover/reconnect tests in the new native lifetime tests.

**Acceptance:** all session, character, persona, forum, membership, and text-file
operations work natively, including export. Constraints, transaction rollback,
active-session effects, immediate UI updates, and persistence retain coverage;
no domain behavior remains solely in these HTTP handlers.

**Handoff:** completed operation/assertion rows, tests, native-flow evidence,
and temporary route/recovery code ready for deletion in Block 10.

## Block 07 — Providers, credentials, OAuth, and settings

**Read:** redesign Sections 8, 13–16; `src/web/{settings_routes,
openai_auth_routes}.*`, native provider/credential/OAuth APIs, settings client/UI,
voice-settings reload handling, and their tests.

**Steps**

1. Migrate provider list/get/create/update/delete with canonical editable config,
   provider-specific validation, and reference/use restrictions.
2. Run provider tests on background provider work, preserving deadlines,
   diagnostics, and public errors without blocking UI callbacks or session Stop.
   Keep completion/context/shutdown rules from earlier blocks.
3. Migrate API-key metadata/create/rename/replace/delete. Native reads expose
   identifiers/presence only; secret writes are explicit, UI buffers are cleared,
   and response fixtures/logs never contain real credential values.
4. Migrate R2 settings get/save/delete with existing references/configuration
   rules, then verify the transfer UI against Block 05's operations.
5. Migrate OAuth status/start/poll/disconnect through the existing provider flow.
   Open approved authorization URLs externally, preserve verification details,
   and never expose the bridge to an embedded login page. Tokens remain native.
6. Migrate style/voice CRUD with defaults, provider voice IDs, references, and
   use restrictions. Preserve appearance publication and affected-session updates.
7. Migrate voice-input/output settings and non-secret runtime capabilities. Native
   runtime DTOs must not return stored keys; later setup/synthesis resolves them
   natively. Keep incomplete media execution explicitly unavailable until Block 08.
8. Update schemas, fixtures, guards, typed clients, and forms together. Preserve
   canonical errors, immediate visible changes, and persistence without route-
   specific policy in application code.
9. Extend context/quit/saturation tests to provider tests and OAuth: old document/
   vault completions cannot publish current status/secrets, and slow provider work
   cannot starve Stop. A lost reply does not imply an operation was rolled back.
10. Port behavior tests and exercise native settings/appearance/credential/R2 UI,
    authorization links, and reload/restart. Use deterministic provider responses
    normally and record dedicated live-account evidence separately.

**Acceptance:** provider, credential, OAuth, R2, appearance, and voice configuration
operations work natively with preserved constraints; stored secrets remain native
and provider work respects lifetime/control admission. Only media execution
operations remain for Block 08.

**Handoff:** bindings and tests, secret-boundary evidence, provider execution/
cancellation rules, external-link checks, and final media runtime DTOs.

## Block 08 — Resources, speech, audio jobs, and voice input

**Read:** redesign Sections 8, 12–13, and 15; Block 01's proven loaders/media
exchange, `src/web/{fish_audio,audio_download,audio_download_routes}.*`,
`webapp/src/{textToSpeech,audioDownloads,voiceInput}.ts`, existing media tests,
and the context/shutdown/native file APIs from earlier blocks.

**Steps**

1. Add the small scoped resource layer over existing readers. Handles identify
   registered resource kinds and validate connection/context/session/vault
   ownership; never accept arbitrary JS paths. Keep it local, without a general
   file server or durable registry.
2. Connect proven WK/WebView2 resource handlers to application bytes/metadata.
   Keep platform tasks out of common code, set MIME/length, and cancel stopped
   reads. Guard late callbacks and prevent resource readers retaining database
   handles across maintenance or view destruction.
3. Revoke on release, context change, deletion/cache clearing, and document loss.
   Enforce verified no-store/private-cache behavior, clean temporary plaintext
   where needed, and keep user-controlled HTML outside the privileged origin.
   Notify/clear copied frontend Blobs too; native revocation cannot revoke a copy.
4. Add the typed binary-resource fetch helper using the proven origin/CSP rules.
   Preserve whole-resource Blob/object-URL playback and seek/resume. Large bytes
   stay outside JSON/base64 RPC; implement ranges only if recorded platform or
   performance evidence requires a different media path.
5. Extract uncached synthesis/preview and bind speech start/cancel/release.
   Cancellation names the original pending bridge request before a resource
   exists. Prevent cancelled queued work starting, cancel executing provider work,
   and release late results without playback. Preserve provider limits/deadlines.
6. Preserve HTMLAudioElement position, pause/resume, end/error, AbortSignal,
   object-URL revocation, and native resource release. Clean reads/playback/Blob
   references on stop, disposal, context change, and invalidation.
7. Migrate persistent single/batch audio generation, status/errors, cache source/
   lookup, cached-entry updates, cache clearing, and related download/save actions.
   Preserve application-owned job IDs; reload reattaches without resubmitting.
   Keep playback stop, preview cancellation, and cache-job cancellation distinct.
8. Implement one native `voiceInput.connect` using the existing provider protocol.
   Accept a bounded setup description, choose destination/settings/credential
   natively, and return the answer without exposing stored keys. Replace the
   probe implementation rather than keeping duplicate setup paths.
9. Retain WebView microphone capture, peer/data channel, transcription normalization,
   and current settings. Bind cancellation to connection/request identity before
   dispatch, during setup, and after reply/context change; a late answer cannot
   attach to a newer peer. Close tracks/peer/channel on every terminal path.
10. Remove native frontend credential-bearing setup fetches and server audio URL
    construction. Retain only explicitly isolated legacy HTTP definitions until
    Block 10. Keep permission denial, unavailable capture, device/provider failure,
    and cancellation actionable without switching protocols or rewriting media.
11. Extend shared race/context/shutdown tests to synthesis, persistent audio jobs,
    loaded-cache deletion, resource reads, setup, and pending playback. Test
    unknown/stale handles, traversal/untrusted requests, late completion, and
    cancellation at every request/resource/playback boundary.
12. Exercise uncached/cached/batch playback, seek/resume, native saves, real microphone/
    transcription, permission denial, cancellation, reload, and quit in both
    hosts. Separate deterministic provider tests from real-device/provider proof;
    missing required platform evidence keeps the block pending.

**Acceptance:** full existing voice-input/output, cache, playback, and media
file behavior works in both native hosts without CHA HTTP or binary JSON payloads.
Credentials remain native, resources are revocable, and new work types pass
maintenance/shutdown tests without changing persistent-job semantics.

**Handoff:** media operation coverage, resource ownership/CSP/cache behavior,
provider/capture evidence, cancellation tests, and obsolete helpers for Block 10.

## Block 09 — Final contract, development workflow, and package parity

**Read:** redesign Sections 4, 12, and 16–20; `resources/cha.yaml`, generated types,
package/Vite/Playwright/staging scripts, desktop hosts/package metadata,
`packaging/macos/runtime-smoke.c`, shared seed/configuration assets, and the
completed operation/assertion inventory.

**Steps**

1. Close every native placeholder/missing binding by following actual callers in
   the inventory. An operation is implemented and verified or an explained pure
   transport deletion; no hidden HTTP fallback or fake successful response.
2. Move retained DTO/envelope definitions to one authoritative schema independent
   of HTTP routes. Reuse existing tooling with a minimal format wrapper if
   required; keep C++ serializers checked against generated frontend contracts.
   Have the temporary HTTP catalog reference shared definitions rather than copy
   them. No new dual-language generator framework is needed.
3. Update generation/check scripts/imports atomically. Preserve critical runtime
   guards/version checking, and demonstrate a real serializer/type mismatch
   fails the shared fixture check before restoring the correct version.
4. Finalize native development launch commands. Vite/HMR may serve assets at
   one exact allowed development origin while application calls use the bridge.
   Keep browser/fake-bridge tests and the temporary HTTP E2E command distinctly
   named; record exact common/frontend/host runner commands.
5. Verify shipping builds cannot enable automation/CDP/development-origin hooks,
   including relevant launch/environment overrides. Keep instrumented native
   test hosts separate while using the same application implementation/assets.
6. Move shared seed/config data out of locations owned by the retiring server
   package and update all consumers. Build both shipping hosts with native startup
   and app/bridge/core only; keep `chaweb` solely as a temporary migration target.
7. Update macOS packaging: direct built assets, proven loader layout, protected
   first launch, icons/signing/library/OS checks. Replace port-based runtime smoke
   assumptions and the temporary `chaweb` assembly/legacy package-check reuse with
   retained application/startup and native-host assertions.
8. Update Windows packaging: mapped built assets, shared seed/config data, manifest/
   icons/runtime checks. Remove build/copy/run requirements for `chaweb.exe` in
   package verification. Replace navigation-only smoke coverage with actual native
   assertions, using CDP only in the separate instrumented test build.
9. Exercise assembled assets with each runner and the shared app implementation;
   separately verify uninstrumented release launch with development mode disabled.
   Confirm no source/build-directory fallback or application listener/cookie is
   needed and that each package's dependency graph excludes the old runtime.
10. Run retained upgrade cases on both platforms: normal/protected startup,
    wrong password/cancel, old databases, absent/valid/obsolete `[web]`, configured
    directories, persisted edits/conversations, vault transfer, and restart.
11. Complete the full native feature/security/lifetime matrix from the design,
    including actual file dialogs, microphone/WebRTC, Blob seek, external links,
    effective CSP, resource privacy, renderer recovery, and work-active quit.
    Use automated application flows plus real-platform checks where needed.
12. Reconcile every retained E2E/package/upgrade assertion with passing replacement
    evidence. Record explicit reasons for pure transport test deletions and the
    final source/target/flag/script deletion list. Do not weaken coverage to pass.

**Acceptance:** every retained behavior, native development path, generated
contract check, and both package/upgrade verification paths works without
`chaweb`. Both uninstrumented releases pass relevant checks. Any missing platform,
voice, or package evidence prevents Block 10's transport deletion.

**Handoff:** final schema/test/dev commands, package artifacts and platform/upgrade
results, completed coverage matrix, and the checked deletion list.

## Block 10 — Remove HTTP/SSE and verify the simplification

**Read:** redesign Sections 1 and 18–21; Block 01's baseline, Block 09's deletion
list, remaining `src/web` files, `CMakeLists.txt`, `Makefile`, `src/web_main.cpp`,
frontend/staging/launch scripts, legacy server packaging, and the final target graph.

**Steps**

1. Confirm the package parity gate passed. Delete only checked transport entries
   whose useful behavior/assertions have moved; do not delete `src/web` by name.
   Remove the listener/runtime wrapper, HTTP routes/framing/status/security,
   access-cookie/Host/port/worker wiring, server executable, and launch machinery.
2. Remove browser-facing SSE writer/heartbeat/takeover/orphan/final-drain adapters
   and their temporary sequence translation. Preserve extracted owner/mailbox/
   projection behavior, consumer backpressure, cancellation, and persistence.
3. Remove frontend HTTP/EventSource adapters, network probes/reconnect ladders,
   old path routing/API URL helpers, Vite API proxy, transport selection flags,
   and legacy E2E launchers. Preserve native recovery, typed clients, local binary
   fetches, browser/fake-bridge tests, and outbound provider protocols.
4. Delete obsolete route schemas only after retained definitions/type checks no
   longer depend on them. Remove compatibility aliases/status mapping/probe code
   and redundant forwarding; keep native test harnesses and behavioral coverage.
5. Remove server-only settings from new examples/options, retaining warning-and-
   ignore compatibility for old `[web]`. Preserve unrelated user configuration
   and database format. Remove remaining temporary incomplete-native capabilities.
6. Retire legacy server release/package/staging targets and transport-only checks.
   Preserve relocated seed data, upgrade assertions, and necessary outbound-provider
   tests. Remove production `cpp-httplib` and unconditional production
   setup, retaining it only in test targets that need it.
7. Update CMake/Make/npm commands and operational READMEs. Flag stale `docs/`
   guidance outside the authorized scope. Search for removed mechanisms and
   interpret hits: outbound provider HTTP/SSE, test servers, and local resource
   URLs remain valid; an unused server fallback does not.
8. Build from fresh directories with tests enabled and disabled. Run common C++,
   frontend, affected race/sanitizer, both native integration, and both package/
   upgrade checks after deletion. Inspect shipped target
   dependencies and application runtime listeners rather than relying on caches.
9. Complete final media/maintenance/lifetime/Stop/long-conversation checks on the
   final implementation. Reuse passing same-revision evidence; rerun affected
   checks after fixes and fill missing coverage rather than repeating everything.
   Reconcile every operation, file, assertion, and pending handoff item.
10. Repeat Block 01's measurements with the same file sets/method. Report net
    handwritten production, generated code, tests/harnesses, packaging, dependency,
    and configuration changes separately. Count file moves as moves and explain
    removed mechanisms and necessary native costs; raw deleted-line churn is not
    a simplification metric. Remove demonstrated duplication without weakening
    behavior/tests or chasing an invented reduction percentage.
11. Complete the final acceptance record. Report unresolved blockers rather than
    declaring completion because one host works or the context is nearly full.
    Keep unrelated database/language/native-media/iOS/multi-window
    redesign outside this migration.

**Acceptance:** production and package verification have no internal HTTP/SSE/
`chaweb` or production `cpp-httplib` dependency. Both native packages pass,
retained behavior has evidence, temporary fallback/probe code
is gone, and the simplification report is reproducible.

**Handoff:** final implementation summary, measured before/after results,
validation/platform evidence, known limitations, and separately scoped follow-up.

## Working records

These records start empty and are populated during implementation. Keep them
concise; no separate migration-tracking application is needed.

### Operation inventory

| Current caller and implementation | Behavior/result/errors | Native replacement and block | Context/cancellation/lifetime | Retained assertion | Status/evidence |
|---|---|---|---|---|---|
| Populate in Block 01 | | | | | Not started |

### Source/test disposition

| Current path | Keep/move/split/delete | Retained behavior and destination | Owner block | Evidence before deletion |
|---|---|---|---|---|
| Populate in Block 01 | | | | |

### Coverage and platform evidence

| Existing assertion or required flow | Common test replacement | macOS evidence | Windows evidence | Transport-only deletion reason |
|---|---|---|---|---|
| Populate in Block 01 and update per block | | Pending | Pending | |

### Block handoff template

```text
Block / status: NN — not started | in progress | waiting for evidence | complete
Starting and resulting revision/checkpoint:
Files changed or moved:
Implemented behavior and key ownership/contract decisions:
Temporary compatibility code and its removal block:
Inventory/coverage rows completed:
Commands, working directory, platform/runtime, and results:
Known failures / checks not run / exact blocker:
Next unfinished numbered step if continuation is needed:
Next block and any changed prerequisites:
```

### Reusable task instruction

```text
Implement docs/blockN.md against the current checkout, replacing N with 1–10.
Use that self-contained brief and the current source. Verify its prerequisites
against actual implementation and evidence, then complete its ordered steps and
acceptance checks while preserving unrelated changes and existing behavior.
Update its execution record with exact verification evidence. Record unavailable
platform evidence as pending; do not claim that gate passed. If context approaches
the handoff threshold, checkpoint a buildable step and record the precise
continuation. Do not begin later blocks implicitly.
```

### Final acceptance record

- [ ] Every retained operation/resource/menu behavior has a native implementation.
- [ ] Both hosts pass automated application flows and actual platform media/UI checks.
- [ ] Context invalidation, bounded delivery, actor retirement, and shutdown pass.
- [ ] Generated DTO checks and real C++ wire-fixture checks remain effective.
- [ ] Both packages pass startup, protected-vault, persistence, and upgrade checks.
- [ ] Production and package verification have no internal HTTP/SSE/`chaweb` dependency.
- [ ] Necessary outbound-provider test infrastructure remains.
- [ ] Temporary fallback/probe code and obsolete server configuration are removed.
- [ ] Net code/dependency/configuration changes are measured against Block 01.
- [ ] Retained behavioral assertions were preserved rather than deleted for line savings.
