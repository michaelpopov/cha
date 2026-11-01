# Russian controls in the View menu

## Purpose

Add a checkable **Russian** item to the native **View** menu. It controls the
visibility of every existing `A→Я` button for Latin-to-Russian transliteration.
The item is unchecked by default, and the buttons are hidden by default.

This document specifies the design. It does not implement the feature.

## Behavior

- Append **Russian** after **Notes prompt** in the View menu on macOS and
  Windows. Use no new menu separator or keyboard shortcut.
- Checked: show the existing `A→Я` buttons wherever their containing controls
  are rendered. Preserve each button's current styling and disabled state.
- Unchecked: remove those buttons from the visible layout, keyboard tab order,
  and accessibility tree. Do not leave an empty button slot or its spacing.
- Apply a change immediately to all mounted screens and dialogs. Screens and
  dialogs opened later use the same setting.
- Keep all other icons, controls, and View options unchanged. Checking Russian
  does not open a hidden toolbar, prompt, sidebar, or panel.
- Preserve text, caret position, editor instances, and unsaved drafts when the
  setting changes. Do not reload the page or remount the provider or editors.

### Visibility and typing mode

The menu controls **button visibility only**. The existing global transliteration
mode remains a separate value:

| Action | Button visibility | Transliteration mode |
| --- | --- | --- |
| Check Russian | Visible | Unchanged |
| Uncheck Russian | Hidden | Unchanged |
| Click a visible `A→Я` button | Unchanged | Toggle, as today |
| Press Ctrl+Shift+Y | Unchanged | Toggle, as today |

Thus, hiding the buttons while transliteration is active leaves it active.
Ctrl+Shift+Y continues to work while the buttons are hidden. Rechecking Russian
shows the current pressed state. The existing voice-input language selection
also remains tied to the typing mode, not to button visibility.

This follows the requested visibility behavior without adding a second way to
enable or disable Russian typing. It does not change the transliteration
mapping, paste/drop handling, IME handling, or support for normal Cyrillic input.

### Lifetime

Use the same lifetime as the existing View flags: one renderer/app instance.
Start unchecked after app launch, a new app instance, or a full page reload.
Keep the value during normal navigation, layout changes, bootstrap refreshes,
and vault switches within that instance.

Do not add persistence, a vault setting, local storage, or cross-instance
synchronization. Remembering the choice across launches is outside this design.

## Visual proposal

The existing menu entries keep their current order and checkmarks. The examples
below show the new last row and an existing field in each state. `✓` is the
native menu checkmark; an empty checkmark position means unchecked.

```text
Default / unchecked                 Checked

View                                View
  Split view                          Split view
✓ Sidebar                           ✓ Sidebar
  Pictures                            Pictures
✓ Toolbar                           ✓ Toolbar
✓ Chat toolbar                      ✓ Chat toolbar
✓ Notes prompt                      ✓ Notes prompt
  Russian                           ✓ Russian

Name                                Name
[ Field text                  ]     [ Field text           ] A→Я

Prompt status row                   Prompt status row
[ Existing status/actions     ]     [ Existing status/actions ] A→Я
```

`A→Я` keeps its existing appearance and position when shown. No new panel
titles, legends, explanatory labels, or helper text are needed.
The Notes prompt status row currently contains only `A→Я`. When Russian is
unchecked, that entire empty row collapses, including its top margin. The Chat
status row keeps its context text and other actions.

## Existing implementation

- `webapp/src/components/TransliterationMode.tsx` owns the shared typing mode,
  Ctrl+Shift+Y listener, `useTransliteration`, `TransliterationToggle`, and
  `TransliteratingInput`.
- `App.tsx` wraps the application in one `TransliterationProvider`. Context also
  reaches dialogs rendered through React portals.
- All current `A→Я` buttons are rendered by `TransliterationToggle`. Consumers
  include Chat and Notes prompt status rows through `PromptInput`, text editor
  dialogs through `TextEditorDialog`, inline renaming through `DetailActions`,
  and short text fields through `TransliteratingInput` in entity screens,
  settings, and the sidebar.
- View flags already live in `webapp/src/state/view.ts`. Native menu actions
  reach `App.tsx` through `window.__CHA_NATIVE_SET_LAYOUT__`; the renderer
  reports applied flags through `reportNativeLayout` in `api/nativeLayout.ts`.
- macOS implements the menu in `packaging/macos/main.swift` and receives View
  state in `packaging/macos/native_bridge.swift`. Windows handles both in
  `packaging/windows/main.cpp`.

## Technical changes

### Renderer state and native menu messages

1. Add `russianVisible: boolean` to `AppState`, initialized to `false`.
2. Add the reducer action `toggle-russian`, which flips only this value.
   Existing navigation and vault transitions preserve it through their normal
   state updates.
3. Handle the native request string `toggle-russian` in the existing layout
   handler in `App.tsx`, beside the other visibility requests and before the
   guarded single/split layout path. This action needs no save operation or
   navigation guard.
4. Append a `russian: boolean` field to the existing `view_state` message.
   Extend `reportNativeLayout` and its calling effect to include the current
   value and report changes. Retain the existing argument order for other flags.
5. Both native hosts derive the Russian checkmark from the renderer's report,
   as they do for other View options. Do not toggle the native checkmark
   optimistically or maintain a second authoritative value in the host.

The flow is:

