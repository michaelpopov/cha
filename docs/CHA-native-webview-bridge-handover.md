# CHA Native WebView Bridge Migration Handover

## 1. Purpose

This document defines the target architecture and implementation plan for removing CHA’s internal HTTP server and replacing browser-style HTTP/SSE communication with direct communication between the React UI running inside an embedded WebView and the existing C++ application/core.

The goals are:

- reduce application complexity;
- eliminate internal localhost networking and browser/server recovery machinery;
- preserve the existing React UI;
- preserve the C++ domain/application logic;
- preserve session semantics, persistence, provider execution, streaming, cancellation, configuration, vault behavior, and audio behavior;
- create a clean platform-neutral application boundary that can be hosted on macOS, Windows, and potentially iOS;
- keep the frontend/backend boundary explicit and testable;
- make the standalone application the only supported deployment model.

The target is **not** to move backend behavior into JavaScript, and it is **not** to rewrite the C++ core in another language.

The target model is:

> **React remains the presentation layer. C++ remains the application/domain/backend layer. HTTP/SSE between them is replaced with a narrow asynchronous native WebView bridge.**

---

## 2. Target Architecture

### 2.1 High-level design

```text
┌───────────────────────────────────────────────────────────────┐
│                     Standalone CHA process                    │
│                                                               │
│   ┌───────────────────────────────────────────────────────┐   │
│   │                  Embedded WebView                     │   │
│   │                                                       │   │
│   │   React / TypeScript UI                               │   │
│   │   ├── screens and components                          │   │
│   │   ├── presentation/application state                  │   │
│   │   ├── ChaClient                                       │   │
│   │   ├── NativeChaClient                                 │   │
│   │   └── NativeBridge                                    │   │
│   └─────────────────────────┬─────────────────────────────┘   │
│                             │                                 │
│                    native asynchronous IPC                    │
│                  invoke / result / event                      │
│                             │                                 │
│   ┌─────────────────────────▼─────────────────────────────┐   │
│   │                   C++ bridge layer                    │   │
│   │                                                       │   │
│   │   BridgeRouter                                        │   │
│   │   ├── request dispatch                                │   │
│   │   ├── argument validation                             │   │
│   │   ├── result/error mapping                            │   │
│   │   └── event delivery                                  │   │
│   └─────────────────────────┬─────────────────────────────┘   │
│                             │                                 │
│   ┌─────────────────────────▼─────────────────────────────┐   │
│   │                  C++ application layer                │   │
│   │                                                       │   │
│   │   Application                                         │   │
│   │   ├── SessionService                                  │   │
│   │   ├── WorkspaceService                                │   │
│   │   ├── SettingsService                                 │   │
│   │   ├── VaultService                                    │   │
│   │   ├── ProviderService                                 │   │
│   │   ├── AudioService                                    │   │
│   │   └── lifecycle / maintenance coordination            │   │
│   └─────────────────────────┬─────────────────────────────┘   │
│                             │                                 │
│   ┌─────────────────────────▼─────────────────────────────┐   │
│   │                   Existing CHA core                   │   │
│   │                                                       │   │
│   │   chat / characters / session / workspace             │   │
│   │   providers / SQLite / SQLCipher / utilities          │   │
│   └───────────────────────────────────────────────────────┘   │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### 2.2 Platform hosts

The same React UI and C++ application layer should be hosted by thin platform adapters:

```text
                         React UI
                            │
                       NativeBridge
                            │
                      BridgeRouter
                            │
                  C++ Application layer
                            │
                       CHA core
                ┌───────────┼───────────┐
                │           │           │
              macOS       Windows    future iOS
             WKWebView     WebView2    WKWebView
