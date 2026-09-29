# ChaWeb implementation plan

Status: planned work for stage 1 of the [ChaWeb design](chaweb.md).
The design defines behavior; this plan lists implementation tasks and checks.
Complete the steps in order, including real iPhone checks in step 7 before
building the state logic in step 8. Repeat device checks with the installed app.

## 1. Keep the accepted scope

- [ ] Follow design [sections 1–2](chaweb.md#1-purpose-and-stages): a standalone
  text UI, existing CHA settings and DTOs, snapshot polling, a serial daemon,
  and one private nginx port per user with a fixed daemon socket.
- [ ] Preserve native CHA and the separate OpenAI listener and adapter.
  Keep runtime commands, the DTO schema, and generated types unchanged.
- [ ] Defer application voice features according to
  [section 8](chaweb.md#8-stage-2-adding-voice); native keyboard dictation is
  ordinary text input and is included in stage 1 checks.

## 2. Prepare the development and test environment

- [ ] Use the Node/npm versions in `webapp/package.json`; install locked
  dependencies with `npm --prefix webapp ci`.
- [ ] Configure the CMake `ninja` preset and build `cha-daemon`,
  `cha_daemon_tests`, and `cha_prepare_test_vault` on Linux.
- [ ] Install nginx for integration tests. Reuse temporary vaults, the fake
  provider in `tests/integration/daemon_integration_test.py`, and the socket
  launcher in `scripts/run_daemon.py`; keep tests away from personal vaults.
- [ ] Record baseline daemon test and `npm --prefix webapp run check` results.
  Have an iPhone and development machine on the same LAN ready for step 7.

## 3. Add the daemon API foundation

- [ ] Add `src/daemon/chaweb_adapter.h` and `.cpp`; wire them into
  `src/daemon/main.cpp` and the daemon/test targets in `CMakeLists.txt`.
  Dispatch `/api/cha/v1/` in the existing serial loop.
- [ ] Extend `src/daemon/scgi.h` and `.cpp` to capture optional `CONTENT_TYPE`
  and reject duplicate recognized fields. Add CGI reason phrases for `201`,
  `204`, `415`, `422`, and `503`, with empty bodies for `204`.
- [ ] Implement validation and error mapping from
  [section 4](chaweb.md#4-http-api). Require `application/json` on every ChaWeb
  POST before parsing or mutation. Reuse `invalid_argument` for rejected input.
- [ ] Capture `application.context_epoch()` once per request and preserve the
  existing limits, cancellation, and shutdown rules from
  [section 6](chaweb.md#6-serial-daemon-requests).

Check: add focused SCGI and adapter tests for media types, malformed JSON,
unknown method/path pairs, statuses, and existing error shapes. Verify that
OpenAI behavior and native DTO checks remain unchanged.

## 4. Implement the six API operations

- [ ] Implement the [endpoint table](chaweb.md#endpoint-summary) in the adapter
  using public `Application` methods and existing JSON serializers.
- [ ] Use `open_session()` then the operation for every named-session request.
  Submit raw commands and determine acceptance from `input_consumed`.
- [ ] Implement creation cleanup and uncertain-outcome handling from
  [section 4](chaweb.md#creating-a-session-with-its-first-input). Return after
  acceptance; leave generation to existing runtime workers.
- [ ] Add `tests/daemon/unit_chaweb_adapter.cpp` to `cha_daemon_tests`. Extend
  `tests/daemon/unit_daemon_process.cpp` for dispatch and shutdown checks.

Check: cover membership validation, existing response shapes, self-notes,
rejection cleanup and cleanup failure, unknown submission outcomes, retired
sessions, idle Stop, and generation continuing after an input response.

## 5. Add nginx routing

- [ ] Add `packaging/linux/nginx-chaweb.conf.example` from
  [section 7](chaweb.md#7-nginx-scgi-and-deployment), with a shared static root
  and literal per-user `scgi_pass`. Preserve the separate OpenAI configuration.
- [ ] Configure MIME types, HTML/hashed-asset caching, buffered SCGI, standard
  `scgi_params` including `CONTENT_TYPE`, the request limit, timeouts, and gzip.
  Set API `Cache-Control: no-store` with `add_header ... always` in nginx.
- [ ] Render this template in the existing integration harness with one daemon
  and one nginx Unix-socket HTTP listener. Prepare a local nginx HTTP listener
  for the Vite development proxy; deployed listeners use private HTTPS.

Check: run `nginx -t` and exercise bootstrap and session requests through SCGI.
Check statuses, limits, compression, caching, and an unavailable daemon socket.

## 6. Build the browser entry and HTTP client

- [ ] Add `index.html`, `main.tsx`, `App.tsx`, `client.ts`, and `styles.css`
  under `webapp/src/chaweb/`; add components only as needed.
- [ ] Add `webapp/vite.chaweb.config.ts` with root `src/chaweb/`, output
  `webapp/dist-chaweb/`, and deployment base `/`. Add `dev:chaweb` and
  `build:chaweb` scripts in `webapp/package.json`; ignore the build output.
  Keep native Vite, TypeScript, and Vitest configurations unchanged.
- [ ] Proxy `/api/cha/v1/` to local nginx during development. Use relative API
  URLs, default same-origin credentials, rejected redirects, JSON POSTs, and
  the browser timeout from the design.
- [ ] Implement the six client operations with shared types/guards from
  `webapp/src/api/client.ts` and `validateBootstrap()` from
  `webapp/src/state/bootstrap.ts`. Handle `204` and non-JSON gateway errors.
- [ ] Load bootstrap from the API and show startup loading/errors. Keep native
  bridge/bootstrap imports out of the browser entry.

Check: build both frontend entries, run client tests for statuses and guards,
and load production ChaWeb through nginx without a native host or login.

## 7. Implement and test the iPhone layout

- [ ] Build both views, stored-session hash navigation, transcript, and composer
  according to [section 3](chaweb.md#3-browser-application). Reuse
  `webapp/src/components/Markdown.tsx` and character appearance code where useful.
- [ ] Implement the editor size button and Visual Viewport layout in
  `webapp/src/chaweb/`: synchronize height/offset on viewport events, prevent
  body scrolling, and apply safe-area spacing. Follow the design's focus,
  keyboard, and dictation rules.
- [ ] Run the development server on the LAN:

  ```sh
  npm --prefix webapp run dev:chaweb -- --host 0.0.0.0
  ```

- [ ] Open `http://<development-machine-LAN-IP>:<vite-port>/` in iPhone Safari.
  Plain HTTP is enough for layout and native keyboard dictation checks; use
  the development proxy for API calls. No HTTPS setup is needed for this step.
- [ ] Check both views, compact/tall editor sizes, keyboard opening/dismissal,
  portrait/landscape, Safari controls, safe areas, long drafts, and scrolling.
  Verify text, caret, focus, and reading position survive size changes.
- [ ] Dictate using the iPhone keyboard while replacing sample transcript
  snapshots about once per second. Use a small temporary development fixture
  before the real polling logic exists. Confirm dictation continues, text stays
  editable, and touch Enter adds a newline without submitting.

Check: record results on the real iPhone and fix layout/input issues before
step 8. Also check desktop keyboard controls and the size button's Enter/Space
activation. Desktop emulation alone does not complete this step.

## 8. Connect drafts, commands, polling, and recovery

- [ ] Add the necessary state/hooks under `webapp/src/chaweb/`. Implement draft,
  navigation, Send, and Stop behavior from [section 3](chaweb.md#3-browser-application),
  failure handling from [section 4](chaweb.md#failure-and-retry-rules), and
  polling/recovery from [section 5](chaweb.md#5-snapshot-polling).
- [ ] Keep state limited to view/selection, drafts, the current snapshot,
  editor mode, and request tracking. Associate responses with their originating
  navigation and draft revision; do not duplicate durable conversation history.
- [ ] Add adjacent Vitest tests with fake fetch and timers for draft preservation,
  first Send and Stop availability, stale responses, hidden-page polling,
  recovery, and preventing automatic replay after uncertain writes.
- [ ] Replace the temporary transcript fixture with real snapshot updates.

Check: run these tests and repeat iPhone dictation during real generation and
polling. Confirm transcript updates preserve the editor and reading position.

## 9. Complete automated integration coverage

- [ ] Extend `tests/integration/daemon_integration_test.py` for the API and daemon
  scenarios in [section 10](chaweb.md#10-validation-and-acceptance). Keep new
  ChaWeb fixtures to one daemon and one nginx Unix-socket HTTP listener;
  preserve existing OpenAI tests, including their multi-user fixtures.
- [ ] Include the `text/plain` creation regression: valid JSON receives `415`
  with no session or provider call. Check unsupported preflights as specified.
- [ ] Check `/` and its referenced production JS/CSS assets return `200`, correct
  MIME types, and intended cache headers through the shipped nginx template.
- [ ] Extend `Makefile`'s `itest-daemon` recipe to build ChaWeb assets before the
  Python tests. Report missing artifacts when the script is run directly.
  Keep browser logic tests in Vitest; no separate integration target or browser
  automation harness is needed.

Check: run `make test`, `make web-check`, `make itest-local`, and
`make itest-daemon` before packaging, using local/fake providers.

## 10. Package and install

- [ ] Update `packaging/linux/package.sh` to install locked frontend dependencies,
  build ChaWeb, and include its output as `chaweb/` alongside the daemon and
  nginx example. Deployment servers must not need Node or `node_modules`.
- [ ] Update `packaging/linux/install.sh` to install `$CHA_DEPLOY_PATH/chaweb`
  and the example. Verify nginx can traverse parent directories and read assets;
  keep vaults/configuration outside the static root with existing permissions.
- [ ] Preserve operator-owned nginx/systemd files on upgrade. Keep explicit port
  assignments and existing `add_user.sh` provisioning; ChaWeb uses no API key.
- [ ] Update `packaging/linux/README.md` with dependencies, asset paths,
  port/socket and TLS setup, permissions, install checks, and upgrade commands.

Check: build with `make package-linux VERSION=<version>`, inspect the archive,
and test clean install/upgrade in an isolated Linux/systemd environment. Verify
socket activation, file access, and preservation of user data/configuration.

## 11. Bring up the private deployment

1. Install using `CHA_DEPLOY_PATH` and `CHA_DATA_PATH`. Provision users with
   `add_user.sh` or reuse their vaults. Verify provider/forum settings and follow
   the existing exclusive-vault and protected-vault password-file rules.
2. Enable each `cha@<user>.socket` and check nginx group access and service
   configuration. Set private address, hostname, TLS, static root, and one
   port/socket pair per user in the nginx example.
3. Make the hostname, port, and trusted certificate available to the iPhone.
   Run `nginx -t`, reload, and check `/` and `/api/cha/v1/bootstrap`. At setup,
   check the second user's port once for its own forums and sessions.
4. In Safari, create a session, continue an older session, and Stop a long reply.
   Verify persistence after browser reload and daemon restart.
5. For upgrades, install matching frontend/daemon files, restart affected daemon
   services, and recheck bootstrap and a conversation. Preserve ports/config;
   reload old browser tabs after an incompatible upgrade.

## 12. Complete installed-app acceptance

- [ ] Run [section 10's acceptance scenario](chaweb.md#10-validation-and-acceptance)
  through installed nginx/systemd. Repeat step 7's real iPhone checks using
  production assets and HTTPS, plus step 8's actual polling and recovery.
- [ ] Verify background/foreground navigation, drafts, network failures, and
  daemon restart on the real browser path; confirm existing OpenAI behavior.
- [ ] Check that stage 1 retains the decisions in
  [section 8](chaweb.md#8-stage-2-adding-voice) without adding voice implementation.

Stage 1 is complete when these checks pass and the package instructions are
sufficient to install and use the application.
