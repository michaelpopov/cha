# Block 4 — Add the browser connection page

Execute only this fourth block of six, then leave the handoff below.
The UI requirements are included here; no earlier conversation or separate
OAuth design reading is required.

## Goal and boundaries

Add an OpenAI connection page to CHA's existing TypeScript UI. The same page
runs in a remote browser connected to Linux console `chaweb` and in the
macOS app's `WKWebView`. C++ owns all OAuth credentials and exchanges; the
browser displays a device code and schedules individual server polls.

Keep the personal-app implementation small: one page, existing layout/error
handling, one outstanding operation at a time. No frontend OAuth library,
token storage, global background poller, cross-tab coordinator, account-profile
page, callback handling, or model-request retry mechanism.

## Input conditions and session setup

1. From the repository root, read applicable `AGENTS.md`/`CLAUDE.md`, run
   `git status --short`, and preserve unrelated changes.
2. Block 3 must provide the four routes, generated TypeScript schema, and
   working shared runtime. Read its actual API definitions in
   `resources/cha.yaml`/`webapp/src/api/schema.d.ts` and the Handoff in
   `docs/block3.md` for response examples, units, and verification results.
   The required semantics are repeated below; use the implemented schema's
   exact field/type names, not names guessed from prior conversation.
3. If prerequisite evidence is missing, inspect the implementation and run
   relevant checks. Report a genuine missing API rather than building another
   auth system in TypeScript. Keep automated tests synthetic and subscription-free.
4. Use the repository's declared Node/npm versions and lockfile. Stay below
   300K session tokens, targeting a checkpoint before 200K. Read scoped files,
   avoid full build-log dumps, and persist unfinished work here.

## Server contract the page consumes

| Method and route | Action |
| --- | --- |
| `GET /api/v1/openai/auth` | Read current display state without upstream auth work |
| `POST /api/v1/openai/auth/login` | Start login or return the current pending attempt |
| `POST /api/v1/openai/auth/poll` | Poll the pending login at most once if eligible |
| `POST /api/v1/openai/auth/disconnect` | Cancel pending login or disconnect |

Every POST sends an empty JSON object, `{}`, with the current client's
normal same-origin/content-type behavior. Preserve macOS cookie handling.
Responses are non-cacheable. Errors use the existing API error envelope.

There are three public states:

- `signed_out`: show **Connect ChatGPT**.
- `waiting`: show the verification link, user code, and **Cancel**. The
  response also gives the attempt expiry and next-poll delay in schema-defined
  units. The verification page is `https://auth.openai.com/codex/device`.
- `connected`: show **Connected to ChatGPT** and **Disconnect**.

No email, account lookup, access/refresh token, private device ID,
authorization code, or verifier belongs in a browser response or UI state.
Do not add further recovery/storage-error states; show the current error UI
and allow an explicit retry/reconnect.

The account and pending attempt belong to the workspace, not to the tab.
Starting again while waiting returns the existing code. Changing a connected
account requires disconnecting first. All browsers may control the same state;
immediate synchronization with another tab is unnecessary.

The server enforces the next allowed poll time and a 15-minute pending
deadline. An early poll returns status without calling OpenAI. Each eligible
poll performs one upstream poll and possibly the final token exchange, with
an overall 15-second network deadline. Controls may wait briefly for serialized
auth work; this is accepted.

Leaving the page stops browser polling, not the server's pending attempt.
Returning can resume it until expiry. Restarting the server loses pending
login but preserves saved connected credentials. Refresh is on model use in
C++; it needs no browser timer. Disconnect does not have to cancel a model
request that already obtained credentials.

## Files to inspect or change

- `webapp/src/api/client.ts` and `client.test.ts`.
- `webapp/src/state/view.ts`, `route.ts`, and their tests as needed.
- `webapp/src/components/Sidebar.tsx`, `App.tsx`, and their tests.
- Add one connection component with focused component tests.
- `webapp/src/test/fixtures.ts` and other `ChaClient` implementations/mocks
  located by search. Update all affected typed mocks.
- Read the generated schema; do not manually edit it. Server changes should
  be limited to a concrete integration defect with a regression test.

## Implementation steps

1. Inspect the current client, route/view state, sidebar, and page lifecycle.
   Add four typed client methods using the generated API types. Reuse existing
   request/error/cookie handling rather than adding another HTTP client.
2. Add an **OpenAI** sidebar destination and route/view state using current
   navigation conventions. Fetch connection status when entering the page.
   Switching to/from it must preserve the selected chat/session and existing
   character-settings state.
3. Render the three states above, with the existing loading and error patterns.
   Do not show a misleading signed-out action before initial status is known.
   Keep connected presentation to the simple text and Disconnect button.
4. Render verification as a normal external HTTPS link opened only on a user
   click, with new-tab protections such as `rel="noopener noreferrer"`.
   Keep CHA open to finish polling. Do not automatically launch a server-side
   browser, accept callback tokens, or add a Swift bridge. Block 5 handles
   the macOS system-browser navigation behavior.
5. While waiting, schedule one timer from the server-provided next-poll delay.
   Wait for that request's response before scheduling the next timer; do not
   use an overlapping interval. Use the returned timing after every poll,
   including slowdown or an early-poll response.
