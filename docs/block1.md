# Block 1 — Inventory and prove both native platforms

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** not started. **Environment:** macOS and Windows/WebView2, with microphones and the supported minimum OS/runtime versions.

## Objective and scope

Establish the migration inventory, repeatable baseline, real wire fixtures, and working local-origin/media/automation choices on both native platforms before broad application extraction.

Implement only fixture-driven feasibility probes and test infrastructure. Keep the current product launch working. Do not migrate the application graph, implement the complete native API, or delete the HTTP server in this block.

## Prerequisites to verify before editing

- A checkout of the current CHA application with the C++ build, React frontend,
  macOS Swift host, and Windows WebView2 host. No earlier migration implementation
  is required for this block.
- Access to the supported native platforms or a way to obtain test evidence from
  those machines against the same revision. Record supported versions from the
  current package configuration; do not invent a new support range.
- Isolated test vaults and dedicated provider configuration for real voice proof.
  Missing credentials or physical platform access are missing evidence, not a
  reason to substitute a fake test and declare feasibility established.

Confirm these in the current checkout and test records. Record actual entry points
and commands if earlier work renamed them. If evidence is unavailable, perform the
relevant check when possible; otherwise keep the prerequisite and gate pending.

## Execution rules and 400K context budget

Plan for **220–300K tokens of working context** within a **400K-token window**,
reserving at least 80K for debugging, final validation, and handoff. These are
planning estimates, not measured guarantees. Search/read only relevant entry points
and direct dependencies; avoid entire generated schemas, build/vendor trees, and
complete test logs. Reuse contracts and fixtures across related operations.

If occupied context approaches 320K before completion, checkpoint a buildable
numbered step and record the exact continuation. Resume this same block in a fresh
context; do not add more blocks or waive the acceptance gate.

CHA is a personal, small application. Prefer the smallest readable change,
ordinary functions, and existing owners. Do not add a general RPC/service/event
framework, durable replay ledger, generic filesystem API, or unnecessary classes.

The migration keeps React presentation and C++ domain state. The final dependency
direction is host → bridge → app → core. The final products are macOS/WKWebView and
Windows/WebView2; Linux retains common tests, not a server release or new GUI.
Keep provider networking, controller semantics, workspace transactions/publication,
leases, SQLCipher/database format, mirroring, and final persistence unchanged.

- Read applicable `AGENTS.md` and `CLAUDE.md` instructions and inspect the current
  diff before edits. Preserve unrelated changes. Paths below are relative to the
  CHA repository root and may have moved in earlier implementation; use `rg` to
  locate the real current definitions rather than recreating old files.
- Keep the temporary HTTP application working until the final removal stage.
  Select transport explicitly at launch; never fall back for an unsupported native
  method or replay a mutation after timeout. An incomplete native mode is a
  development milestone, not a release substitute.
- Keep one owner thread per session controller and common lifecycle policy.
  UI callbacks must not block on owners, providers, stores, dialogs, or joins.
  Preserve validation, errors, limits, reference checks, and unknown-outcome semantics.
- Move behavioral assertions with code. Keep outbound provider HTTP/SSE, R2, and
  deterministic provider test listeners. They are not the application server.
- Use isolated temporary vaults and dedicated test resources. Live provider/R2
  verification needs explicit test configuration; never overwrite a user's vault
  or ordinary cloud object. Do not log secrets, prompts, or raw sensitive replies.
- Keep UI interaction/copy changes limited to what this migration needs. Do not add
  panel title labels, legends, or explanatory helper copy for obvious controls.
- Ignore unused obsolete configuration with warning logs; do not block an otherwise
  valid operation because an unused setting has a wrong value or type.
- Work in small buildable increments. When migrating related operations before
  transport removal, reuse this extraction pattern: domain function → thin legacy
  adapter → fixed native binding and typed client → schema/real serializer fixture
  → caller and behavior tests.
- A completed prerequisite must be supported by the current code and evidence,
  not merely a status label. Inspect available records or rerun focused checks.
  If a substantive prerequisite is absent, report it and do not recreate earlier
  blocks inside this one. Missing platform evidence remains pending.
