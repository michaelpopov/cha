# Persist View menu choices

## Purpose and status

Remember the user's View menu choices and restore them when CHA starts again.
Save choices automatically when they take effect. Do not add a Save command.

This document is a technical design. It does not implement the feature.
It describes the current working tree, including the Russian menu item.

## Required behavior

- Persist all seven checkable View menu choices on macOS and Windows.
- Restore the saved choices before the renderer's first render. The native
  menu checkmarks and the rendered controls must agree.
- Keep the current defaults for a new installation or a missing setting.
- Apply menu choices immediately, with the current navigation and note-save
  checks. Save the result after the renderer applies it.
- Use one set of preferences per local CHA configuration directory. Changing
  vaults does not change these preferences. They are not vault content and do
  not travel through vault export, import, merge, download, or R2 upload.
- Keep the application usable if preferences cannot be read or written.
  Report a warning through native diagnostic output. Do not show a dialog or
  undo a successful View change because saving failed.

The scope includes Split view because it is a checkable View choice. Russian
stores button visibility only. It does not store the transliteration typing
mode. This design supersedes the nonpersistent View lifetime specified in
`specs/russian.md`; its other behavior remains unchanged.

## Settings and defaults

Use the existing `view_state` field names in the stored JSON and the native
startup payload. Do not store the entire renderer state.

| View menu item | JSON field | Renderer field | Default |
| --- | --- | --- | --- |
| Split view | `split` | `layoutMode` | `false` / `single` |
| Sidebar | `sidebar` | `sidebarOpen` | `true` |
| Pictures | `pictures` | `pictureOpen` | `false` |
| Toolbar | `toolbar` | `notesToolbarVisible` | `true` |
| Chat toolbar | `chat_toolbar` | `chatToolbarVisible` | `true` |
| Notes prompt | `notes_prompt` | `notesPromptVisible` | `true` |
| Russian | `russian` | `russianVisible` | `false` |

Persist the selected flag even when its containing screen is not visible.
For example, Pictures can be checked when there is no character picture to
show. Do not replace a preference with the current DOM visibility.

Do not persist window size or position, panel widths, sidebar tab, selected
picture character, active vault credentials, open screen, selected note or
session, editor state, or unsent drafts. Keep the current startup conversation
selection. Restoring Split view changes the layout, not the selected content.

## Current implementation

- `webapp/src/state/view.ts` defines the seven flags and their defaults in
  `initialAppState`. The reducer changes the flags in memory. Bootstrap and
  vault changes normally preserve them through state spreads.
- `webapp/src/components/App.tsx` initializes the reducer with
  `initialAppState`. It installs `__CHA_NATIVE_SET_LAYOUT__` and reports the
  applied flags in an effect through `reportNativeLayout`.
- `webapp/src/api/nativeLayout.ts` sends the existing host-only `view_state`
  message. It includes all seven booleans and the connection ID.
- `packaging/macos/native_bridge.swift` validates this message and calls
  `onLayoutChanged`. `packaging/macos/main.swift` updates the menu checkmarks.
  Its bootstrap script runs at document start.
- `packaging/windows/main.cpp` validates the same message and updates the menu.
  It serves a per-document `/__cha-bootstrap.js` before the application module,
  for both packaged assets and the supported development origin.
- The application already has a local configuration directory. On macOS it
  is `~/Library/Application Support/CHA`. On Windows it is
  `%LOCALAPPDATA%\CHA\config`, or the test host's selected configuration root.
- `app.toml` stores application configuration, including vault selection.
  Other root-level `.toml` files are discovered as vault definitions by
  `load_configuration_directory` in `src/app/application_config.cpp`.

## Design choice

Let each native host read and write one small `view.json` file in its existing
configuration directory. Reuse the existing bootstrap script to supply the
initial values, and reuse `view_state` to save applied changes.

This keeps the feature at the boundary that already handles the native View
menu. It needs no runtime command, C ABI addition, database table, API endpoint,
DTO/OpenAPI change, or bridge protocol version change. The two native hosts
have short platform-specific file operations and the same seven-field contract.