```

Each platform host is responsible for:

- creating the native application window;
- creating/configuring the WebView;
- loading packaged frontend assets;
- installing the JavaScript/native bridge;
- forwarding JavaScript requests to the common C++ `BridgeRouter`;
- delivering C++ events to JavaScript;
- native file/open/save dialogs;
- platform lifecycle;
- permissions and platform-specific integration;
- local binary-resource delivery when required.

Platform hosts must **not** own product semantics such as session rules, provider behavior, workspace validation, persistence policy, or vault policy.

---

## 3. Core Architectural Rules

### 3.1 Preserve the C++ core

The following remain C++ responsibilities:

- session identity and lifecycle;
- transcript semantics;
- character/persona/forum rules;
- model-context creation;
- provider execution;
- generation cancellation;
- persistence;
- configuration validation;
- vault and database behavior;
- audio-generation coordination;
- secret handling;
- authoritative application state.

### 3.2 Replace transport, not semantics

Existing HTTP routes need to be classified before removal.

Every current browser-facing endpoint must become one of:

1. **Application operation** — move to a transport-neutral C++ service.
2. **Session event** — delivered through native WebView events.
3. **Binary/local resource** — delivered through a local WebView resource handler.
4. **Native UI operation** — handled through file dialogs/share/save APIs.
5. **Transport-only behavior** — delete.

Examples of transport-only behavior include:

- listener port management;
- HTTP request parsing;
- Host/Origin validation for the internal API;
- HTTP status mapping;
- SSE heartbeat;
- EventSource reconnect policy;
- browser connection takeover logic;
- localhost access tokens/cookies.

### 3.3 Keep a narrow privilege boundary

JavaScript must not receive arbitrary access to C++ objects.

Do not expose:

```text
callCppFunction(name, rawPointer, ...)
executeShell(...)
rawSql(...)
readAnyFile(path)
writeAnyFile(path)
```

Instead expose domain operations:

```text
session.submit
session.stop
forum.update
provider.test
configuration.export
```

### 3.4 Preserve session-thread ownership

Removing HTTP worker threads does not make `SessionController` safe to call from the UI thread.

The existing invariant should initially remain:

> One session owner thread exclusively owns and mutates its `SessionController`.

Native bridge calls should enqueue operations to the session actor where appropriate.

### 3.5 Release builds must not listen on localhost

The final standalone application must not:

- bind a TCP port;
- start an internal HTTP server;
- expose an application API over localhost;
- use EventSource for core application updates;
- depend on a runtime access cookie;
- use a local HTTP server to serve packaged frontend assets.

Outbound network access to external providers remains unchanged.

---

## 4. Current CHA Layers: Target Treatment

## 4.1 `src/chat`

**Keep.**

This contains presentation-neutral conversation/domain state and should remain independent of transport and WebView code.

## 4.2 `src/characters`

**Keep.**

Character definitions, model context, provider association, persona behavior, validation, and identity belong in the core.

## 4.3 `src/providers`

**Keep initially.**

Retain:

- `Providers`;
- `ProviderRequest`;
- `ProviderClient`;
- outbound HTTPS;
- Chat Completions protocol;
- Responses API protocol;
- provider-side streaming decoding;
- cancellation;
- request-level diagnostics;
- API key handling;
- OpenAI OAuth integration.

Important distinction:

> The browser-facing SSE layer is removed. Provider-facing streaming remains.

Therefore code such as provider SSE framing/decoding must not be deleted merely because the UI no longer uses SSE.

## 4.4 `src/session`

**Keep.**

Retain:

- `SessionController`;
- `SessionRepository`;
- transcript journal;
- session storage;
- leases;
- SQLite/SQLCipher integration;
- restoration and persistence;
- stable forum/session identity;
- controller update semantics.

## 4.5 `src/workspace`

**Keep.**

Retain:

- workspace loading;
- configuration materialization;
- validation;
- built-ins;
- session-opening policy;
- published immutable workspace model.

## 4.6 `src/util`

**Mostly keep.**

Remove only utilities proven to exist solely for HTTP/server transport.

## 4.7 SQLite and SQLCipher

**Keep the current database format during this migration.**

Do not combine the transport migration with a storage-format migration.

---

## 5. Extract Application Logic from `src/web`

A key requirement is to avoid deleting useful application behavior just because it currently lives under `src/web`.

The current web layer combines:

- genuine application orchestration;
- HTTP transport;
- browser connection management;
- serialization;
- desktop runtime composition.

These responsibilities must be separated.

---

## 6. Introduce a Transport-Neutral `src/app` Layer

Recommended conceptual structure:

```text
src/
  app/
    application.h
    application.cpp
    application_options.h
    application_error.h

    session_service.h
    session_service.cpp

    workspace_service.h
    workspace_service.cpp

    settings_service.h
    settings_service.cpp

    vault_service.h
    vault_service.cpp

    provider_service.h
    provider_service.cpp

    audio_service.h
    audio_service.cpp

    database_maintenance.h
    database_maintenance.cpp
```

Exact filenames may differ. The important dependency rule is:

```text
bridge -> app -> core/domain

core/domain -X-> bridge
core/domain -X-> WebKit/WebView2
app         -X-> HTTP
```

---

## 7. Refactor `ApplicationRuntime`

The current `ApplicationRuntime` owns both application behavior and web-server behavior.

Its responsibilities should be split.

### 7.1 New application composition root

Create a transport-neutral application object:

```cpp
namespace cha::app {

class Application {
public:
    static std::unique_ptr<Application> open(
        const ApplicationOptions& options);

    Bootstrap bootstrap();

    SessionService& sessions();
    WorkspaceService& workspace();
    SettingsService& settings();
    VaultService& vaults();
    ProviderService& providers();
    AudioService& audio();

    void shutdown();
};

}
```

It should own or coordinate the current process-level objects:

- workspace configuration store;
- session repository;
- providers;
- active sessions;
- API-key store;
- OAuth state;
- audio-download manager;
- vault state;
- mirror service;
- database maintenance coordinator.

It must not own:

- `httplib::Server`;
- listener thread;
- host/port configuration;
- HTTP routing;
- browser access token.

### 7.2 Preserve deterministic shutdown ownership

Current construction/destruction ordering should remain explicit.

The new composition root must ensure:

1. admission of new UI commands stops;
2. active generation is cancelled/settled according to policy;
3. active session actors stop;
4. audio work stops;
5. repository/store/database resources close;
6. provider supervisor shuts down;
7. logging stays available through teardown.

---

## 8. Move Database Maintenance out of the Web Runtime

Vault/database operations currently coordinate several subsystems. This is application logic and must be preserved.

Create a transport-neutral maintenance coordinator responsible for:

- reserving global maintenance;
- preventing conflicting live-session work;
- pausing audio activity;
- obtaining workspace-store maintenance lock;
- checkpointing repository/database state;
- closing database handles;
- performing import/export/upload/download/protection operations;
- reopening database handles;
- resynchronizing workspace/session state;
- marking the application unusable if safe reopen fails.

The bridge should call this service; it should not reproduce the coordination logic.

---

## 9. Move Settings/Provider Policy out of HTTP Routes

Any substantive code in settings routes must move to application services.

Examples include:

- testing provider connectivity/configuration;
- determining which characters use a provider;
- determining which characters/personas use a style;
- determining which entities use a voice;
- validating provider updates;
- converting persisted workspace values into editable application settings;
- enforcing delete/update constraints.

After extraction, an adapter should be approximately:

```text
decode input
  -> service method
  -> encode output
