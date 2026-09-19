# Block 8 — Resources, speech, audio jobs, and voice input

Implement this block in the CHA repository. This is a standalone execution brief:
its scope, prerequisites, design contracts, steps, tests, and handoff requirements
are included here. Reading `docs/plan.md`, `docs/redesign.md`, or other block briefs
is not required. The current source code and actual prior implementation/evidence
are still required inputs; this document does not claim those prerequisites exist.

**Initial status:** not started. **Environment:** macOS and Windows/WebView2, native test tools, deterministic providers, and microphones.

## Objective and scope

Complete revocable local resources, uncached speech, persistent audio/cache jobs, playback/download behavior, and native authenticated voice setup while keeping WebView capture and playback.

Reuse proven local loaders, native credentials/providers, maintenance, file actions, and lifecycle. Do not create an HTTP file server, binary JSON channel, new realtime protocol, generic job framework, or native media-stack rewrite.

## Prerequisites to verify before editing

- Both platforms have already proven the selected packaged origin, microphone/
  WebRTC setup and permission behavior, local resource fetch/Blob playback/seek,
  effective CSP, and native automation; confirm actual recorded public APIs.
- Native hosts, bounded bridge, original-request cancellation identity, safe
  document callbacks, actor lifecycle, and shared shutdown are implemented.
- Maintenance/context invalidation protects vault/database lifetime and exposes
  the hook media readers/jobs need; controlled native save actions are available.
- Providers, credentials, OAuth, appearance/voice configuration, and non-secret
  runtime capabilities are migrated. Existing synthesis and persistent audio
  backends/concurrency/deadline semantics remain available to reuse.

Confirm these in the current checkout and test records. Record actual entry points
and commands if earlier work renamed them. If evidence is unavailable, perform the
relevant check when possible; otherwise keep the prerequisite and gate pending.

## Execution rules and 400K context budget

Plan for **250–320K tokens of working context** within a **400K-token window**,
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
direction is host → bridge → app → core. The only supported products are
macOS/WKWebView and Windows/WebView2.
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

Start with the platform-feasibility work's proven loaders/media
exchange, `src/web/{fish_audio,audio_download,audio_download_routes}.*`,
`webapp/src/{textToSpeech,audioDownloads,voiceInput}.ts`, existing media tests,
and the context/shutdown/native file APIs from earlier blocks.

Read the steps and embedded requirements below before changing behavior. Names of
new modules are illustrative; reuse the current implementation and avoid parallel
copies. Braces and `*` in paths denote small related file groups, not literal names.

## Embedded design requirements

The following requirements are included directly so this brief can be executed
alone. They constrain behavior this block implements or touches. They do **not**
expand the scope beyond the numbered steps and acceptance gate; requirements for
later capabilities describe the boundary those capabilities must eventually meet.

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

### Runtime State, Vault Context, and Maintenance

The application has explicit running, maintenance, stopping, and unavailable
states. State changes and operation admission are coordinated outside platform
UI code. Native menus use the same gate as bridge calls.

`app.bootstrap` returns a monotonically increasing `context_epoch` identifying
the current usable application/vault context. Every domain request includes the
epoch obtained from bootstrap. The bridge captures that context when admitting
work and the operation checks it again before applying effects. The context check and the
mutation/resource acquisition share exclusion with maintenance through the
existing guards; a check followed by an unprotected use is insufficient. Never resolve
an old queued operation against whichever vault happens to be current later.

A context epoch is a stale-work check, not a credential or a replacement for
origin validation. Do not add persistent identifiers or a registry for it.

Operations that switch/replace the database, change protection, or close handles
and rebuild live state follow this order:

1. Reserve global maintenance and stop admitting conflicting domain work.
2. Invalidate the old context for queued work, subscriptions, resource handles,
   and presentation results. Already executing work must settle/cancel under
   the existing ownership and maintenance rules before retargeting.
3. Stop/release affected session actors; pause/cancel audio and voice setup as
   required. Prevent resource readers from retaining handles being closed.
4. Acquire the workspace-store maintenance lock before repository/database
   guards, preserving the existing lock order and checkpoint/reopen behavior.