- Run checks relevant to actual changes; broaden after new failures, risk, or
  incomplete coverage. A skipped platform check, fake bridge, or successful page
  load is not equivalent to a passing real native user flow.
- Implement this block only. Update its execution record with artifacts and exact
  results. Do not change unrelated documentation, publish/merge a release, or
  proceed into the next block implicitly. Keep a reviewable diff/checkpoint using
  the user's existing workflow; no branch/PR per block is required.

## Code entry points

Start with `CMakeLists.txt`, `Makefile`,
`src/web/application_runtime.*`, route installers, `src/web_main.cpp`,
`webapp/src/api/{client,events}.ts`, `webapp/playwright.config.ts`, both desktop
hosts/package scripts, and `webapp/src/{voiceInput,textToSpeech}.ts`.

Read the steps and embedded requirements below before changing behavior. Names of
new modules are illustrative; reuse the current implementation and avoid parallel
copies. Braces and `*` in paths denote small related file groups, not literal names.

## Embedded design requirements

The following requirements are included directly so this brief can be executed
alone. They constrain behavior this block implements or touches. They do **not**
expand the scope beyond the numbered steps and acceptance gate; requirements for
later capabilities describe the boundary those capabilities must eventually meet.

### Decision, Scope, and Expected Benefit

Replace CHA's internal HTTP/SSE interface with a small asynchronous native
WebView bridge. React remains the presentation layer. C++ remains responsible
for application state, domain behavior, providers, and persistence.

The supported release applications in this migration are macOS (WKWebView) and
Windows (WebView2). The standalone application becomes the only supported
deployment model. Retire the existing Linux browser/server package at cutover;
retain Linux builds for core, application, bridge, and deterministic provider
tests. A Linux desktop WebView host and an iOS product are separate projects.

The primary benefit is removing an operating model and its failure modes:
localhost listeners, access cookies, HTTP request workers, browser-facing SSE,
heartbeats, browser takeover, and network reconnect policy. Expect a moderate
net reduction in production code, not deletion of everything under `src/web`.
Application orchestration and data validation currently live there too.
Platform messaging and resource delivery replace some transport code.

The migration must demonstrate a net simplification. Record production-code,
test, dependency, and configuration changes at the final cutover. Do not count
moving files, renaming namespaces, or deleting behavior tests as simplification.
Do not make a promised line-count reduction a reason to remove necessary behavior.

Keep the implementation small:

- One fixed operation dispatcher, asynchronous replies, and an ordered session
  event channel.
- One native window and at most one selected session event subscription per
  document. No general publish/subscribe or multi-window framework.
- Reuse the existing session owner, command queue, projection, and bounded
  mailbox behavior.
- Extract application operations into ordinary functions or existing objects.
  Additional service classes are optional, not migration requirements.
- No general RPC framework, replay log, plugin system, durable command ledger,
  compatibility negotiation, or IPC heartbeat.

Preserve session semantics, multicast, reasoning, cover/uncover, delete-turn,
default-character changes, configuration constraints, vault behavior, provider
execution, cancellation, audio, and restart persistence. Keep the database
format and SQLCipher integration unchanged.

### Architecture and Ownership

```text
React components
    |
    +--> ChaClient / presentation actions
    +--> SessionEventsConnector
                |
        Native client and event adapter
                |
        Platform WebView adapter
                |
        Common bridge dispatcher
                |
        C++ Application
                |
        Existing core and providers
```

This is a logical ownership diagram, not a single-process guarantee. A WebView
has browser/renderer processes managed by its platform runtime; its document or
renderer can fail independently of the C++ application. Native IPC removes
localhost networking, not document replacement or renderer failure.

Dependency direction:

```text
platform host -> bridge -> app -> core
```

- Core/domain code must not depend on WebView, bridge, or platform UI APIs.
- Application code must not depend on inbound HTTP request/response types,
  routing, cookies, or a listener. Outbound HTTP/HTTPS through providers and R2
  remains valid.
- Bridge code decodes allowed operations and maps results. Application
  operations enforce product rules and state preconditions.