```

During migration, HTTP routes may temporarily call the new services. Later the bridge will call the same services.

---

## 10. Refactor `LiveSession` into a Transport-Neutral Session Actor

The current `LiveSession` contains both valid concurrency architecture and browser/SSE concerns.

Split them.

### 10.1 Behavior to preserve

Preserve equivalents for:

- one owner thread per active session;
- exclusive `SessionController` ownership;
- bounded command queue where still useful;
- provider-event draining;
- generation-state processing;
- snapshot/projection creation;
- lifecycle state;
- orderly shutdown;
- mirroring;
- persistence-related transitions;
- cancellation;
- wake notification.

### 10.2 Behavior to remove

Remove concerns whose only purpose is browser/server communication:

- HTTP worker interaction;
- SSE connect/disconnect;
- `SseMailbox`;
- SSE heartbeat;
- browser connection state;
- stream takeover/supersession;
- browser orphan timers;
- reconnect-oriented bookkeeping;
- TCP connection lifecycle.

### 10.3 New abstraction

Rename/reframe as something like:

```text
ActiveSession
SessionActor
RunningSession
```

Conceptually:

```cpp
class ActiveSession {
public:
    CommandFuture submit(SessionCommand command);
    SessionSnapshot snapshot();

    SubscriptionId subscribe(SessionEventSink sink);
    void unsubscribe(SubscriptionId id);

    void shutdown();

private:
    std::thread owner_;
    std::unique_ptr<SessionController> controller_;
    CommandQueue commands_;
    EventSubscribers subscribers_;
};
```

The exact API can remain synchronous at the service boundary if existing queue/deadline behavior makes that simpler, but WebView/UI threads must never block on long provider generation.

---

## 11. Session Operations vs Generation Lifetime

Submitting a prompt must remain a short application command.

Do not hold a bridge RPC open until the model completes.

Target flow:

```text
React
  |
  | invoke session.submit
  v
C++ SessionService
  |
  | enqueue/accept
  v
ActiveSession
  |
  | generation runs asynchronously
  v
Provider worker
  |
  | deltas/events
  v
ActiveSession
  |
  | native bridge events
  v
React
```

Stopping is a separate operation:

```text
session.stop
```

This preserves good cancellation semantics and avoids reinventing long-lived RPC.

---

## 12. Introduce a Common Native Bridge

Create a frontend abstraction:

```ts
export interface NativeBridge {
  invoke<T>(method: string, params?: unknown): Promise<T>;

  on<T>(
    event: string,
    handler: (payload: T) => void,
  ): () => void;
}
```

React components must not know whether the platform is WKWebView or WebView2.

---

## 13. Preserve `ChaClient` as the Frontend Service Boundary

CHA already has a valuable interface boundary in `ChaClient`.

Keep it.

Target frontend layering:

```text
React components
      |
      v
   ChaClient
      |
      v
NativeChaClient
      |
      v
 NativeBridge
```

During migration:

```text
                  ChaClient
                 /         \
        HttpChaClient     NativeChaClient
             |                 |
           HTTP           native bridge
```

This permits incremental migration without rewriting the component tree.

---

## 14. Bridge RPC Protocol

Even though the bridge is process-local, use explicit request/result envelopes.

Benefits:

- correlation of asynchronous calls;
- uniform macOS/Windows behavior;
- logging;
- clean error model;
- testability without a WebView;
- future iOS compatibility.

### 14.1 Request

```json
{
  "id": 42,
  "method": "session.submit",
  "params": {
    "forum_id": "history",
    "session_id": "session-1",
    "input": {
      "text": "..."
    }
  }
}
```

### 14.2 Success

```json
{
  "id": 42,
  "ok": true,
  "result": {
    "accepted": true
  }
}
```

### 14.3 Error

```json
{
  "id": 42,
  "ok": false,
  "error": {
    "code": "session_not_live",
    "message": "That session is not open."
  }
}
```

### 14.4 Event

```json
{
  "event": "session.append",
  "payload": {
    "forum_id": "history",
    "session_id": "session-1",
    "seq": 19,
    "target": {
      "kind": "entry",
      "entry_id": 123
    },
    "text": "..."
  }
}
```

---

## 15. Bridge Error Model

Retain stable semantic error codes.

Examples:

```text
invalid_argument
not_found
session_not_live
session_busy
command_queue_full
operation_cancelled
vault_password_error
provider_error
application_unavailable
internal_error
```

HTTP status codes must disappear from application semantics.

Do not preserve concepts such as `409`, `413`, or `503` in the new bridge simply because the old route used them.

Define a common public error type:

```cpp
struct AppError {
    ErrorCode code;
    std::string public_message;
};
```

Internal exception details go to logs, not the UI.

---

## 16. Bridge Version / Compatibility Handshake

Expose:

```text
bridge.info
```

Example result:

```json
{
  "protocol_version": 1,
  "application_version": "...",
  "platform": "macos"
}
```

A packaged native app should normally ship compatible frontend/native code together, but this handshake is still useful for:

- Vite development;
- diagnostics;
- preventing stale cached frontend code from continuing;
- future migration/versioning.

---

## 17. C++ Bridge Router

Create a platform-neutral layer:

```text
src/bridge/
  bridge_router.h
  bridge_router.cpp
  bridge_protocol.h
  bridge_error.h
  bridge_event_sink.h

  app_bindings.cpp
  session_bindings.cpp
  workspace_bindings.cpp
  settings_bindings.cpp
  vault_bindings.cpp
  audio_bindings.cpp
```

Conceptually:

```cpp
class BridgeRouter {
public:
    BridgeResult invoke(
        std::string_view method,
        const Json& params);