5. Perform the operation and reopen/resynchronize repository and workspace state.
6. Publish a new usable context epoch, or mark the application unavailable if
   safe reopen fails. Only then reopen admission.

If work stopped actors or invalidated handles before failing, recovery still
publishes a new epoch after safe reopen, even when the active vault is unchanged.
Read-only operations that leave live state and handles intact need not invalidate
the context.

A stale command fails with `vault_changed`; it must not execute in a different
vault. Stale session events/results and resource URLs are discarded or rejected.
Existing callers using `vault_name` remain compatible during migration, but a
name alone is not sufficient across switch-away/switch-back or database replacement.

The initiating maintenance call receives its own terminal result despite the
context invalidation. That result identifies the resulting context/state and
must not be treated as ordinary old-context data. Also send a bounded
`app.contextChanged` notification for native-menu operations. The frontend accepts this notification for its live connection even when it
announces a newer epoch. It rejects remaining old-context promises (their
mutation outcomes may be unknown), clears context-bound state, stops
voice/playback, bootstraps, and reopens the
selected conversation if it still exists. A full WebView reload is an acceptable
initial implementation. Repeated notifications for the same epoch are harmless.

If safe reopen fails, ordinary domain operations consistently return
`application_unavailable`. Keep the native shell and diagnostics available to
report the error and quit/restart; do not leave apparently usable commands
operating on partially reopened state.

### Bridge Requests and Command Completion

Use a fixed allowlist of domain operations. Do not expose arbitrary C++ objects,
function lookup, shell execution, SQL, or generic filesystem paths.

Each document gets a native-created `connection_id`. Native code associates it
with the trusted loaded document; accepting a matching value in JSON alone does
not establish trust. Each request ID is unique within that connection.

Example request:

```json
{
  "connection_id": "view-9",
  "id": 42,
  "context_epoch": 3,
  "method": "session.submit",
  "params": {
    "forum_id": "history",
    "session_id": "session-1",
    "input": { "text": "..." }
  }
}
```

Example completed short command:

```json
{
  "connection_id": "view-9",
  "id": 42,
  "context_epoch": 3,
  "ok": true,
  "result": { "clear_input": true }
}
```

Retain the actual public CommandResult DTO, including draft-clearing and notice
behavior. The example illustrates that result; it is not permission to substitute
a generic `accepted: true` for it.

`session.submit` completes when the owner has executed the short command and
determined its result. It must not complete merely because the queue accepted it,
and must not wait for model generation to finish. Generation and Stop remain
separate application operations. Reply/event arrival order must not be used as
a substitute for the session event ordering contract.

Example failure:

```json
{
  "connection_id": "view-9",
  "id": 42,
  "context_epoch": 3,
  "ok": false,
  "error": {
    "code": "session_not_live",
    "message": "That session is not open."
  }
}
```

Each admitted RPC produces at most one terminal reply for its live document.
Reject duplicate outstanding request IDs. Do not reuse IDs within a connection;
replace the connection before exhausting the JavaScript-safe integer range.
Validate all numeric identifiers/sequences without lossy conversion; wire values
that can exceed JavaScript's safe integer range must use an explicit string form.

Correlation does not provide exactly-once execution. Preserve the existing
`command_timeout` meaning: the result is unknown and the command may still
execute. Document disposal, timeout, or loss of a reply does not roll back a
mutation. Never automatically replay mutations after reload, timeout, or a
transport fallback. Refresh authoritative state before offering a deliberate
user retry. No durable deduplication ledger is needed.

`bridge.info` returns a protocol version, application version, and platform.
`app.bootstrap` returns context and initial UI state. These operations work
without a previously bootstrapped epoch. An incompatible protocol stops domain
calls with a clear reload/rebuild error; it does not negotiate multiple protocol
versions. Matching packaged frontend/native code is the supported deployment.

### Dispatch, Threads, and Shutdown

The common router decodes the envelope, checks connection/context and allowed
method, invokes the application operation, and maps a terminal result. It must
not perform blocking application work in a platform message callback.

