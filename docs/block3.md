# Block 3: ChaWeb browser shell and iPhone layout

Status: implementation instructions; the work is not yet complete.
This block implements plan steps 6–7 and the production-asset checks from step
9. It requires a working ChaWeb API and local nginx listener from blocks 1–2.
It delivers the standalone build, HTTP client, navigation, and tested layout.
Full command, draft-revision, polling, and recovery state belongs to block 4.

This document contains the requirements for this block. Background:
[chaweb.md](chaweb.md) and [chaweb-plan.md](chaweb-plan.md).

## Architecture and scope

ChaWeb is a React/TypeScript application served as static files by nginx. It
uses the same origin for `/api/cha/v1/`. nginx chooses a daemon by its listening
port and forwards SCGI over that user's fixed Unix socket. There is no API-key
screen, credential storage, server selector, or application login. Default
same-origin fetch credentials permit browser-managed Basic authentication if
the operator later enables it. Production ports are private HTTPS listeners.

The main target is iPhone Safari. Use two full-screen views on phones and
desktops: forums/sessions and conversation. No sidebar, drawer, top chat title,
settings, recipient selector, rename action, microphone button, or media player.
Stage 1 accepts ordinary keyboard dictation; CHA speech settings are unused.

## Files and build

| File | Work |
| --- | --- |
| `webapp/src/chaweb/index.html` | New browser HTML entry and viewport meta tag. |
| `webapp/src/chaweb/main.tsx`, `App.tsx`, `styles.css` | Browser mounting, navigation, components, and layout. |
| `webapp/src/chaweb/client.ts` | Narrow `ChaWebClient` interface and fetch implementation. |
| Adjacent `*.test.ts` / `*.test.tsx` | HTTP client, navigation, and component tests. |
| `webapp/vite.chaweb.config.ts` | Separate browser build and development proxy. |
| `webapp/package.json` | Add `dev:chaweb` and `build:chaweb`. |
| `.gitignore` | Ignore `/webapp/dist-chaweb/`. |
| `Makefile` | Build ChaWeb assets in the existing `itest-daemon` recipe. |
| `tests/integration/daemon_integration_test.py` | Test actual production files through nginx. |

Use Node/npm versions pinned in `webapp/package.json` and install with
`npm --prefix webapp ci`. Reuse existing React, Vite, TypeScript, Vitest,
Testing Library, Marked, and DOMPurify dependencies. No new package project or
test harness is needed. Existing TypeScript includes `src`, and Vitest finds
`src/**/*.test.{ts,tsx}`; keep those configurations unchanged.

Set the new Vite root to `webapp/src/chaweb` and output to
`webapp/dist-chaweb`, using paths resolved relative to the configuration file.
Use deployment base `/`. The build must typecheck and produce `index.html`
plus hashed JS/CSS assets. Keep the native build, staging, and
`webapp/vite.config.ts` intact. Do not copy its native-bootstrap plugin.

Add these script behaviors:

```json
{
  "dev:chaweb": "vite --config vite.chaweb.config.ts",
  "build:chaweb": "npm run typecheck && vite build --config vite.chaweb.config.ts"
}
```

Use a development port distinct from native Vite's 5173, for example 5174,
with a strict port and localhost binding by default. Proxy `/api/cha/v1/` to
the local nginx listener, initially `http://127.0.0.1:8087`. Keep this as
development configuration, not an application setting. The phone connects to
Vite; the Vite process connects to nginx on the development machine.

## HTTP client contract

All paths below are relative to `/api/cha/v1`. `S` is
`/forums/{forum_id}/sessions/{session_id}`. Encode each opaque ID as a URL
segment; never derive identity from titles or transcript text.

| Request | Request body | Success and validation |
| --- | --- | --- |
| `GET /bootstrap` | None | `200`; `Bootstrap`, `validateBootstrap()`. |
| `GET /forums/{forum_id}/sessions` | None | `200`; `SessionListing[]`, `isSessionListingArray()`. |
| `POST /forums/{forum_id}/sessions` | `{ "text": "..." }` | `201`; `CreateSessionResult` with `id` and `label`, `isSessionLabelResult()`. |
| `GET S` | None | `200`; `SessionSnapshot`, `isSessionSnapshot()`. |
| `POST S/input` | `{ "text": "..." }` | `204`; do not parse a body. |
| `POST S/stop` | `{}` | `204`; do not parse a body. |

Reuse types and guards from `webapp/src/api/client.ts` and bootstrap validation
from `webapp/src/state/bootstrap.ts`. `resources/dto.yaml` and the generated
`webapp/src/api/schema.d.ts` remain unchanged. Use the existing error schema;
do not create a second DTO layer or fill the large native `ChaClient` interface
with dummy operations. Accept additional response fields.