    void set_event_sink(BridgeEventSink* sink);
};
```

The router should only:

- locate an allowed method;
- validate/decode the request;
- call an application service;
- map result/error;
- serialize the reply.

It must not become another location for business rules.

---

## 18. Bridge Input Validation

Removing HTTP does not mean JavaScript input becomes trusted.

Still validate:

- IDs;
- required fields;
- enum values;
- numeric bounds;
- prompt/text size;
- filenames;
- provider configuration;
- resource identifiers;
- state preconditions.

Remove only transport validation such as:

- HTTP content type;
- route escaping;
- query parsing;
- URL decoding;
- Host validation;
- Origin validation for the old API;
- cookie validation;
- HTTP body framing.

Security validation at the application boundary remains mandatory.

---

## 19. Frontend Runtime Validation

The existing HTTP client needs substantial runtime response validation because frontend and server are independently communicating over a network protocol.

In the standalone model:

- UI and native backend ship together;
- TypeScript types and bridge version provide stronger deployment consistency;
- C++ validates bridge inputs;
- persisted/foreign data can still be malformed.

Therefore:

### Keep

- TypeScript static types;
- validation for persisted/foreign/untrusted data;
- targeted validation for critical events;
- explicit bridge protocol version;
- public error mapping.

### Simplify

- duplicated shape guards used only to defend against an independently deployed incompatible HTTP server;
- HTTP-protocol failure classes;
- server-unavailable/reconnect assumptions.

Do this selectively, not mechanically.

---

## 20. Session Updates Without Browser SSE

### 20.1 Target model

```text
React
  |
  +-- invoke("session.submit")
  |
  +-- subscribe to native session events
             |
             v
        ActiveSession
             |
             v
      SessionController
```

No EventSource exists.

### 20.2 Retain snapshot + incremental update semantics

The existing high-level distinction is useful.

Use:

1. authoritative full snapshot;
2. incremental append/update events.

A full snapshot should be sent:

- when the session opens;
- when the UI subscribes;
- after WebView reload/reconnect;
- after a sequence discontinuity;
- when an incremental representation is no longer safe.

Incremental events should be used for:

- streamed answer text;
- streamed reasoning text where applicable;
- narrowly representable state changes.

---

## 21. Event Sequence Numbers

Retain monotonic sequence numbers for incremental events.

Even without TCP packet loss, they provide protection against:

- WebView reload;
- subscription replacement races;
- event bridge bugs;
- accidental reorder;
- duplicated delivery.

If the frontend detects a discontinuity:

```text
stop applying incremental events
    |
    v
request session.snapshot
    |
    v
replace local authoritative state
```

---

## 22. Remove Heartbeats

SSE heartbeat traffic is unnecessary in a process-local bridge.

Do not replace it with IPC heartbeat traffic unless later debugging demonstrates a real need.

Application/WebView lifecycle is the liveness signal.

---

## 23. Event Delivery Threading

C++ worker/session threads must never call WKWebView or WebView2 APIs directly.

Required flow:

```text
session owner / provider notification
            |
            v
      BridgeEventSink
            |
            v
 platform UI-thread scheduler
            |
            v
  WKWebView / WebView2
            |
            v
      JavaScript handler
```

The platform adapter is responsible for marshaling to the platform UI thread.

---

## 24. Event Coalescing

Model providers can produce many small output chunks.

Do not blindly map each tiny provider delta to one WebView IPC message.

Add an event coalescing stage capable of:

- combining adjacent text updates for the same target;
- flushing on a short UI cadence;
- immediately flushing important lifecycle changes;
- falling back to a snapshot when the incremental stream becomes ambiguous.

The interval should be measured. A frame-scale cadence is a reasonable starting hypothesis, not a hard requirement.

---

## 25. Delete Browser Reconnect Machinery

The following concepts should be removed from the production frontend:

- EventSource connection state;
- stream failure recovery;
- snapshot probe after network failure;
- reconnect delay ladder;
- “reconnecting live updates” state;
- replacement event-stream attachment;
- SSE-specific failure classification;
- waiting for a server stream slot;
- network-oriented browser takeover behavior.

These solve problems that no longer exist.

---

## 26. WebView Reload and UI Rehydration

A WebView reload must not destroy the C++ application state.

Target recovery:

```text
WebView reload
    |
    v
initialize NativeBridge
    |
    v
bridge.info
    |
    v
app.bootstrap
    |
    v
reopen/resubscribe active session
    |
    v
receive authoritative snapshot
```

This replaces network reconnect logic with state rehydration.

---

## 27. Multi-Window Semantics

Do not retain browser takeover behavior accidentally.

If multi-window support is added, define it deliberately.

Possible future rules:

- multiple local read-only views;
- one primary mutable view;
- all views subscribe to the same session actor;
- no concept of network “superseding” a remote browser stream.

This is separate from the initial migration.

---

## 28. Frontend Asset Loading

Production React assets should no longer be served by `chaweb`.

Separate:

1. packaged frontend assets;
2. application RPC/events;
3. large/binary application resources.

### 28.1 macOS

Load packaged UI through:

- `WKWebView` local-file loading with a restricted root, or
- a custom `WKURLSchemeHandler`.

Prefer a stable trusted application origin/scheme.

### 28.2 Windows

Use:

- WebView2 virtual-host-to-folder mapping, or
- a controlled resource-request handler.

Do not start a TCP server.

### 28.3 Development

Vite can remain for HMR.

Development model:

```text
Vite -> serves JS/CSS/HTML only

React -> NativeBridge -> C++ application
```

There is no Vite proxy to a CHA HTTP API.

Bridge access in development must be limited to an explicit allowlisted development origin.

---

## 29. Local Binary Resource Channel

Large media should not be sent through JSON/base64 RPC.

Examples:

- cached/generated speech;
- images;
- database/configuration exports;
- future book/media resources.

Introduce a local resource scheme, conceptually:

```text
cha-resource://audio/<opaque-id>
```

or an equivalent WebView-native mechanism.

This is **not** a network server and must not open a socket.

### 29.1 Security requirements

Resource handlers must:

- expose only explicit resource types;
- validate opaque/stable identifiers;
- enforce current vault/session access;
- never interpret arbitrary filesystem paths supplied by JavaScript;
- set appropriate MIME types;
- reject unknown resource classes.

Never expose:

```text
cha-resource://file?path=/Users/...
```

---

## 30. Audio Migration

Current browser-visible cached-audio HTTP URLs should disappear.

Recommended frontend API:

```ts
const source = await client.getEntryAudioSource(
  forumId,
  sessionId,
  entryId,
);