Use the smallest execution arrangement that preserves existing ownership:

| Work | Execution and admission |
|---|---|
| WebView APIs, navigation, permission callbacks, dialogs | Platform UI thread; no blocking waits for C++ work |
| Session mutations, snapshots, subscription establishment | Existing session owner queue, with asynchronous completion to the bridge |
| Settings reads/writes and other ordinary operations | Small bounded application work queue; retain store locks and semantic validation |
| Provider tests and authenticated voice setup | Background/native provider work; do not occupy session command execution |
| Database/vault maintenance | Serialized background lifecycle operation and common maintenance gate |
| Outbound generation and audio | Existing provider/audio workers and cancellation mechanisms |
| Native-to-JavaScript delivery | Bounded mailbox drained via the UI thread |

An HTTP adapter may temporarily wait on an application result on its worker
thread. The native path must not fill a shared worker pool with threads waiting
for session replies. Adapt owner completion asynchronously. In particular,
Stop, unsubscribe, disposal, and shutdown must not wait behind slow provider
tests, dialogs, audio synthesis, or database transfers in a general FIFO.

Bound outstanding bridge calls and ordinary queued work with small internal
limits; reuse existing limits where appropriate rather than adding configuration.
Keep control admission separate from bulk work so saturated ordinary calls
cannot prevent cancellation/disposal. A full queue returns a stable public error.
An admitted call holds its reply-capacity slot until consumption or connection
invalidation, not merely until its worker finishes. If a sender exceeds the
native in-flight limit, invalidate that connection with bounded error reporting
instead of allocating an unbounded queue of rejection replies.
During maintenance, reject conflicting commands promptly instead of accumulating
work to run against a later context.

An event sink or completion must carry a checked connection lifetime, not an
unguarded raw pointer to a platform view. Queued callbacks may run after
invalidation, but then do nothing. Before delivering, recheck the live document
and context. Destroying a view does not destroy the C++ Application.

Shutdown is asynchronous from the native UI's perspective:

1. Publish stopping; reject new bridge and native-menu domain operations.
2. Invalidate document deliveries/subscriptions and cancel pending native dialogs.
   Settle local promises if the document still exists, without requiring receipt.
3. Cancel/settle admitted application tasks, maintenance, provider tests, voice
   setup, generation, and audio according to their existing cancellation policy.
   Wake abandoned waiters; do not hold a lifecycle mutex while joining work that
   needs it.
4. Let owners finish their required persistence/mirroring and destroy their
   controllers; join actors and other application jobs. Final persistence occurs
   after the relevant state has settled, not as a one-time flush before it changes.
5. Close repository/store/database resources only after their users have stopped.
   Preserve provider completion/supervisor dependencies during cancellation and
   joining, then shut down the provider supervisor.
6. Destroy Application and platform adapters, keeping logging available through
   teardown. Destroy the view/window according to native quit behavior.

Use one bounded shutdown grace rather than restarting a full deadline for every
actor. Preserve the existing controlled process-failure policy if an owner or
worker cannot safely finish. Never detach a live worker and then destroy objects
it can access. No shutdown step depends on the renderer draining its event queue.

### Document Lifetime and Rehydration

Install the trusted bridge receiver before application JavaScript starts. Bind
its native connection lifetime to a specific loaded main-frame document.

On navigation, reload, renderer failure, or view disposal:

- Stop admitting requests from the old document.
- Invalidate its connection before any new document can use the bridge.
- Reject pending JavaScript promises when possible and remove local listeners.
- Detach native subscriptions and release pending presentation batches/resources.
- Cancel document-owned voice setup and speech preview work. Already executed
  mutations and application-owned generation/cache jobs retain their own policy.
- Ignore late replies, resource completions, and callbacks from that connection.

Request IDs can start again only in the new connection. A late reply to old
request 42 must never resolve new request 42.

Recovery:

```text
new trusted document/receiver
    -> bridge.info
    -> app.bootstrap
    -> restore application-selected session if still available
    -> session.open
    -> session.subscribe with a new ID
    -> initial snapshot
```

