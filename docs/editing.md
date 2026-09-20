# Editing workspace entities

Use this guide when adding or changing an entity editor in the native
application. The [codebase tutorial](tutorial.md) explains the wider ownership
and bridge model; the [maintainer guide](MaintainerGuide.md) describes exported
workspace files.

## Follow an existing interaction

When a workflow resembles an existing one, reuse its interaction shape:

1. Put a small “New …” row at the top of the relevant list.
2. Open a focused form and validate its name.
3. Create the entity, insert the returned summary, and select its detail screen.

Keep entity-specific behavior explicit. Session creation opens a live
conversation; persona creation only creates configuration. Use control labels,
actionable errors, and validation without explanatory text that repeats the
obvious operation.

## Keep workspace ownership explicit

The native entry point is an `Application` operation. Context-bound operations
must take the caller's nonzero epoch and use the existing admission check. Do
not obtain the current epoch later to make a stale request appear current.

The configuration store owns its immutable workspace. Hold one
`WorkspaceConfigStore::snapshot()` result while using references and pass
`const Workspace&` into helpers together with their other dependencies. There
is no global workspace publication to arrange in production or tests.

## Persist through `WorkspaceConfigStore`

SQLite configuration is authoritative. Do not edit the original import
directory or mutate a published `Workspace` directly. Use the store's existing
edit path:

1. Modify the materialized private tree through a small workspace write method.
2. Load a candidate to validate the complete result.
3. Collect its configuration rows.
4. Replace the rows in one transaction.
5. Publish the candidate only after commit.

This gives creations and edits the same rollback and restart-required behavior.
Build the minimum valid directory, TOML, and Markdown files. Validate names and
collisions before writing where possible. Generate stable IDs in native code
when the form only asks for a display name, and validate them against the
workspace's reserved-ID and collision rules.

Use the edit result's affected forum IDs to update live sessions. Presentation
changes use `refresh_affected_sessions`; changes requiring new generation
configuration use `invalidate_affected_sessions`, which requests `reloading`
for every affected live controller, including background sessions. Provider
requests retain their original inputs until cancellation. All controller work
belongs on the shared `SessionRuntime` thread, never on the editor's caller.

## Keep the native contract synchronized

For a new operation, update the pieces it actually needs:

- Add its `Method` enum, name, and control/epoch/context-change policy in
  `src/bridge/bridge_protocol.*`.
- Add the admitted application operation and its workspace/settings helper.
- Dispatch through `workspace_dispatch.cpp` or `settings_dispatch.cpp` for
  ordinary synchronous operations. Keep asynchronous completion in the router
  when it needs router-owned state. Reuse `request_params.h` validation.
- Define request/result DTOs in `resources/dto.yaml`, then run
  `npm run api-types` from `webapp/`.
- Add the typed `ChaClient` method and its `nativeClient.ts` mapping. Keep the
  frontend method policy and shared method-policy fixture in sync with C++.
- Return canonical entity data and reuse the native error envelope.

The DTO schema generates types; it does not generate dispatch or operation
policy. Test method names, parameter shapes, epoch behavior, and results at
these boundaries. There are no HTTP routes or verbs to add.

Use shared runtime guards for repeated structures. Validate data used by the
consumer, particularly snapshots and append targets, without copying every DTO
shape into a second set of unused validators. Keep `bridge.info` as an exact
protocol-version check; no compatibility negotiation is needed.

## Update visible state immediately

After a successful mutation, update the relevant bootstrap roster and current
visible references. `webapp/src/state/entityUpdates.ts` centralizes summary
construction and updates shared by character, persona, and forum mutations.
Extend that helper when a new current reference needs the same update.

Keep historical transcript attribution intact. A persona rename updates the
current roster and forum summary, not the author names stored in past entries.

`view.ts` groups inspection state by entity. Remembered selections, the current
forum, and active conversation are separate. Preserve that behavior when
clearing or switching views. Writability comes from the canonical detail
response. Character settings writability is distinct from definition editing;
built-in Assistant can allow the former while forbidding the latter.

Keep detail data and mutations in the detail screen. Reuse `EditableTitle` and
`DetailActions` from `components/DetailActions.tsx` for rename and
edit/upload/delete controls. Those controls own their dialogs; the screen
supplies canonical values and save/delete callbacks and updates its loaded
detail after success. `TopBar` only owns navigation presentation and the
sidebar toggle. It should not fetch entity data or coordinate detail mutations.

For simple lists, `useLoad(client, loader, failureMessage)` provides data, load
error, retry, and stale-completion suppression. Use a stable loader function so
renders do not repeatedly start loads. Keep mutation errors separate. Detail
editors, drafts, subscriptions, and context transitions may need their own
effects rather than being forced through the list hook.

## Exercise native file selection and saving

React file inputs run inside the native WebView. On macOS, the installed
`WKUIDelegate` must implement
`webView(_:runOpenPanelWith:initiatedByFrame:completionHandler:)`. It opens an
`NSOpenPanel`, honors selection flags, and completes with URLs or `nil` on
cancellation. Without it, a DOM test can pass while the real picker does not
appear.

The control can remain an icon button that activates a hidden file input. Once
selected, the frontend reads the file and sends its contents through the typed
mutation. Check the corresponding WebView2 behavior on Windows.

For exports, use the existing native save path. Choose the destination on the
platform UI thread, then revalidate the captured context and write off that
thread. Do not allow a save dialog opened in one vault to silently export data
from another after a switch.

## Verify the changed boundaries

- Parser/bridge tests cover valid and invalid parameters, errors, and epochs.
- Application/store tests inspect durable rows and the published snapshot.
- UI tests cover validation, immediate summary updates, selection, stale loads,
  and mutation failures.
- Wire fixtures and malformed-value tests cover any changed response shapes.
- `make web-check` checks generated types, TypeScript, and frontend tests;
  `make web-stage` verifies the production frontend build.
- Build C++ and run the affected application, workspace, or bridge suites.
- Compile and exercise a native host when changing pickers, save dialogs,
  resource loading, reload, or shutdown.

DOM tests do not prove that native panels appear or that UI-thread callbacks
obey their lifetime contracts.