audio.src = source.url;
```

The URL is a local WebView resource URL, not HTTP.

Generation and cache ownership remain native/C++ responsibilities.

---

## 31. Exports, Downloads, and File Saving

Browser download routes should be replaced with explicit application/native operations.

Examples:

```text
session.export
configuration.import
configuration.export
database.upload
database.download
```

Where a user destination is required:

1. React invokes an operation;
2. platform host opens a native dialog;
3. application service writes/reads through a controlled native path;
4. result returns to React.

Do not preserve HTTP downloads just to reuse `<a download>` behavior.

---

## 32. macOS Bridge Adapter

The existing macOS application already embeds WKWebView.

Create a dedicated adapter such as:

```text
MacWebViewBridge
```

Responsibilities:

- install trusted bridge bootstrap JS;
- receive request envelopes;
- forward to common `BridgeRouter`;
- deliver asynchronous result/error;
- deliver application events;
- marshal WebKit operations to the main thread;
- control navigation and external links;
- manage local application resources.

### 32.1 JavaScript -> native

Use WKWebView script message handling.

### 32.2 Native -> JavaScript

Expose one stable receiver, conceptually:

```js
window.__CHA_NATIVE__.receive(message)
```

Native code sends serialized result/event envelopes to this receiver.

### 32.3 External content

Bridge access must remain limited to trusted CHA content.

External links should open:

- in the system browser, or
- in a separate unprivileged view with no CHA bridge.

---

## 33. Windows Bridge Adapter

The existing Windows application already embeds WebView2.

Create:

```text
WebView2Bridge
```

Responsibilities mirror macOS:

- receive JSON web messages;
- forward to `BridgeRouter`;
- post JSON results/events back;
- marshal calls to the correct UI thread;
- restrict trusted origins/content;
- handle local resources;
- handle native dialogs/lifecycle.

React must not contain platform branches for bridge transport.

---

## 34. Future iOS Compatibility

Design the common bridge to remain iOS-compatible.

An iOS port should be able to reuse:

- React application;
- `NativeBridge` TypeScript API;
- `NativeChaClient`;
- bridge envelopes;
- `BridgeRouter`;
- C++ application layer;
- most CHA core code.

Architecture:

```text
WKWebView
    |
Swift / Objective-C++
    |
BridgeRouter
    |
C++ Application
    |
CHA core
```

Avoid common bridge APIs that assume:

- Win32 handles;
- COM objects;
- macOS AppKit objects;
- helper subprocess execution;
- arbitrary desktop filesystem access.

iOS background/network lifecycle policy is a separate problem and should not shape the desktop bridge protocol.

---

## 35. Suggested Native API Surface

Names below are illustrative but demonstrate the desired domain-oriented vocabulary.

### 35.1 Application

```text
bridge.info
app.bootstrap
```

### 35.2 Sessions

```text
session.list
session.create
session.rename
session.delete
session.open
session.close
session.snapshot
session.submit
session.stop
session.cover
session.uncover
session.deleteTurn
session.setDefaultCharacter
session.subscribe
session.unsubscribe
```

Subscription can be implicit in `session.open` if that makes the implementation simpler.

### 35.3 Characters

```text
character.get
character.create
character.update
character.updateDefinition
character.delete

character.file.get
character.file.create
character.file.update
character.file.delete
```

### 35.4 Personas

```text
persona.get
persona.create
persona.update
persona.delete
```

### 35.5 Forums

```text
forum.get
forum.create
forum.update
forum.delete
forum.members.update
```

### 35.6 Providers

```text
provider.list
provider.get
provider.create
provider.update
provider.delete
provider.test
```

### 35.7 Styles and voices

```text
style.list
style.create
style.update
style.delete

voice.list
voice.create
voice.update
voice.delete

voiceInput.get
voiceInput.save
voiceInput.runtime

voiceOutput.get
voiceOutput.save
voiceOutput.runtime
```

### 35.8 API keys

```text
apiKey.list
apiKey.create
apiKey.rename
apiKey.replaceValue
apiKey.delete
```

### 35.9 Vault/database

```text
vault.list
vault.create
vault.update
vault.delete
vault.switch
vault.merge

vault.r2.list
vault.r2.download

database.upload
database.download

configuration.import
configuration.export
```

---

## 36. Bootstrap Contract

Replace HTTP bootstrap routes with:

```text
app.bootstrap
```

Return only the state required to construct the initial UI, such as:

- active vault;
- vault summaries;
- forums;
- character/persona summaries needed for navigation;
- application capabilities;
- relevant runtime settings;
- bridge protocol version.

Avoid an unbounded “dump everything” bootstrap.

Detailed data should still use specific operations.

---

## 37. Secrets and Credentials

Provider/API credentials remain native/C++ state.

The React UI may receive:

- credential IDs;
- display names;
- whether a credential exists;
- non-secret provider configuration.

It should not receive secret values during normal operation.

If editing/replacing a secret is supported, treat it as a specific write-only flow.

Removing localhost networking reduces attack surface but does not justify weakening credential handling.

---

## 38. Security Model

### 38.1 Bridge only trusted application content

Enable privileged bridge functions only for:

- packaged CHA frontend;
- explicit Vite development origin.

### 38.2 Navigation isolation

Remote pages must never inherit CHA bridge privileges.

### 38.3 Explicit method allowlist

Only registered operations can execute.

### 38.4 Validate all parameters

Native code treats JavaScript requests as untrusted input.

### 38.5 Restrict filesystem access

Expose product operations rather than generic paths.

### 38.6 Retain CSP

A restrictive CSP remains useful.

XSS in a WebView with native bridge access is still a security problem.

### 38.7 No localhost security layer

Once the HTTP server is gone, remove:

- runtime access cookies;
- listener authorization;
- Host validation;
- Origin validation for the old API;
- private random localhost token.

Do not reproduce these concepts inside the native bridge.

---

## 39. Timeouts

Separate timeout categories.

### Keep

- provider total timeout;
- provider idle timeout;
- bounded session-command wait where it protects against a stuck actor;
- database-maintenance deadlines where meaningful.

### Remove

- HTTP read timeout;
- HTTP write timeout;
- SSE heartbeat timeout;
- browser connection grace periods tied to network disconnect.

---

## 40. Application Lifecycle and Shutdown

The native process—not a browser connection—owns application lifetime.

Target shutdown:

```text
native quit/close request
      |
      v