Preserve useful frontend guards against old navigation results and React
StrictMode cleanup. Remove EventSource status, SSE supersession, stream-slot
waiting, server probes, and network reconnect delay ladders. A native renderer
failure is handled by the host's reload/recreation path; repeated failure becomes
an actionable reload/restart state instead of an endless loop.

Use platform lifecycle notifications, including WKWebView web-content process
termination and WebView2 ProcessFailed. Do not assume the main application dies
when its renderer does. See the
[WebView2 process model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model)
and Apple's
[web-content termination callback](https://developer.apple.com/documentation/webkit/wknavigationdelegate/webviewwebcontentprocessdidterminate%28_%3A%29).

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

### File Operations, Native Menus, and Operation Coverage

Where the current operation uses a configured import/export directory, preserve
that behavior. Where a user-selected source or destination is required, the host
opens a native dialog on the UI thread and gives a controlled native handle/path
to the application operation. Do not return arbitrary filesystem access to
JavaScript or change existing menu semantics merely to unify APIs.

Dialog cancellation completes as cancellation without side effects. No worker
holds a database lock while waiting for user input. After a dialog returns,
revalidate document/context before starting work. Save failures must not destroy
an existing destination; retain the current temporary-file/replace behavior and
cleanup. Shutdown cancels pending dialogs without blocking the UI thread.

Native menu actions call the same application operations and maintenance gate
as the bridge. Their enabled state follows application capabilities and lifecycle.
Do not put a second copy of vault/password/transfer policy in each host.

The operation inventory must cover at least these groups:

| Group | Proposed operations |
|---|---|
| Application | `bridge.info`, `app.bootstrap` |
| Sessions | `session.list/create/rename/delete/open/close/snapshot/submit/stop`, `session.cover/uncover/deleteTurn/setDefaultCharacter/export`, `session.subscribe/unsubscribe` |
| Characters | `character.get/create/update/updateDefinition/delete`, `character.file.get/create/update/delete` |
| Personas | `persona.get/create/update/delete` |
| Forums | `forum.get/create/update/delete`, `forum.members.update`, `forum.file.get/create/update/delete` |
| Providers | `provider.list/get/create/update/delete/test` |
| OAuth | `openaiAuth.get/start/poll/disconnect` |
| Styles/voices | `style.list/create/update/delete`, `voice.list/create/update/delete` |
| Voice input | `voiceInput.get/save/runtime/connect/cancel` |
| Voice output | `voiceOutput.get/save/runtime`, `speech.start/cancel/release` |
| Entry audio | `audio.start/startBatch/status/source/clearCache/release` |
| Keys | `apiKey.list/create/rename/replaceValue/delete` |
| R2 settings | `r2Storage.get/save/delete` |
| Vaults | `vault.list/create/update/delete/switch/merge`, `vault.r2.list/download` |
| Transfers | `configuration.import/export`, `database.upload/download` |

Slash-separated names above are shorthand for individual fixed operations, not
a dynamic namespace or generic CRUD framework. Final spellings are settled in
the inventory. Native-only operations need not gain a React button or wire
binding when no frontend caller needs one.

Keep OAuth state and tokens native, preserving the current authorization flow.
Open approved authorization links in the system browser; do not expose the CHA
bridge to an embedded login page. Display user-facing verification details when
the existing flow requires them, not stored provider credentials.

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

### Validation, Errors, Timeouts, and Diagnostics

Keep stable application errors and public messages independent of HTTP status.
Preserve distinctions the UI uses, including:

```text
invalid_argument / existing mapped input errors
not_found
prompt_too_large
session_not_live
session_stopping
session_limit_reached
session_open_timeout
command_queue_full
command_timeout
operation_cancelled
vault_changed
vault_password_required
source_vault_password_required
speech_busy
application_unavailable
internal_error
```

The inventory defines any deliberate old-to-new code mapping and updates callers
and tests together. Do not collapse password prompts, unavailable state, and an
unknown mutation outcome into one generic failure. Internal exception details
go to logs; only approved public messages reach React.

Bridge shape validation covers envelope/method, required/unknown fields according
to the DTO, types, safe numeric ranges, enums, identifiers, filenames, and encoded
message/text size. Keep existing product limits, including prompt and editable
file sizes. Bound input before expensive parsing/copying where the platform
allows it. Do not discard size safeguards just because HTTP body framing goes.

Application operations independently validate permissions/capabilities,
references, provider configuration, state preconditions, and context before
side effects. Native-menu callers must not bypass those rules.

Keep targeted runtime checks for critical envelopes, snapshots/appends, and
foreign/persisted data. TypeScript generics and a version match do not validate
C++ serialization. Native client tests using a fake bridge are useful but must
be complemented by shared wire examples produced by the real C++ serializer.

Keep provider total/idle timeouts, bounded owner-command waits, applicable
maintenance deadlines, and shutdown grace. Remove HTTP read/write deadlines,
SSE heartbeat/drain timers, and network reconnect/orphan grace.

Distinguish timeout/caller abandonment from operation cancellation. Speech and
voice setup use explicit cancellation; generation uses `session.stop`; a
maintenance operation follows its safe commit/reopen boundary. Do not promise
rollback just because a UI stopped waiting.

Log lightweight method/result/duration and lifecycle diagnostics. Remove routine
listener, HTTP status, heartbeat, and reconnect logging. Avoid per-token info
logs; retain provider diagnostics under their current policy and measure event
delivery with aggregate/debug instrumentation when needed.

## Implementation steps

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
    the final server-removal stage. Keep permission denial, unavailable capture, device/provider failure,
    and cancellation actionable without switching protocols or rewriting media.
11. Extend shared race/context/shutdown tests to synthesis, persistent audio jobs,
    loaded-cache deletion, resource reads, setup, and pending playback. Test
    unknown/stale handles, traversal/untrusted requests, late completion, and
    cancellation at every request/resource/playback boundary.
12. Exercise uncached/cached/batch playback, seek/resume, native saves, real microphone/
    transcription, permission denial, cancellation, reload, and quit in both
    hosts. Separate deterministic provider tests from real-device/provider proof;
    missing required platform evidence keeps the block pending.

## Verification for this block

- Verify actual local resource fetch/playback on both hosts with correct MIME/
  length, origin/CSP permissions, abort, no-store/private-cache behavior, and
  scoped authorization; no TCP resource serving or JSON/base64 binary transport.
- Reject unknown/released/stale/cross-context handles, traversal, and requests
  from untrusted documents; cancel task reads and ignore late callbacks safely
  during reload, database closure, cache invalidation, and view destruction.
- Test speech cancellation before queue execution, during provider work, after
  resource completion, and after disposal; late completion cannot restart playback.
- Exercise uncached/cached/batch audio, status/errors/cache updates/clear, native
  downloads/saves, Blob seek/resume, error/end cleanup, object-URL release, and
  already-copied Blob invalidation. Reload reattaches without resubmitting jobs.
- Test native authenticated voice setup with a bounded description, native-only
  destination/key selection, and current provider protocol. Real microphone/
  transcription/permission-denial checks supplement deterministic setup tests.
- Cancel voice setup before/after replies, switch context, deny permission, lose
  devices, and quit; close tracks/peers/data channels and never attach old answers
  to a newer capture session.
- Extend shared maintenance/shutdown/saturation tests to resource readers, speech,
  application-owned cache jobs, loaded audio, and voice setup. Distinguish playback
  stop, preview cancellation, and persistent-job cancellation.

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

## Acceptance gate

Full existing voice-input/output, cache, playback, and media
file behavior works in both native hosts without CHA HTTP or binary JSON payloads.
Credentials remain native, resources are revocable, and new work types pass
maintenance/shutdown tests without changing persistent-job semantics.

Mark complete only after the numbered steps and relevant checks above pass.
Record pre-existing failures separately, and never report a pending check as passed.

## Required deliverables

Provide media operation coverage, resource ownership/CSP/cache behavior,
provider/capture evidence, cancellation tests, and obsolete helpers for the final server-removal stage.

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