- Platform hosts own window/WebView creation, UI-thread scheduling, packaged
  assets, document trust checks, permissions, native dialogs, and lifecycle.
  They do not implement session, vault, or provider policy.

Use the existing Swift host and its C/C++ interoperability boundary on macOS.
No host-language rewrite is required. WebKit/WebView2 types stay in platform
adapters, including resource-task and callback types.

Production applications must not start an application TCP listener, expose a
localhost API, serve assets through a local server, use an access cookie, or
create EventSource for CHA updates. Outbound provider traffic is unchanged.
A virtual HTTPS URL resolved inside WebView2 is allowed: the prohibition is on
a listening server, not on the spelling of a resource URL.

### Inventory Before Extraction

Create a checked migration inventory before deleting any adapter. Each entry
must identify its current caller, application behavior, native replacement,
public result/error contract, cancellation/lifetime behavior, and retained
verification. Names in this document are proposed vocabulary; the inventory
of actual code is the completion checklist.

Inspect:

- Every `ChaClient` method and every HTTP route.
- The separately injected `SessionEventsConnector`, snapshots, appends, and
  session lifecycle notifications.
- Direct frontend `fetch` calls, audio URL helpers, downloads, file inputs,
  navigation/deep links, browser storage, and clipboard use.
- Voice-input WebRTC setup, microphone permission, and its authenticated
  provider request, which currently runs in JavaScript.
- Native runtime entry points and menus, including pre-WebView vault unlock,
  import/export, R2 transfer, shutdown, and post-maintenance reload.
- Public errors, operation timeouts, generated TypeScript types, build scripts,
  package checks, upgrade checks, and the deterministic fake provider.

Classify each entry as an application operation, session event, resource,
native UI action, or transport-only behavior. Preserve useful behavior found
under a transport name. Also classify each affected source and test file as
keep, move, split, or delete, recording where retained behavior/assertions go.
For example, `src/web/sse_mailbox.cpp` and `tests/web/unit_sse_mailbox.cpp`
contain coalescing and backpressure behavior that must survive extraction;
neither file is disposable merely because it lives under `web`.

Minimum coverage:

| Area | Behavior that must be accounted for |
|---|---|
| Bootstrap | Initial/selected conversation, summaries, recent sessions, active vault, capabilities |
| Sessions | List/create/open/rename/delete, submit/multicast, stop, cover/uncover, delete-turn, default character, export |
| Characters/personas | CRUD, character definitions and Markdown files, reference validation |
| Forums | CRUD, members, forum Markdown files, effects on active sessions |
| Providers | CRUD/test, usage constraints, keys, external request diagnostics |
| OAuth | Status/start/poll/disconnect and opening the authorization URL externally |
| Styles/voices | CRUD, usage constraints, voice-input/output settings and runtime capabilities |
| Credentials | List metadata, create/rename/replace/delete, R2 settings |
| Vaults | List/create/update/delete/switch/merge, password/protection, R2 listing/download |
| Maintenance | Configuration import/export, database upload/download, reopen failure |
| Audio | Uncached speech/preview, cache lookup/clear, single/batch generation, status, playback and cancellation |
| Native host | Startup unlock, menu state/actions, file dialogs, external links, close/quit/reload |
| Packaging | Native assets, type generation, release verification, upgrade compatibility, Linux server retirement |

Retain `src/chat`, `src/characters`, `src/providers`, `src/session`,
`src/workspace`, and non-transport utilities. Provider-side SSE decoding,
libcurl, leases, transcript journals, immutable workspace publication, mirroring,
SQLite, and SQLCipher are not browser transport and must survive.

### Packaged Assets and Development

Prove the production asset/origin choice on both platforms before broad
application extraction. Loading a page is not sufficient: the chosen origin
must support the actual React build, module/assets paths, navigation/reload,
storage used by CHA, microphone/WebRTC, audio, and enforceable CSP.

#### macOS

Start with a restricted packaged resource scheme implemented through
WKURLSchemeHandler. Local-file loading is an alternative only if it passes the
same tests; a restricted file root does not prove secure-context or microphone
compatibility. Use public WebKit APIs and test on the supported minimum macOS
version as well as the development machine.