stop bridge admission
      |
      v
persist/flush required session state
      |
      v
cancel or settle provider work
      |
      v
stop active session actors
      |
      v
stop audio work
      |
      v
close repositories/store/database
      |
      v
shutdown provider supervisor
      |
      v
destroy C++ Application
      |
      v
destroy WebView/window/process
```

Do not infer session/application shutdown from a browser stream disappearing.

---

## 41. Configuration Cleanup

Remove application settings used only by the internal server, including concepts such as:

```toml
[web]
host = "..."
port = ...
```

Also remove configuration whose only purpose is:

- HTTP thread-pool sizing;
- HTTP pending-request limits;
- HTTP request-body limits where not duplicated as real product input limits;
- HTTP read/write timeout;
- SSE heartbeat;
- browser orphan/disconnect grace.

Keep genuine product limits, including:

- prompt size;
- session limits if still desired;
- provider timeouts;
- provider token limits;
- database/vault configuration;
- logging.

---

## 42. Logging Changes

Remove transport logs centered on:

- server startup/listener address;
- HTTP route;
- HTTP status;
- SSE connect/disconnect;
- heartbeat;
- browser reconnect.

Add lightweight application/bridge diagnostics where useful:

```text
bridge method=session.submit id=42 result=ok duration_ms=3
session event=append forum=... session=...
```

Do not log:

- prompts;
- full responses;
- credentials;
- sensitive configuration.

Preserve provider diagnostic policy.

---

## 43. Build-System Target State

Conceptually:

```text
cha_core
    ^
    |
cha_app
    ^
    |
cha_bridge
   / \
  /   \
cha_macos   cha_windows
```

Possible final targets:

```text
cha_core
cha_app
cha_bridge
cha_macos
cha_windows
```

Remove from the standalone build when no longer referenced:

- cpp-httplib;
- web-server executable;
- listener support;
- server packaging;
- host/port wiring.

Do **not** remove libcurl if it is still used for external model providers.

---

## 44. Expected Frontend Changes

High-impact areas:

```text
webapp/src/api/client.ts
webapp/src/api/events.ts
webapp/src/state/sessionRecovery.ts
runtime/bootstrap wiring
audio resource URL helpers
download/export flows
```

New areas:

```text
webapp/src/native/bridge.ts
webapp/src/api/nativeClient.ts
```

Most React components should remain unchanged if they already depend on `ChaClient` and action interfaces.

---

## 45. Expected C++ Changes

High-impact areas:

```text
src/web/
packaging/macos/
packaging/windows/
CMakeLists.txt
```

New:

```text
src/app/
src/bridge/
```

Moderate:

```text
src/session/
```

Expected low-impact:

```text
src/chat/
src/characters/
src/providers/
src/workspace/
most src/util/
```

---

## 46. HTTP/Web Components Expected to Disappear or Shrink

After application logic has been extracted, review for deletion or major reduction:

```text
src/web/http_server.*
src/web/http_response.*
src/web/route_support.*

src/web/session_routes.*
src/web/lobby_routes.*
src/web/settings_routes.*
src/web/vault_routes.*
src/web/openai_auth_routes.*
src/web/audio_download_routes.*

src/web/sse_stream.*
src/web/sse_mailbox.*
src/web/browser_connection_state.*

src/web/asset_handler.*
```

Also rework `application_runtime.*`; the transport-neutral parts should already have moved to `src/app`.

Do not delete files purely by directory name without inspecting whether application logic remains.

---

## 47. Migration Strategy

The migration should be incremental and keep the application working throughout.

---

## Phase 0 — Inventory Existing Browser Contract

Document every:

- `ChaClient` method;
- HTTP route;
- SSE event;
- binary/download route;
- public error code;
- frontend-generated HTTP URL.

Classify each entry:

```text
application operation
session event
resource
native UI action
transport-only
```

This inventory becomes the migration checklist.

---

## Phase 1 — Extract Application Services

Move business behavior out of HTTP routes.

Temporary state:

```text
HTTP route
   |
   v
App Service
   |
   v
CHA core
```

No user-visible behavior should change.

Add direct application-service tests.

Success condition:

> HTTP handlers mainly parse, call a service, and serialize.

---

## Phase 2 — Separate Session Actor from SSE

Refactor `LiveSession`.

Temporary state:

```text
                       +--> SSE adapter
ActiveSession actor ---+
                       +--> future native event adapter