Use relative URLs, default `same-origin` credentials, and `redirect: "error"`.
Set `Content-Type: application/json` on every POST, including Stop. Add no
bearer header or `credentials: "omit"`. Apply a 60-second browser request
timeout and clear its timer when complete. The transport must never retry a
mutation. Read retries are wired into the state layer in block 4.

Check HTTP status and content type before JSON parsing; parse successful DTOs
through the guards. The error shape is
`{"error":{"code":"...","message":"..."}}`. The adapter uses `400`,
`404`, `415`, `422`, `500`, and `503`; nginx/SCGI can also return `413`, `502`,
or `504`. Expose status and a safe message to the state layer. A `404` requires
navigation-list refresh; `422` displays CHA's notice; other failures display a
safe message. Keep drafts on every failure. Use a generic message for non-JSON
proxy errors, never render proxy HTML or raw exceptions as chat content.

Creation accepts the first input and returns its ID. Later input and Stop
return no notice body; callers refresh snapshots. A failed or timed-out mutation
can have succeeded and must not be automatically resubmitted. Closing a browser
request does not stop accepted generation. The serial daemon can delay requests
behind classification or another client's long turn.

## Startup and navigation

Load bootstrap from the API and show loading or an actionable error if it
cannot load. Do not request `__cha-bootstrap.js`, native host globals, or native
resource URLs. Validate the complete bootstrap before filtering: Entrance must
remain in the validated object. Hide its forum by `entrance_forum_id`, its
sessions, and the process-local Welcome conversation from navigation. Ignore
initial-session fields for automatic opening; hide vault controls while keeping
the underlying metadata intact.

Support stored-session hash routes:
`/#/forums/{forum_id}/sessions/{session_id}`. At startup restore a valid route;
otherwise show the forum/session view. Parse Back/Forward changes and fetch the
selected session. Avoid duplicate fetches from programmatic hash changes.
An unknown resource displays an error and refreshes navigation choices.

The list view and new conversation are local view state, with no `/new` route.
New Session clears a stored-session hash and shows that forum's local empty
conversation/draft. It makes no create, snapshot, or cleanup request. Forum
selection alone only changes the list. Do not create a session on page load.

Build only the local state needed to exercise the views and editor here. Wire
read-only bootstrap/list/snapshot navigation to the real API. Present command
states through component props/callbacks for testing; block 4 connects real Send
and Stop and protects drafts against late acknowledgements. A sample transcript
fixture must not become a second runtime transport or a shipped demo mode.

## Forums/sessions view

- Put a full-width native forum combobox at the top. Its value is the selected
  forum's name; changing it loads that forum's sessions.
- Show sessions ordered by recent activity using `updated_at`, with title and
  date/time. The whole row opens the session. Mark the current session with a
  check when it belongs to this forum. `SessionListing.live` does not mean a
  reply is generating.
- Keep New Session reachable at the bottom while the list scrolls. It returns
  to the existing local draft for that forum if one exists.
- Add no screen heading, Done button, action menu, or helper labels. Selecting
  a row opens it immediately, including the current session.
- Opening this view through the Sessions icon selects the conversation's forum,
  dismisses the keyboard, and preserves its draft and editor size mode.

## Conversation and composer

Use all visible space above the composer for the transcript. Add no forum or
session title above it. Render stored entries in order with speaker identity,
display name, time, Markdown, and complete/streaming/cancelled/failed status.
Keep different character replies separate and preserve stored authorship when
the current roster changes. Show covered history as normal transcript content.

Reuse `webapp/src/components/Markdown.tsx` and
`webapp/src/components/characterAppearance.ts`, with the needed appearance rules
from `webapp/src/styles/app.css`.
Preserve sanitization, wrapping, and code-block scrolling. Do not display
reasoning text or add audio behavior from `has_cached_audio`. Token counts,
search indicators, special covered styling, and jump-to-latest are optional
only if a suitable reused component already supplies them without added work.

Build a small browser composer; avoid importing voice/settings/native behavior
from the full native `ChatScreen`. Use a standard full-width multiline textarea.
Below it place a compact control row: icon-only Sessions left, Send/Stop right.
No controls overlap text or take width from the editor. Visible icons can be
small, but give them accessible names and roughly 44-by-44 CSS-pixel targets.
UI copy is limited to controls, actionable state, validation, and errors.

Place a native `button type="button"` between transcript and editor to toggle
between compact (about two lines) and expanded (about half the usable visible
area). Start compact. Accessible name: Expand editor or Shrink editor for the
current action. Support native click, tap, Enter, and Space. No drag logic,
custom keyboard resizing, or auto-growing textarea.