Do not assume a custom scheme supports getUserMedia merely because its HTML
loads. The early platform spike must establish a working supported origin and
record the chosen loader and permission behavior. If neither candidate preserves
voice input, native cutover is blocked until a small supported solution is proven.
Do not quietly disable voice, enable private WebKit flags, or turn this transport
migration into a native media-stack rewrite.

#### Windows

Use HTTPS virtual-host-to-folder mapping for immutable packaged frontend assets.
Use controlled resource handling for dynamic application media. Neither opens
a TCP listener. Restrict cross-origin access and map only the packaged asset root.

Microsoft documents secure-context support for virtual HTTPS content and
different limitations for file loading, as well as potential media-loading
limitations for virtual-host mappings. Verify dynamic media separately:
[WebView2 local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content).

#### Shared asset requirements

Use hash routes for the native UI on both platforms: the packaged document's
path stays fixed, with `#/` or `#/s/{forum}/{session}/` identifying the view.
Keep the existing route grammar/identifier validation, but read the fragment
and make history writes fragment-only. Update initial navigation, back/forward,
reload, reset-to-Welcome, and route tests together. A same-document route change
does not replace the bridge connection. Keep path routing only in the temporary
HTTP frontend during coexistence; no new routing library is needed.

Do not assume a custom scheme permits the current path-changing `pushState`.
The [History API URL-rewrite rules](https://html.spec.whatwg.org/multipage/nav-history-apis.html#can-have-its-url-rewritten)
restrict path changes for both file URLs and non-HTTP(S) schemes. Serving the
shell from a scheme handler does not itself relax those rules. With hash routes,
the asset loader serves a fixed shell and known assets; missing assets fail
instead of becoming an arbitrary shell fallback. Test fragment navigation in
both actual hosts.

Update Vite's base/module asset paths for the selected loader. Do not assume the
current root-relative asset paths work with file loading. Keep application
assets immutable and separate from vault files and generated media.

#### Development

Vite may serve only frontend assets/HMR. Domain calls still use the native
bridge. Require an explicit development build/launch mode and one exact
allowlisted origin, including scheme and port. Release builds must not honor
a development-origin override or expose debug listeners.

A browser-only fake bridge remains useful for component/client tests. Production
behavior must also be tested in actual native hosts. During coexistence, select
HTTP or native transport explicitly at startup; never automatically retry an
uncertain mutation through the other transport.

### Local Resources, Voice Input, and Speech

Large media and database exports do not travel as JSON/base64 RPC. Small control
data, including WebRTC setup descriptions, can use ordinary bounded messages.

For document-owned voice setup and speech synthesis, cancellation names the
original bridge request ID within its connection. The native client knows that
ID before submission and maps its AbortSignal/stop action to the specific cancel
operation; callers do not have to wait for a resource handle to cancel. A cancel
received while the operation is queued prevents it from starting. A late cancel
cannot affect a newer request. This uses the existing pending-call record, not
a new general job registry. Application-owned audio cache jobs keep their
existing independent identity and lifetime.

#### Resource lifetime and isolation

Return opaque resource handles/URLs scoped to a connection and context epoch.
The application validates session/vault ownership and supplies bytes; the host
adapts that reader to its resource API. The application never sees WebKit/COM
resource-task objects.

- Resolve only registered resource types and opaque IDs, never JavaScript paths.
- Reject handles after release, context change, deletion/cache invalidation, or
  document teardown. Do not allow browser caching to bypass current access.
- Cancel/release native reads when a platform resource task stops. A callback
  completing after cancellation must not use a destroyed task or old database.
- Set the correct MIME type and length. Start with complete-resource reads into
  the existing Blob playback path described below; do not build a range-serving
  media adapter unless a proven platform or performance need requires it.
- Keep decrypted/private media out of a persistent WebView disk cache. Use the
  chosen handler's effective no-store policy and verify it; temporary plaintext
  files require controlled private storage and cleanup.
- Do not expose user-controlled HTML/JavaScript or arbitrary vault files under
  the privileged packaged-application origin.

A virtual HTTPS URL is allowed when resolved entirely by the WebView adapter.
The requirement is no socket-served application resources.

#### Voice input and secrets

The current voice runtime endpoint returns the credential value and
`voiceInput.ts` performs the authenticated setup fetch in JavaScript. That path
must change to satisfy native credential ownership.

Keep microphone capture, the WebRTC peer, transcription data channel, and
presentation normalization in the WebView when the platform spike proves them.
Add a specific `voiceInput.connect` operation: JavaScript supplies the bounded
session description needed for setup; native code selects validated settings
and credentials, performs the existing authenticated provider exchange, and
returns the answer description. JavaScript never supplies an arbitrary
destination or receives the stored credential. Preserve existing transcription
settings and external protocol behavior.

Cancel pending setup on user cancel, document loss, context change, or shutdown.
After a late reply, a cancelled JavaScript voice session must not attach it to a
new peer. Stop microphone tracks and close the peer/data channel on all terminal
paths. Permission denial and unavailable capture remain actionable states.

This moves a small authenticated exchange, not the full real-time media stack,
into C++. `voiceInput.runtime` returns only non-secret settings/capabilities.
Verify the existing provider's setup behavior end to end before removing the
old route. Do not introduce a different provider protocol as part of migration.

#### Voice output and cached audio

Preserve both uncached speech/preview and persistent entry-audio generation.
`getEntryAudioSource` alone does not cover the existing feature.

Use operation-specific speech start/cancel/release calls for ephemeral synthesis,
returning an opaque resource handle when ready. A cancelled document must not
leave a provider request or private temporary resource retained indefinitely.
A late completion after cancellation is released without starting playback.

Keep cache generation as application-owned jobs with single/batch submission,
status, errors, cached-entry notifications, and cache clearing. Reload may
reattach to those jobs; it must not submit the same batch automatically.

Preserve the existing `textToSpeech.ts` playback flow: read complete audio into a
Blob, create an object URL, and use that URL for playback/pause/resume. The typed
audio/resource client fetches the opaque local resource through the WebView
handler, replacing server audio URL construction. This binary resource read is
allowed; application RPC still goes through the native bridge. Bytes do not
travel through JSON/base64 messages.

Prove this resource fetch, its origin/CSP permissions, and Blob playback/seek in
both hosts during the initial platform-feasibility work; custom-scheme HTML loading alone does not prove fetch
support. This retains today's whole-audio memory cost and avoids implementing
Range/206 handling merely to preserve seeking. Direct resource-URL playback is
an alternative only if the spike or measurements justify it; it must then prove
the platform's range/seek requirements before adoption.

Preserve AbortSignal cancellation, object-URL revocation, and native resource
release on stop/error/disposal. Also abort reads, stop playback, and clear Blob
references on context/resource invalidation: revoking a native handle does not
revoke bytes already copied into JavaScript. Test generation cancellation
separately from stopping playback and cancelling an application-owned cache job.
Preserve outbound provider networking and current audio provider concurrency/
diagnostic policy.

### Security and Credential Boundaries

JavaScript input remains untrusted even when frontend/native code ship together.
Keep domain-scoped operations; never expose `executeShell`, `rawSql`,
`readAnyFile`, `writeAnyFile`, pointers, or generic native object access.

#### Document and message trust

- Install the bridge only for the packaged application document or explicitly
  enabled exact development origin.
- In each message handler, validate the actual sender, main frame, expected
  document connection, and canonical origin/asset scope. Do not rely on a
  sender-supplied URL, a JSON connection ID, or origin string prefixes.
- Block unauthorized top-level and frame navigation, redirects, and popup
  creation. External approved links open through the existing system-browser
  policy. Remote pages and user-controlled HTML never inherit bridge privileges.
- Recheck connection/document identity before sending replies, events, or private
  resources. Navigation cancellation alone does not validate queued callbacks.
- Keep resource IDs and protocol version separate from authorization.
- Do not add localhost cookies, random access tokens, or Host-header checks to
  the native interface.

On Windows use JSON messages constructed with the JSON library. On macOS use a
fixed receiver body with values supplied as arguments, or equivalently safe
serialization; never interpolate prompts, responses, filenames, or error text
into executable script.

Relevant platform guidance:
[WebView2 security](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security)
and [WebKit JavaScript arguments](https://developer.apple.com/documentation/webkit/wkwebview/callasyncjavascript%28_%3Aarguments%3Ain%3Acontentworld%3A%29?changes=_7_2_7_5).

#### CSP and resources

CSP currently comes from the HTTP asset response; copying the existing HTML to
a bundle does not retain it. Establish the replacement CSP enforcement in the
chosen loader before removing that response header.

Keep a restrictive script/style/resource policy. Permit only the selected local
asset/media sources and actual required external connections. Move the voice
setup request native so it no longer requires a JavaScript credential-bearing
fetch. Permit the exact local audio resource source in `connect-src` and Blob
playback in `media-src`; verify these with the selected loader rather than
broadening the policy to arbitrary schemes. Any development HMR allowances
belong only to development mode.

The platform spike must record how CSP is applied before application scripts
run and verify it in the packaged hosts. If a meta policy is used, account for
directives it cannot enforce and implement those restrictions in the host.
Preserve navigation/frame isolation independently. Do not allow remote scripts,
unsafe evaluation, or broad filesystem access just to make an asset loader work.

Test policy behavior, not only the presence of a header or tag. Rewrite useful
existing asset/injection tests for the native loader instead of deleting them
with the HTTP-header assertions.

#### Secrets

Stored API keys, R2 secrets, vault passwords retained for native operations, and
OAuth tokens remain native. Runtime DTOs expose IDs, names, presence, and
non-secret settings. Secret creation/replacement is an explicit write-only flow;
clear UI buffers promptly and never return the stored value in normal reads.

Do not log bridge parameters, raw replies, prompts, responses, credentials,
session descriptions, or sensitive configuration. Resource caches must not
persist plaintext from protected vaults through the browser after release.

### Build and Packaging

Final logical targets:

```text
cha_macos   ----+
               +--> cha_bridge --> cha_app --> cha_core
cha_windows ----+
```

Core includes existing chat, characters, providers, session, workspace, and
utilities. Outbound libcurl and provider protocol decoding stay. Remove
cpp-httplib from the standalone production dependency graph; deterministic fake
provider/test targets may retain it as an isolated test dependency.

Package the built React assets directly. Preserve supported OS/runtime checks,
icons, signing, resource paths, and the current startup/upgrade behavior. Keep
WebView2 runtime checks in the Windows host.

Both desktop packaging scripts currently verify production web files using
`chaweb`; macOS also reuses Linux package-check machinery. Replace those checks
with application-level compatibility tests plus real native-host verification
before removing the server target. Do not relabel a browser test with a fake
bridge as a packaged native end-to-end test.

At cutover remove the server executable, listener/runtime cookie wiring, port
flags, production HTTP asset serving, Vite API proxy, server launch scripts, and
Linux server package target. Retain reusable seed data and compatibility tests;
move shared assets out of Linux-specific locations when necessary rather than
deleting them with the package.

Linux can continue to compile and test common code without WebView dependencies.
An actual Linux GUI product requires a separately scoped host; this migration
does not leave a server release as an accidental supported fallback.

## Implementation steps

1. Record the starting revision, dirty paths, baseline test results, supported
   OS/runtime versions, and native unlock/menu behavior. Establish a repeatable
   code-size baseline separating handwritten production, generated code, tests,
   packaging, and documentation; exclude vendored/build output and record the
   exact file set/command for the final server-removal stage.
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

## Verification for this block

- Run baseline common C++ checks, frontend checks/build, and existing HTTP E2E
  where supported; preserve pre-existing failures with reproduction commands.
- On both native hosts assert a safe round trip and an intentional assertion
  failure/nonzero test exit. Record the runner launch, readiness, timeout, and
  process cleanup behavior.
- Verify module/assets, fragment history/reload, used storage APIs, trusted sender
  checks, blocked remote/frame navigation, effective CSP, and release-disabled hooks.
- Prove actual microphone permission/capture/provider setup and local-resource
  fetch/Blob playback/seek/cancel/release. Record cache/privacy observations and
  minimum-platform results separately from deterministic fixture tests.
- Confirm neither native feasibility mode starts a CHA application listener.
  An outbound-provider test listener or test-only CDP port is a separate purpose.

### Commands and platform evidence

Run from the repository root with the configured supported native toolchain.
These commands exist at the start of the migration; follow recorded target/script
renames in the current checkout. Do not invent test target names or resurrect a
retired target merely to run an old command.

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -N
ctest --test-dir build/ninja --output-on-failure
npm --prefix webapp run check
npm --prefix webapp run build
```

Use `ctest -R` with discovered names for focused iteration. Existing targets include
`cha_tests`, `cha_web_tests`, `cha_web_stress_tests`, and conditionally
`cha_web_process_tests`; app/bridge replacements may already exist. Use the pinned
Node/npm versions in `webapp/package.json` and `webapp/.node-version`; install locked
dependencies with `npm ci` from `webapp` when needed.

For ownership/concurrency changes use the `asan-ubsan` and, on supported toolchains,
`tsan` configure/build presets with focused tests. Record toolchain limitations;
do not add broad suppressions or weaken assertions to turn a failure green.

`npm --prefix webapp run e2e` originally starts `chaweb`. Use it only for retained
HTTP baseline/regression coverage while applicable; it is never native proof.
Locate or create the real host runner required by this task and record its exact
command and OS/WebView version. A native test must exercise actual bridge/app code.

For package work, existing entry points are:

```sh
./packaging/macos/package.sh <test-version> <temporary-output-parent>
```

```powershell
./packaging/windows/package.ps1 -Version <test-version> -OutputParent <temporary-output-parent>
```

Replace placeholders with actual test values and isolated output directories.
Read the scripts first; their original server-based validation must be replaced
when this block requires native package parity. Test-only CDP or an outbound fake
provider listener does not authorize an application HTTP fallback in production.

## Acceptance gate

The inventory/baseline exists and both platforms pass origin,
voice/media, trust/CSP, and native-automation probes. Missing real-platform or
provider evidence remains pending. A feasibility failure blocks broad extraction;
do not disable voice, use private APIs, or keep production HTTP to pass this gate.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide inventory, baseline/failures, wire fixtures, chosen loaders/public
APIs, platform evidence, runner commands, and probe code to reuse or remove.

Include the implementation and retained/new tests in the working diff. Record actual
interfaces, lifetime rules, commands, and compatibility leftovers so later work
can use the result without assumptions about filenames or implicit conversation state.

## Execution record and final response

This record is intentionally uncompleted. Fill it with actual evidence as work
proceeds, or provide the same fields in the task handoff if editing this record
is outside the active user's document scope. Prior evidence may be reused only
when its revision/coverage remains applicable. Do not require the next agent to
read an entire conversation to recover decisions.

```text
Status: not started | in progress | waiting for evidence | complete
Starting and resulting revision/checkpoint:
Files changed/moved and actual new APIs/targets:
Implemented behavior and key ownership/contract decisions:
Prerequisites verified and evidence used:
Temporary compatibility code and when it can be removed:
Exact commands, working directories, platform/runtime versions, and results:
Known failures, checks not run, and exact missing evidence:
Inventory/coverage changes and remaining work:
Next unfinished numbered step if this block needs continuation:
```

Maintain concise rows for the operations/files/assertions touched by this block.
For the inventory block, enumerate the complete migration; for later blocks,
carry forward existing evidence and record the relevant updates.

| Operation/caller or source/test path | Retained behavior/result/errors | Native destination or deletion reason | Context/cancellation/lifetime | Verification and status |
|---|---|---|---|---|
| Populate during execution | | | | |

| Required flow/assertion | Common test evidence | macOS evidence | Windows evidence | Remaining limitation |
|---|---|---|---|---|
| Populate during execution | | | | |

The final response must state what was implemented, why, what was actually tested,
and any unresolved limitation. If incomplete, give the exact next step and missing
prerequisite/evidence. A context limit or a mostly working platform is not success.

## Optional provenance

The [migration plan](plan.md) and [design proposal](redesign.md) explain the overall
sequence and original rationale. They are reference material, not additional
required instructions for executing this brief.