Do not extend `app.toml`: View saves must not rewrite vault selection or other
application configuration. Use `.json` so the file is not discovered as a
vault definition. Do not use renderer local storage: the host already knows
the configuration directory and must initialize native checkmarks as well as
the renderer, independent of the webview's storage profile and origin.

The renderer remains authoritative for the live applied choices. The file is
the startup source. A native host keeps one current View value so it can build
bootstrap payloads, compare reports, and update checkmarks. This value is not
a second renderer store or a general preferences service.

## File format and loading

Example `view.json` with the current defaults:

```json
{
  "split": false,
  "sidebar": true,
  "pictures": false,
  "toolbar": true,
  "chat_toolbar": true,
  "notes_prompt": true,
  "russian": false
}
```

Store only these fields. Do not write `native_action`, `connection_id`, context
epochs, timestamps, vault names, or content identifiers. No schema version is
needed for this fixed set of independent booleans. A future optional boolean
can use its default when absent.

Loading rules, identical on both platforms:

1. Start with the seven defaults in the table.
2. A missing file is normal. Return the defaults without a warning.
3. If the file cannot be read, contains invalid JSON, or has a non-object root,
   warn and return the defaults. Do not fail application startup.
4. For each known field, accept only a JSON boolean. If absent, keep its
   default. If present with another type, warn and keep that field's default.
   Keep valid sibling fields. Do not coerce `0`, `1`, strings, or `null`.
5. Ignore unknown fields and warn. Do not block startup for unused or obsolete
   preferences. Subsequent saves contain only the seven known fields.
6. Do not repair, delete, or rewrite the file during loading. A later applied
   View change can replace it with valid current preferences.

Read once per native application launch. Reuse that instance's current View
value when its document reloads or its renderer recovers. Do not watch the file
or reload it on a vault switch.

Do not create `view.json` merely to store defaults. In particular, do not create
it before the existing empty-directory configuration bootstrap has completed;
that bootstrap checks whether the configuration directory is empty.

## Startup and renderer initialization

Extend the existing native bootstrap with this assignment:

```javascript
window.__CHA_NATIVE_INITIAL_VIEW_STATE__ = {
  split: false,
  sidebar: true,
  pictures: false,
  toolbar: true,
  chat_toolbar: true,
  notes_prompt: true,
  russian: false
};
```

The values come from the native host's current View value. Use JSON serialization
to create the object literal; do not insert file text into executable JavaScript.
Include only normalized known booleans. Retain the existing trusted-origin,
main-frame, and connection checks.

In `webapp/src/api/nativeLayout.ts`:

- Declare the optional global as `unknown` at the input boundary.
- Add a small reader that maps valid known booleans to renderer preference
  fields. Map `split` to `layoutMode: 'split' | 'single'`.
- Return a partial preference object if the global is absent or incomplete.
  Reject arrays and other non-object values. Ignore invalid members; defaults
  are applied by the state initializer. Do not copy arbitrary object members.
- Import `ViewPreferences` from `state/view.ts` with a type-only import. Define
  this type once in that state module as a `Pick<AppState, ...>` of exactly
  the seven fields. Keep the state module independent of the native API module,
  window access, and file operations.

In `webapp/src/main.tsx`, read the bootstrap value before `createRoot().render`
and pass the result to `App` as an optional `initialViewPreferences` prop.
`App` uses a lazy reducer initializer that returns a fresh state based on
`initialAppState`, overriding only those seven fields. The initializer can live
in `state/view.ts` and must be pure. Keep `initialAppState` unchanged so it
remains the default for tests and other consumers.

Select each preference explicitly and use its default when the supplied value
is `undefined`. Use nullish fallback, not boolean OR: a saved `false` is a valid
choice. Do not spread the prop into application state. The startup reader
omits invalid fields rather than returning them with invalid values; the
initializer accepts the resulting typed `Partial<ViewPreferences>`.