6. Stop scheduling on success, error, cancel/disconnect, or unmount. Clear
   timers and ignore stale completions after navigation. If a request is
   already in flight, an ignored result must not recreate a timer or overwrite
   a new page state. Use the existing lifecycle patterns, not a new framework.
7. Serialize this page's mutations and disable conflicting buttons in flight.
   Cancel and Disconnect call the same endpoint; once a user requests either,
   do not schedule another poll. Status returned after navigation controls
   resumption; no local-storage attempt ID or tab leadership is needed.
8. Keep all browser persistence and URLs token-free. Do not add a frontend
   refresh loop or silently replay failed model requests. On an API error,
   stop polling and offer a clear explicit way to retry/reload status.
9. Add focused client, component, timer, and navigation tests. Use synthetic
   snapshots matching the schema and fake timers; automated tests must not
   open a real verification flow or need a subscription.

## Completion verification

From `webapp`, run `npm ci` if dependencies are missing, then:

```sh
npm run check
npm run build
```

For the existing browser harness, first build the server from the repository
root:

```sh
cmake --preset ninja
cmake --build build/ninja --target chaweb_app
```

Then from `webapp`:

```sh
npm run e2e
```

The harness uses disposable data and a mock model. Install its Playwright
browser dependency if needed; do not replace mocks with live account access.
If browser tooling is unavailable, report that check as outstanding.

Verify these cases explicitly:

- Initial load, all three states, start, cancel, disconnect, error display,
  and explicit retry/resume after navigation.
- Client tests assert exact methods/routes, empty JSON POST bodies, and use of
  the existing error handling.
- Fake timers show no overlapping polls, scheduling from returned timing,
  and no rescheduling after terminal state, cancellation, error, or unmount.
  A late completion does not alter the newly selected view.
- Relevant buttons cannot create conflicting mutations. A pending attempt
  discovered by GET resumes without starting a second login.
- The verification link needs a user click and has new-tab protections.
  The browser holds only displayable status/code/timing, never token data.
- Existing chat navigation, session selection, character settings, and browser
  tests still work. Include a browser smoke of the new page's signed-out state.

## Handoff

Update this section as the durable block record and stop before changing
macOS startup or package defaults.

- Status: complete.
- Changed files, client methods, component, and route/view name:
  - `webapp/src/api/client.ts`: typed `OpenAiAuth` plus
    `getOpenAiAuth` (`GET /api/v1/openai/auth`),
    `startOpenAiAuth` (`POST /api/v1/openai/auth/login`, body `{}`),
    `pollOpenAiAuth` (`POST /api/v1/openai/auth/poll`, body `{}`),
    `disconnectOpenAiAuth` (`POST /api/v1/openai/auth/disconnect`, body `{}`).
    Reuses `requestJson` / `jsonMutation` / existing error conversion.
  - `webapp/src/state/view.ts`: `MainView` `'openai'`, action `'show-openai'`,
    topbar title `OpenAI`. Switching to/from this view keeps
    `activeConversation`, `inspectedCharacterId`, and
    `characterSettingsAvailable`.
  - `webapp/src/components/Sidebar.tsx`: **OpenAI** primary destination.
  - `webapp/src/components/OpenAiConnection.tsx`: `OpenAiConnectionScreen`.
    App routes `mainView === 'openai'` to it. Fetches status on enter.
  - Also: `Icons.tsx`, `App.tsx`, `app.css`, `fixtures.ts`, client/view/App
    tests, `OpenAiConnection.test.tsx`, `e2e/shell.spec.ts`.
- Poll scheduling/cleanup and retry behavior:
  One `setTimeout` from `next_poll_delay_ms` (or `0`). The next timer is
  scheduled only after that poll returns, using the returned delay. An
  epoch plus `cancelled` flag ignore stale completions after navigation,
  unmount, or Cancel/Disconnect; an ignored in-flight poll does not
  recreate a timer or overwrite the new view. HTTP errors stop polling and
  offer **Try again**, which reloads `GET` status. Snapshot `error` is
  shown in the existing alert UI; signed-out reconnect is **Connect ChatGPT**.
  Cancel and Disconnect both call `disconnectOpenAiAuth`. Conflicting
  buttons disable while a mutation is in flight; Cancel remains available
  during a poll.
- Verification link's actual navigation/new-window behavior for block 5:
  Waiting renders a normal `<a href={verification_url} target="_blank"
  rel="noopener noreferrer">` whose href is the snapshot URL
  (`https://auth.openai.com/codex/device`). It opens only on a user click.
  No `window.open`, callback tokens, or Swift bridge. CHA stays on the
  connection page and continues polling.
- Commands, test counts, browser smoke, and results:
  - `cd webapp && npm run check`: api-types match; typecheck; 184 vitest
    tests passed (was 167).
  - `cd webapp && npm run build`: production bundle built.
  - `cmake --preset ninja && cmake --build build/ninja --target chaweb_app`
  - `cd webapp && npx playwright test`: 37 passed (20 served + 17 chromium),
    including signed-out smoke: sidebar **OpenAI** shows **Connect ChatGPT**.
- Remaining work or unavailable checks: none for this block. Automated
  tests are synthetic and subscription-free; live ChatGPT login was not
  required. Stop before macOS startup or package defaults.
- Ready for block 5: yes. The shared page, client methods, poll lifecycle,
  `target="_blank"` verification link, and checks are in place.