```text
View > Russian
  → existing native setLayout("toggle-russian")
  → existing renderer layout handler
  → dispatch({ type: "toggle-russian" })
  → shared button visibility updates
  → view_state with russian: true/false
  → native menu checkmark updates
```

This extends the existing host-only View message. It needs no runtime command,
database change, DTO/OpenAPI change, or native protocol version change. Keep
the current connection checks and message validation.

### Shared controls

- Give `TransliterationProvider` a `controlsVisible` prop with a default of
  `false`; pass `state.russianVisible` from `App.tsx`.
- Expose that prop in the existing context alongside `enabled` and `toggle`.
  Keep `enabled` in its current provider state. Do not copy visibility into a
  second state variable or add another provider.
- Have `TransliterationToggle` read the context and return `null` when controls
  are hidden. Without a provider, visibility also defaults to false. Standalone
  callers that need visible controls can use the existing provider with
  `controlsVisible={true}`.
- Keep `useTransliteration` conversion, focus handling, and its standalone mode
  fallback unchanged. Button visibility must not gate conversion or shortcuts.
- Keep the provider at its current tree position without a visibility-based
  key, so toggling visibility preserves its typing mode.
- In `app.css`, make `.cha-transliterating-field` use one full-width column
  without a column gap by default. Apply the existing two-column layout and
  `0.4rem` gap only when it contains `.cha-transliteration-toggle`, using
  `:has(...)`. The stylesheet already uses `:has`.
- Also hide `.cha-notes-screen .cha-chat-status:empty` with `display: none`.
  `NotesScreen` always renders this wrapper, and `.cha-chat-status` gives it a
  `0.35rem` top margin even after its only button returns `null`. This rule
  removes the leftover space and automatically stops applying when the button
  returns. Keep the nonempty Chat status row visible. Do not test the truthiness
  of the `transliteration` React element to detect an empty row: the element
  exists even when its component renders `null`.

Conditional rendering in this one button component covers all current sites,
including portals. Do not add visibility props to each screen or a document-wide
CSS class, and do not hide buttons with opacity or `visibility: hidden`.

### Native hosts

**macOS:** Add an initially unchecked `russianMenuItem` after Notes prompt and
an action that calls `nativeBridge?.setLayout("toggle-russian")`. Extend the
`view_state` parser and `onLayoutChanged` callback with the Russian boolean,
using the same strict boolean validation as the other flags. Update the menu
item in the existing callback in `main.swift`.

**Windows:** Add a menu command ID such as `kViewRussian`, append the unchecked
Russian item, and route its command to `set_layout("toggle-russian")`. Extend
the existing `view_state` validation and `CheckMenuItem` updates with `russian`.

Keep host and renderer changes together. Use the existing menu delivery and
connection behavior; add no retry mechanism or host preference store.

The separate ChaWeb interface has no native View menu and does not use these
transliteration controls. It needs no changes.

## Validation and acceptance

Use focused checks for the new behavior and adapt existing transliteration
tests that assume buttons are always visible.

1. **Reducer:** Initial visibility is false. Two toggles restore false without
   changing other View flags. Navigation and vault context transitions retain
   the selected value.
2. **App and shared controls:** Start with no transliteration buttons. Invoke
   `__CHA_NATIVE_SET_LAYOUT__('toggle-russian')` and verify mounted controls
   appear and the reported `view_state.russian` is true. Toggle again and verify
   removal and a false report. Cover a prompt, a short text field, an inline
   rename, and a portal text editor, including one opened after the toggle.
3. **Typing behavior:** Verify showing buttons does not enable transliteration.
   Enable it, hide the buttons, and verify typing still transliterates. Verify
   Ctrl+Shift+Y still works while hidden, and showing buttons restores the
   current `aria-pressed` state. Retain existing paste, drop, and IME checks.
4. **State preservation:** Toggle with an unsaved draft and a mounted editor.
   Verify text, selection, and editor identity survive. In split view, verify
   both prompt rows update together. Hidden parent panels remain hidden.
5. **Existing test updates:** App-level tests such as `LiveChat.test.tsx` must
   show the controls through the native handler before clicking them.
   Standalone component harnesses that click a button must supply
   `TransliterationProvider controlsVisible={true}`. Update affected View-state
   expectations without weakening other flag checks.
6. **Native UI:** In `tests/native/ui.js`, explicitly show Russian before the
   existing Notes button-alignment check. Add default-hidden and toggle checks.
   Extend the corresponding Windows View smoke checks. Inspect the actual
   native menu on both platforms for label, placement, initial state, and
   checkmark synchronization; renderer hooks alone do not verify native menus.
7. **Layout and lifecycle:** Check Chat, Notes, short fields, inline rename, and
   text dialogs at wide and narrow sizes. Hidden buttons leave no empty slot
   and cannot receive tab focus. Verify that the empty Notes status row has no
   layout box or margin contribution, while Chat context text and other actions
   remain visible. Restore Russian and verify Notes button alignment again.
   A full reload/new instance starts unchecked.

For implementation, run `npm --prefix webapp run typecheck` and the affected
Vitest suites, build the renderer and native hosts on their target platforms,
and run the affected native UI smoke checks. No live provider API calls are
needed.

## Scope

Implementation is limited to the View flag, shared button visibility and
spacing, native menu/message wiring, and relevant checks. It does not translate
the application UI or add Russian typing to new fields. This design adds no
dependencies or persisted data.