Do not restore preferences with a mount effect or by sending seven toggle
commands after bootstrap. Those approaches first render defaults, can report
them back to the host, and can run navigation guards or toggle a value twice.
Initialize the reducer directly. React StrictMode can call the initializer
again without side effects. The first `view_state` report must already contain
the restored values.

Keep the provider and editor tree stable. The existing
`TransliterationProvider controlsVisible={state.russianVisible}` continues to
control Russian button visibility; its typing mode still starts disabled.

## Saving applied changes

Keep the current native menu request strings and renderer reducer actions.
In particular, retain the note-leave checks for a switch from Split view to
Single view. A cancelled or failed switch does not change the saved choice.

Change the existing `reportNativeLayout` effect in `App.tsx` from `useEffect`
to `useLayoutEffect`, retaining the same seven dependencies and payload. Send
the report during the commit that applies the choices, before the updated
screen is painted. This is a small message post; file I/O stays in the native
host. Do not send reports from the reducer, during render, or from effect
cleanup.

This timing matters for an immediate normal quit. The current
`useNoteLeave.prepareClose` can send `close_reply: ready` immediately when there
is no note to save. Leaving the View report in a deferred effect permits that
reply to precede the latest report, even after the screen changes. A layout
effect posts the committed choices before a later close callback can reply.
React documents the timing distinction in
[useEffect](https://react.dev/reference/react/useEffect) and
[useLayoutEffect](https://react.dev/reference/react/useLayoutEffect).

Use this flow on both platforms:

```text
native View command
  -> existing renderer handler and guards
  -> reducer commits the change
  -> view_state report from the layout effect
  -> native validation for the current connection
  -> compare with the host's current View value
  -> update that value and native checkmarks
  -> replace view.json if the value changed
```

Compare all seven booleans. If a valid report equals the current host value,
update checkmarks if needed but do not write a file. This prevents initial
reports, StrictMode effect replay, and renderer reloads from writing defaults
or overwriting a change made by another instance.

On a changed valid report, first update the current host value and checkmarks,
then try to save the full seven-field snapshot. Perform the small write in the
native UI message handler. Add no worker, timer, debounce, queue, or retry loop.
Reports in one host are processed in order. Do not save an optimistic menu
toggle or an unvalidated report. Invalid or stale-connection messages must
change neither the current value, the checkmarks, nor the file.

If saving fails, retain the applied in-memory value and warn. A later actual
View change attempts another full snapshot save. An identical report does not
retry. Reloading the renderer within this host still uses its current value;
restarting the application uses the last successfully saved file.

Save on change, not on quit or unmount. Closing an unchanged instance must not
overwrite newer preferences saved by another instance. No change to the close
negotiation is needed. A normal restart restores changes whose reports have
been handled and whose writes succeeded, including a normal quit immediately
after a visible change. Preserve the ordering of messages from the same
document: its View report must be handled before its subsequent ready reply
allows shutdown. Verify this in both native hosts. A forced termination before a report
reaches the host can retain the previous choice. Do not promise recovery of an
unprocessed event or durability through sudden power loss.

### File replacement

Write a complete UTF-8 JSON object plus a trailing newline to a temporary file
in the same directory, then replace `view.json`. Readers must see a complete
old or new document, never a partially written document. Leave the previous
file intact if writing or replacement fails. Remove a temporary file from the
failed operation when possible.

- On macOS, use Foundation's atomic data write to the fixed file URL.
- On Windows, use a unique temporary filename, close and check the stream, and
  replace the destination with `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`.
  The same path must work when the destination does not yet exist. Do not use
  remove-then-rename, which creates a gap and can lose the previous preferences.
  Include the process ID and a process-local sequence in temporary names so
  simultaneous app instances do not share a temporary file.
- Keep files under the existing private application directory and its current
  access protections. Store no secrets. Do not create another data directory.
- Warn with the operation, path, and available error detail through the native
  host's diagnostic output. Do not add a new logging subsystem or error dialog.

## Native host changes

### macOS

- Define a small seven-boolean View value and its JSON load/save helpers in
  `packaging/macos/native_bridge.swift`. This file is already compiled by the
  application and native test host. Reuse the existing strict Foundation
  boolean checks (`CFBooleanGetTypeID`) so numeric JSON values are not accepted.
- Keep one current View value in `ApplicationDelegate`, loaded from
  `supportDirectory/view.json` before installing the menu. Loading a missing
  file must not create the support directory or preferences file.
- Initialize the seven menu checkmarks from that value in `installMenus`.
- Extend `nativeBootstrapJavaScript` and `nativeBootstrapUserScript` to accept
  the View value. Let `ChaNativeBridgeReceiver` obtain the latest value from
  the delegate through a small callback when preparing a document replacement.
  Use defaults if a standalone test receiver has no callback.
- In the existing `onLayoutChanged` callback, form the reported value, compare
  it with the current value, update the menu, and save only an actual change.
  Keep persistence errors out of the fatal startup error path.
- Wire the test host to the same helpers using its temporary configuration
  directory. It must never read or write the user's production `view.json`.

### Windows

- Define the equivalent View value and JSON load/save helpers beside the native
  View/bootstrap code in `packaging/windows/main.cpp`. Reuse `nlohmann::json`.
- Keep one current value in `WindowsApplication`. Load
  `config_directory_ / L"view.json"` after the existing application-data setup
  and before starting the webview. Apply its checkmarks to the existing menu
  then; do not reorder the special startup-close test paths to add preferences.
- Pass this value to `native_bootstrap_script` when serving the bootstrap
  resource. Preserve its current per-document connection ID and no-store
  response. Do not move bootstrap delivery to an asynchronous script injection.
- After the existing complete boolean and connection validation in
  `handle_native_action`, compare and update the current value, update
  checkmarks, and save changed preferences. Keep malformed messages consumed
  as they are today.
- Use the existing `--user-data` test root for isolation. No registry entry,
  production launch option, or environment setting is needed.

Keep platform implementations local and small. A shared persistence service
across Swift and C++ would require a new bridge surface without serving another
current feature.

## Multiple instances and compatibility

The Window > New Instance command remains available. Each new instance reads
the most recently saved file. Existing instances keep their live choices and
do not change when another instance writes the file.

The last successful file replacement wins for the whole seven-field snapshot.
Do not merge individual flags between running instances. For example, if A
saves Sidebar off and B later changes Pictures from an older configuration,
B's complete snapshot becomes the next launch configuration. Starting,
reloading, or closing B without a View change does not overwrite A's save.
Atomic replacement prevents torn JSON; it does not provide live synchronization.
No file watcher, lock manager, conflict dialog, or cross-instance messaging is
required for this behavior.

There is no migration from earlier CHA versions because they stored no View
preferences. Missing fields use current defaults. An older application that
does not implement this feature leaves the JSON file alone. Unknown future
fields are ignored with warnings and can be dropped on a later save.

The separate ChaWeb application and Linux daemon have no native View menu and
need no changes. These preferences are not added to vault configuration or
Assistant-editable application settings.

## Implementation scope

Expected source changes:

| File | Change |
| --- | --- |
| `webapp/src/api/nativeLayout.ts` | Read and validate the startup View payload; declare the optional global. |
| `webapp/src/state/view.ts` | Add a pure initializer and a type for the seven preferences; retain existing defaults. |
| `webapp/src/main.tsx` | Read the initial payload and pass it to `App`. |
| `webapp/src/components/App.tsx` | Accept initial preferences, use the lazy reducer initializer, and report committed choices in a layout effect. |
| `packaging/macos/native_bridge.swift` | View value, JSON helpers, and bootstrap payload. |
| `packaging/macos/main.swift` | Load, initialize checkmarks, and save applied changes. |
| `packaging/windows/main.cpp` | Equivalent file, bootstrap, and menu wiring. |
| Renderer and native tests | Validate initialization, restart, storage failures, and unchanged behavior. |

No menu labels, order, controls, spacing, styles, or screen layouts change.
The only visible difference is that the existing choices are restored.

## Validation and acceptance

Use focused tests for the persistence contract and the existing native host
harnesses. Do not add live provider API checks.

### Renderer checks

1. Test the startup reader with all seven booleans, `false` values, partial
   payloads, missing global, arrays, unknown fields, and invalid member types.
   Verify that arbitrary fields cannot enter application state.
2. Test the pure initializer with defaults and saved values. Verify it does
   not mutate `initialAppState` and changes no other application fields. Check
   that explicit `undefined` uses defaults and explicit `false` is preserved.
3. Mount `App` with a saved mix of values, including Split view and Russian.
   Verify the first `view_state` report has that mix, with no preceding default
   report. Repeat under StrictMode. Verify Russian typing still starts off.
4. Verify bootstrap refresh, navigation, and vault reset/refresh retain the
   preferences. Verify cancelled Split-to-Single navigation leaves the flag
   and subsequent report unchanged.
5. Keep existing toggle tests. Reset any injected startup global between tests;
   isolated App tests can supply the prop directly without native file access.
6. Check the ordering of a committed View report and a subsequent immediate
   ready-to-close reply, including the path with no active note. Do not add a
   delay or wait for the persistence file before requesting close.

### Native and restart checks

1. With an empty temporary configuration, start with the current defaults.
   Verify no `view.json` is created until a View choice actually changes.
2. Change all seven flags to a nondefault combination. Wait for the native
   report to be processed and check the saved file. Quit and launch a new host
   process with the same root. Verify the rendered state and actual native menu
   checkmarks. Use both true and false values, then change them back and restart.
3. Reload the document and exercise renderer recovery. Verify the first report
   and controls use the host's current choices, with no write for an equal
   report. Ensure the macOS test host uses the production persistence helpers;
   its renderer hook alone cannot prove production menu initialization.
4. Check absent fields, invalid fields with valid siblings, unknown fields,
   malformed JSON, and a non-object root. Verify defaults and warnings as
   specified. Startup must remain usable and loading must not rewrite the file.
5. Force a save failure using a controlled filesystem condition in the test
   directory. Verify live controls and checkmarks change, a warning is emitted,
   and the previous valid file remains readable. Remove the failure, make
   another actual View change, and verify that the full new snapshot is saved.
6. Submit malformed, nonboolean, incomplete, and stale-connection `view_state`
   messages. Verify they cannot change the file or native checkmarks.
7. Verify a cancelled layout transition does not write a new preference, and a
   successful transition does. Open another vault and verify the same flags.
8. Launch two instances on one test configuration. Verify new instances read
   the file, unchanged reports/reloads/close do not write, and a later actual
   change saves the complete snapshot. Concurrent writes must leave valid JSON.
9. Inspect the native View menu on macOS and Windows. Verify all seven restored
   checkmarks and current request behavior. Do not rely only on renderer hooks.
10. Change a View choice and request normal quit as soon as the corresponding
    DOM change is visible, without first waiting for the report or saved file.
    Restart with the same root and verify that choice was saved. Exercise the
    no-active-note path, which can reply ready immediately, and check that the
    native host processes the View report before accepting the ready reply.

Use temporary directories throughout native tests. Extend
`tests/native/ui.js`, the macOS test host/probes and launch scripts, and
`tests/native/windows/webview2.spec.ts` as needed for two-process restart checks.
Existing default-state smoke checks must start with a fresh test root.

During implementation, run:

```sh
npm --prefix webapp run typecheck
npm --prefix webapp test -- src/api/nativeLayout.test.ts src/state/view.test.ts src/components/App.test.tsx
npm --prefix webapp run build
```

The `nativeLayout.test.ts` suite is new. Build and run the affected native host
checks on each target platform, including a real process restart. Record any
platform checks that cannot be run. This design-only change requires no
application build or provider access.