```

The actor owns session concurrency and semantics.

SSE becomes only an output adapter.

---

## Phase 3 — Add `NativeBridge` and `NativeChaClient`

Add frontend bridge interface and native implementation while retaining HTTP implementation.

Do not modify presentation components unnecessarily.

Runtime can choose the appropriate client.

---

## Phase 4 — Implement C++ `BridgeRouter`

Implement the transport-neutral native RPC dispatcher.

Test without WebView.

Minimum operations:

```text
bridge.info
app.bootstrap
session.open
session.snapshot
session.submit
session.stop
```

Minimum events:

```text
session.snapshot
session.append
```

---

## Phase 5 — macOS End-to-End Native Path

Use existing WKWebView host.

Prove:

1. launch;
2. bridge handshake;
3. bootstrap;
4. open session;
5. submit prompt;
6. stream answer;
7. stop generation;
8. obtain fresh snapshot after UI reload.

At this point the native path should be usable while HTTP remains available as a fallback during development.

---

## Phase 6 — Windows Native Path

Implement the same logical API through WebView2.

Verify parity with macOS.

Do not add platform-specific branches to React except for real capability differences.

---

## Phase 7 — Move All Session Updates off SSE

Switch production standalone runtime to native events.

Remove frontend EventSource dependency and reconnect ladder.

Keep snapshot resynchronization.

---

## Phase 8 — Migrate CRUD and Configuration APIs

Move remaining operations to `NativeChaClient`:

- forums;
- characters;
- personas;
- providers;
- styles;
- voices;
- keys;
- vaults;
- settings.

At completion, normal application flows should use no internal HTTP operations.

---

## Phase 9 — Migrate Audio and Binary Resources

Replace:

- audio HTTP URLs;
- browser downloads;
- binary API responses.

Use native resource handlers and native file UI.

---

## Phase 10 — Remove HTTP Asset Serving

Package React assets directly with native applications.

Production WebView loads local packaged resources.

Vite remains optional for development HMR.

---

## Phase 11 — Delete HTTP Server Runtime

Remove:

- `httplib::Server`;
- listener thread;
- port allocation;
- server start/stop;
- runtime access token;
- browser-facing routes;
- browser SSE transport.

Remove cpp-httplib dependency when no remaining target needs it.

---

## Phase 12 — Remove Server-Specific Frontend Code

Delete/simplify:

- HTTP URL construction;
- `fetch`-based CHA API;
- HTTP status mapping;
- EventSource client;
- session recovery ladder;
- reconnect messages;
- browser takeover state;
- Vite API proxy;
- cached HTTP audio URL construction.

Preserve `ChaClient` as the logical frontend service interface.

---

## Phase 13 — Remove Server Configuration and Packaging

Remove:

- server host/port configuration;
- server executable from release package;
- shell/runtime logic whose only job is to start `chaweb`;
- local firewall/network assumptions;
- HTTP-specific production tests.

---

## 48. Testing Strategy

Complexity reduction must not reduce behavioral confidence.

### 48.1 Keep core tests

Preserve tests for:

- transcript;
- character/model context;
- provider request execution;
- provider cancellation;
- protocol decoding;
- session controller;
- session repository;
- SQLite/SQLCipher;
- workspace;
- database lease;
- configuration;
- concurrency;
- shutdown ordering.

### 48.2 Add application-service tests

Exercise `src/app` without WebView or bridge serialization.

### 48.3 Add bridge-router tests

Drive:

```text
request envelope
  -> BridgeRouter
  -> App service
  -> result/error envelope