Remember the mode in browser memory, not a fixed height. Recalculate height
when available space changes, leaving room for controls and some transcript.
Long drafts scroll inside the textarea. Preserve text, selection, keyboard,
and reading position while toggling. Prevent pointer activation from needlessly
blurring an already focused editor while retaining native keyboard accessibility.

On desktop Enter invokes Send and Ctrl+Enter inserts a newline. With primary
`(pointer: coarse)`, Enter inserts a newline and the Send icon submits. Detect
IME composition and never submit while composing. Do not infer phone behavior
from touch support alone. Opening a stored session must not autofocus or open
the keyboard. Incoming transcript updates must not remount the editor, change
its draft, move its caret, or interrupt dictation.

Follow new output only if the user is near the end. Otherwise preserve their
reading position, including through editor resizing and snapshot replacement.
Use stable entry IDs as keys. The user scrolls down to resume following.

## Visual Viewport layout

Use this viewport meta value in the HTML entry:
`width=device-width, initial-scale=1, viewport-fit=cover`.

Set app height from `window.visualViewport.height` and align its top using
`visualViewport.offsetTop` relative to the layout viewport. Synchronize on
initial load and visual viewport `resize` and `scroll`; remove listeners on
unmount. Use the ordinary window viewport when this API is unavailable.

Prevent `html`/`body` scrolling. Inside the measured app use a column flex layout
with `min-height: 0` for shrinking transcript/list regions and internal scroll
areas for transcript, list, and editor. Keep the composer/control row in this
layout above the keyboard. Do not use a separately fixed composer as a keyboard
workaround. Safari may shrink the visual viewport without shrinking layout
viewport units such as `100vh`.

Use border-box sizing and include safe-area padding within the measured height:
`env(safe-area-inset-bottom)` for the home indicator, plus top/side insets as
needed. Insets are not keyboard height. Set textarea text to at least 16 CSS
pixels. Verify rotation, browser controls, and keyboard changes on real hardware.

## Production assets and automated checks

Extend `make itest-daemon` now to build assets with `build:chaweb` before the
existing Python harness. Keep the daemon and test-vault targets. The Python
runner must report a missing `dist-chaweb/index.html` clearly when run directly.
Use the existing ChaWeb fixture's single daemon and Unix HTTP listener.

Point the rendered shipped nginx template at actual `webapp/dist-chaweb` output.
Include real nginx MIME mappings. Request `/`, `/index.html`, and referenced
JS/CSS assets: require `200`, correct MIME types, revalidated HTML (`no-cache`),
and long-lived immutable hashed assets. Confirm API `no-store` behavior remains
intact. Do not substitute placeholder HTML or a Vite server for these checks.

Use adjacent Vitest tests and existing jsdom/Testing Library setup for client
validation, `204`, error/timeout handling without write retries, navigation,
sanitized rendering, and size/keyboard controls. Use full existing fixtures in
`webapp/src/test/fixtures.ts` or `tests/fixtures/wire/` where suitable; incomplete
bootstrap samples must not weaken shared guards. No new Playwright suite.

```sh
npm --prefix webapp run build
npm --prefix webapp run build:chaweb
make web-check
make itest-daemon
```

## Early iPhone check and completion

Before block 4, start the local daemon/nginx listener and run:

```sh
npm --prefix webapp run dev:chaweb -- --host 0.0.0.0
```

Open `http://<development-machine-LAN-IP>:5174/` in iPhone Safari, using the
configured port if different. The phone and development machine must share a
reachable LAN; allow that development port through the local firewall if needed.
The proxy target stays localhost on the machine. Plain HTTP is enough for this
layout and native keyboard dictation check. Dictation must be enabled in the
iPhone keyboard settings; ChaWeb makes no audio or Web Speech API calls.

Check both views, portrait/landscape, keyboard show/hide, Safari browser controls,
safe areas, compact/expanded sizes, long drafts, long transcripts, and internal
scrolling. While editing, tap the size toggle and verify text, caret, and the
open keyboard survive. Verify touch Enter adds a newline and opening a stored
session does not summon the keyboard.

Temporarily feed replacement sample snapshots about once per second through the
real transcript component while dictating. Confirm editable dictated text and
focus survive updates, and dictation never submits. This fixture tests layout
before the full polling state exists; remove it when block 4 wires real updates.
Also check desktop Enter/Ctrl+Enter, composition handling, and native Enter/Space
activation of the size button.

Record the device/browser and results. Fix layout/input problems before starting
block 4. Desktop emulation is useful but does not satisfy this completion check.
If a physical device is unavailable, report the check as outstanding; do not
claim the layout block complete. Completion also requires passing builds,
Vitest/native frontend checks, and production-asset integration tests.