```

Test:

- valid operation;
- invalid fields;
- unknown method;
- semantic errors;
- stable error code mapping;
- event subscription;
- sequence behavior;
- unsubscribe;
- shutdown admission.

### 48.4 Frontend client tests

Test `NativeChaClient` against a fake `NativeBridge`.

No C++ process is required for these unit tests.

### 48.5 Session-stream tests

Verify:

- initial snapshot;
- incremental answer text;
- reasoning text if supported;
- generation completion;
- cancellation;
- sequence gap;
- snapshot recovery;
- UI unsubscribe/resubscribe.

### 48.6 Platform smoke tests

For each desktop platform verify:

- packaged UI loads;
- `bridge.info`;
- bootstrap;
- session creation/open;
- prompt submission;
- visible streaming;
- Stop;
- app close/restart;
- persistence;
- native save/open dialog;
- local audio resource.

### 48.7 Provider integration tests

Retain the deterministic local OpenAI-compatible fake provider used for integration testing.

The outbound provider path stays real.

Only the frontend-to-CHA path changes from HTTP/SSE to native bridge.

---

## 49. Tests to Remove or Rewrite

Remove tests whose sole purpose is the deleted transport:

- route matching;
- HTTP request parsing;
- content-type handling;
- HTTP status mapping;
- HTTP worker limits;
- Host checks;
- Origin checks;
- runtime-cookie authentication;
- listener startup;
- port selection;
- SSE heartbeat;
- EventSource reconnect;
- SSE supersession;
- production HTTP cache headers;
- static HTTP asset headers;
- Vite API proxy behavior.

Rewrite end-to-end user flows through the native bridge.

---

## 50. Performance Considerations

Direct IPC should remove multiple layers, but performance must still be designed.

### 50.1 Avoid UI-thread work

Never perform:

- provider network calls;
- database maintenance;
- large file processing;
- session owner work

on the platform UI thread.

### 50.2 Avoid excessive serialization

Normal control/DTO messages can use JSON.

Large binary data should use resource/file mechanisms instead.

### 50.3 Batch high-frequency events

Coalesce streaming text where useful.

### 50.4 Measure WebView bridge overhead

Profile:

- event frequency;
- serialization cost;
- UI update frequency;
- native-to-JS delivery latency.

Do not recreate SSE-like complexity unless measurements justify it.

---

## 51. Main Risks

### 51.1 Hidden application logic in `src/web`

**Risk:** useful behavior is deleted with transport code.

**Mitigation:** extract services and add direct tests before deleting routes.

### 51.2 Threading regressions

**Risk:** native calls bypass session ownership rules.

**Mitigation:** preserve session actor queue and owner-thread invariant.

### 51.3 Event ordering bugs

**Risk:** streamed UI state diverges.

**Mitigation:** sequence numbers plus authoritative snapshot fallback.

### 51.4 WebView UI thread blockage

**Risk:** synchronous bridge implementation freezes UI.

**Mitigation:** bridge is asynchronous; expensive work remains on C++ worker/owner threads.

### 51.5 Large binary messages

**Risk:** JSON/base64 bridge becomes slow and memory-heavy.

**Mitigation:** local resource scheme/native files.

### 51.6 Privileged bridge exposed to remote content

**Risk:** arbitrary page gains native application privileges.

**Mitigation:** strict origin/content control and external-link isolation.

### 51.7 Platform divergence

**Risk:** macOS/Windows bridges evolve differently.

**Mitigation:** one platform-neutral bridge protocol/router and very thin adapters.

### 51.8 Migration scope creep

**Risk:** transport migration becomes a provider/storage/language rewrite.

**Mitigation:** explicitly preserve C++ core, provider architecture, and database format first.

---

## 52. Things Explicitly Out of Scope

Do not combine this migration with:

- rewriting the C++ backend in Go or Rust;
- replacing provider transport with a new networking stack;
- changing the SQLite schema without independent need;
- replacing SQLCipher;
- redesigning character/session semantics;
- introducing a plugin architecture;
- multi-window redesign;
- full iOS lifecycle implementation.

These can be separate projects after the native architecture is stable.

---

## 53. Complexity Expected to Disappear

The completed migration should eliminate entire categories of internal infrastructure:

```text
local HTTP server
route matching
HTTP body parsing
HTTP response generation
HTTP status mapping
listener port management
localhost authentication cookie
Host/Origin policy for application API
HTTP worker pool
HTTP request queue
HTTP read/write timeouts
browser-facing SSE stream
SSE heartbeat
SSE mailbox transport
EventSource
network reconnect ladder
browser-stream takeover
production HTTP asset server
Vite API proxy
browser/server version-deployment problem
```

This is the primary simplification benefit.

---

## 54. Complexity That Must Remain

The following are real application complexity:

```text
session serialization
session owner thread
provider request concurrency
provider-side streaming protocol
generation cancellation
workspace/config validation
SQLite/SQLCipher
vault lifecycle
database maintenance
persistence ordering
audio generation/download
application shutdown ordering
public error semantics
```

Do not remove these merely to make the architecture appear smaller.

---

## 55. Acceptance Criteria

Migration is complete only when all criteria below are satisfied.

### 55.1 Runtime

- standalone CHA opens no local application-server port;
- React runs inside WKWebView/WebView2;
- all UI-to-core application operations use native IPC;
- all core-to-UI live updates use native events;
- production React code does not `fetch` the CHA application API;
- production React code does not create `EventSource` for CHA;
- external provider HTTP/HTTPS still works normally.

### 55.2 User functionality

- application bootstrap works;
- forums work;
- characters and personas work;
- provider settings/test work;
- sessions can be created/opened/renamed/deleted;
- prompts can be submitted;
- answers stream incrementally;
- reasoning presentation works where applicable;
- Stop works during generation;
- multicast works;
- cover/uncover works;
- delete-turn works;
- default-character changes work;
- session state persists across restart;
- vault switching works;
- protected/encrypted vault behavior stays compatible;
- import/export works;
- audio/voice features work without internal HTTP URLs.

### 55.3 Architecture

- `src/app` has no dependency on HTTP;
- `SessionController` has no dependency on WebView/native UI;
- React components depend on `ChaClient`, not native platform APIs;
- platform host depends on bridge/application abstractions, not session internals;
- WebKit/WebView2 code is confined to platform adapters;
- `BridgeRouter` can be tested without launching a WebView.

### 55.4 Build

- cpp-httplib is no longer required by the standalone application;
- release package contains no application HTTP server;
- host/port settings are gone;
- server-specific tests and packaging are gone or explicitly isolated from the standalone product.

### 55.5 Security

- bridge is available only to trusted CHA frontend content;
- external navigation cannot invoke privileged bridge operations;
- generic filesystem access is not exposed;
- secrets remain native;
- bridge inputs are validated;
- CSP and frontend injection protections remain appropriate.

---

## 56. Recommended First Native Vertical Slice

The first implementation should prove the entire architecture with the smallest useful surface.

### Commands

```text
bridge.info
app.bootstrap
session.open
session.snapshot
session.submit
session.stop
```

### Events

```text
session.snapshot
session.append
```

### Test scenario

1. launch native app;
2. load React UI;
3. complete bridge handshake;
4. bootstrap;
5. open one existing session;
6. submit a prompt;
7. receive streamed text from deterministic local provider;
8. stop generation;
9. reload WebView;
10. request new snapshot and recover UI state.

Once this works on macOS:

1. prove the same contract on Windows;
2. migrate remaining operations;
3. migrate resources/audio;
4. delete SSE;
5. delete HTTP API;
6. delete HTTP asset serving;
7. remove server packaging/configuration.

---

## 57. Recommended Final Dependency Shape

Native:

```text
cha_macos ─────┐
               ├──> cha_bridge ──> cha_app ──> cha_core
cha_windows ───┘
```

Core:

```text
cha_core
  ├── chat
  ├── characters
  ├── providers
  ├── session
  ├── workspace
  └── util
```

Frontend:

```text
React components
      |
      v
   ChaClient
      |
      v
NativeChaClient
      |
      v
 NativeBridge
      |
      v
platform WebView adapter
```

No core/domain dependency points back toward bridge, WebView, or platform code.

---

## 58. Final Target State

CHA should no longer be conceptually a web server embedded inside a desktop application.

It should be a native standalone application whose presentation layer is implemented in React:

```text
                    CHA
                     |
         +-----------+-----------+
         |                       |
     React/WebView          C++ application
         |                       |
         +----- native IPC ------+
                                 |
                         domain + providers
                         persistence + OS
```

The WebView is the presentation runtime.

The C++ process is the application runtime.

The native bridge is a narrow, typed, asynchronous process-local interface between them.

The internal HTTP server, browser-facing SSE transport, localhost security machinery, and browser/network reconnect model are removed entirely.
