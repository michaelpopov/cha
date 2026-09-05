# Block 5 — Finish macOS startup and shared package defaults

Execute only this fifth block of six, then update the handoff below.
This brief includes the requirements needed here without previous conversation
or separate design reading.

## Goal and boundaries

Make a fresh macOS CHA launch work without an API key, open device verification
in the system browser, and add a subscription provider to the shared package
seed. Preserve existing users' databases, provider selections, and API keys.

Keep this personal application small. Reuse the common C++ runtime and browser
connection page. Do not add Swift token handling, a callback listener, a new
login wizard, provider migration, or runtime dependency on Node/Pi/Codex.
Node remains a build-time dependency for the existing web frontend.

## Input conditions and session setup

1. Read applicable `AGENTS.md`/`CLAUDE.md`, run `git status --short` from
   the repository root, and preserve unrelated changes.
2. Blocks 1–4 must have delivered working auth, the subscription provider,
   shared runtime routes, and the browser page. Inspect current implementation
   and their Handoffs as needed. In particular, `docs/block2.md` records the
   live-tested model and `docs/block4.md` records the link/navigation behavior.
   Their source/tests and recorded results supply actual state, not the chat.
3. The package script currently requires an Apple Silicon Mac, supported macOS,
   Xcode command-line tools, and the repository's declared Node/npm tooling.
   Inspect the script's platform checks before building. Do not treat a build
   on another platform as proof that the macOS wrapper works.
4. A manual login smoke needs the user's approval. Use disposable application
   data, preferably a separate OS test user/VM or an existing safe test-profile
   facility. Do not overwrite, rename away, or delete the active Application
   Support profile just to simulate a first launch. Do not add a general
   profile-management feature for this test.
5. If prerequisites/platform/login access are missing, complete safe work and
   available checks, then record what remains unverified. Stay below 300K
   session tokens, targeting a durable checkpoint before 200K.

## Shared behavior to preserve

Both platforms use `ApplicationRuntime` with one C++ OAuth owner per workspace.
The browser's **OpenAI** page shows Connect ChatGPT, a waiting link/code/Cancel,
or Connected to ChatGPT/Disconnect. It uses:

- `GET /api/v1/openai/auth`.
- `POST /api/v1/openai/auth/login`, `/poll`, and `/disconnect`, each with
  `{}` and the existing origin/content-type/cookie behavior.

The verification URL is `https://auth.openai.com/codex/device`. The user
approves there, and the page polls the local runtime. No browser-to-CHA OAuth
callback or pasted authorization code is involved. A Linux browser may be on
another machine; macOS must leave its `WKWebView` displaying CHA.

The C++ owner, not Swift/JavaScript, holds access token, refresh token, expiry,
and account ID. Its private mode-0600 file is
`<normalized absolute database path>.openai-auth.json`. On Mac it belongs
beside the database in Application Support, not in the bundle or temporary
workspace. Renewal is on model use with five minutes left; no platform timer
is needed. Pending login disappears on restart, connected credentials persist.

One mutex serializes auth and saving with a 15-second overall network deadline.
Brief waits and explicit reconnect after refresh/save failures are accepted.
Do not replace these policies or introduce another auth owner in Swift.
Retain the Mac runtime's `CHA_RUNTIME` cookie protection and normal downloads.

## Files to inspect or change

- `packaging/macos/main.swift`: startup, application-data preparation,
  initial import, API-key menu, and `WKWebView` delegates.
- `packaging/macos/runtime_bridge.cpp`, `runtime_bridge.h`,
  `runtime-smoke.c`, and `package.sh` as needed for integration/tests.
- Shared `packaging/linux/import-seed/`, including
  `system/providers/terra/config.toml`,
  `system/assistant/character.toml`, and seed character selections.
- Relevant import tests in `tests/application/unit_workspace.cpp` and
  `unit_workspace_config_store.cpp`.
- `scripts/check-linux-package.sh`, `scripts/test-linux-package-upgrade.sh`,
  and browser fixtures only where assertions assume the old seed.

## New provider seed contract

Add a separately named provider, leaving the existing API-key provider intact.
Use these fixed settings with the actual model verified in block 2:

```toml
auth = "openai_subscription"
host = "chatgpt.com"
port = 443
https = true
base_path = "/backend-api/codex"
mode = "net"
api = "responses"
model = "<block 2 live-tested model>"
stream = true
web_search = "off"
cache_retention = "off"
```

Do not ship the placeholder model string. No `api_key_env`, explicit
temperature/max-output-token setting, enabled web search, or non-off cache
retention is allowed, including effective character/assistant overrides.
These are first-version CHA limits. The general cache default is `short`,
so `off` must be explicit.

Fresh seed characters and the assistant should select the new subscription
provider. Existing databases are not rewritten on launch or upgrade. Existing
users can add/select it through current configuration import and character
settings; no migration or new setup UI is needed.

## Implementation steps

1. Trace `applicationDidFinishLaunching`, `prepareApplicationData()`,
   `importInitialDatabase(apiKey:)`, and the current key-loading/prompt path.
   Separate normal data preparation/import from the requirement to obtain a
   key. A fresh keyless launch must reach the common runtime/UI.
2. Preserve inherited/saved API keys, existing `.env` loading, and
   **Change API Key…**. Only remove the mandatory startup prompt/gate.
   Do not erase saved values, automatically disconnect, or switch existing
   provider selections. Missing API-key credentials already fail on use,
   not workspace loading.
3. Keep initial database import key-independent. Reuse the existing runtime
   bridge and shared seed; do not send OAuth tokens through Swift or invent
   platform-specific account storage.
4. Inspect how the shared page's external link actually reaches `WKWebView`
   delegates, including a new-window action if it uses `target="_blank"`.
   Open the verification HTTPS link with `NSWorkspace`, keeping CHA loaded.
   Use the smallest delegate change needed; preserve internal navigation,
   downloads, and cookie protection. Do not add a callback URL scheme/server.
5. Add the new named provider to the shared seed, using the verified model.
   Point fresh seed characters/assistant at it, checking effective overrides.
   Retain the old provider and all existing upgrade/import behavior.
6. Update affected seed/import/package expectations. Keep browser tests on
   deterministic fake providers; changing production defaults must not make
   automated tests call a subscription or require a real key.
7. Verify packaged runtime assets exclude credential files and unnecessary
   Pi/Codex/Node runtime dependencies. Build tools are not runtime assets.
   Make only concrete integration fixes, not packaging-framework changes.
8. Build and smoke the Mac package in disposable data. Confirm keyless launch,
   system-browser approval, return to the common connected page, and private
   storage beside the database. Separately verify existing saved/inherited
   keys and Change API Key behavior without exposing key values in logs.

## Completion verification

From the repository root:

```sh
cmake --preset ninja
cmake --build build/ninja --target cha_tests cha_web_tests cha_macos_runtime
./build/ninja/cha_tests
./build/ninja/cha_web_tests
```

The macOS runtime target is platform-specific. From `webapp`, use `npm ci`
if needed, then:

```sh
npm run check
npm run build
```

On the supported Mac, build with a fresh output parent to avoid replacing
existing artifacts:

```sh
oauth_package_dir=$(mktemp -d)
./packaging/macos/package.sh oauth-block5 "$oauth_package_dir"
```

Read the package script's existing runtime-smoke invocation and use it; do not
create another permanent harness. Record the actual output location and checks.
Before marking complete, verify:

- Wrapper build and existing runtime smoke pass. Fresh launch without a key
  reaches CHA; device approval opens in the system browser and the common
  connection page reaches connected state.
- The private sibling credential file lives in Application Support with the
  database, outside the bundle; never print its contents.
- Shared seed/import tests pass with the tested model and legal effective
  settings. Existing API-key provider definitions remain available.
- Existing database selections/data, saved/inherited keys, `.env`, the API-key
  menu, cookie protection, and downloads retain their behavior.
- No live credentials enter fixtures or packages. Automated browser tests
  remain deterministic. Linux package verification is still a separate check,
  not inferred from a Mac build.

## Handoff

Update this file with actual evidence and stop before the final cross-platform
verification block. Do not duplicate progress in the overview plan.

- Status: complete.
- Changed startup/import/navigation files and behavior:
  - `packaging/macos/main.swift`: `prepareApplicationData()` and
    `importInitialDatabase()` no longer require a key. Inherited
    `OPENAI_API_KEY` and a saved `.env` are still applied; missing keys no
    longer prompt at launch. **Change API Key…** is unchanged. `WKUIDelegate`
    `createWebViewWith` opens `target="_blank"` HTTPS links with
    `NSWorkspace`; same-frame external navigations are cancelled so CHA stays
    loaded. Internal `127.0.0.1` navigation, downloads, and the `CHA_RUNTIME`
    cookie are unchanged.
  - `packaging/macos/runtime-smoke.c`: initial import runs with
    `OPENAI_API_KEY` unset; cookie-gated `GET /api/v1/openai/auth` is checked.
  - `packaging/macos/package.sh`: rejects Node/Pi/Codex runtime files in the
    bundle. `runtime_bridge.cpp`/`.h` were inspected and not changed.
- New provider name, live-tested model, and fresh seed selections:
  provider `chatgpt`, model `gpt-5.6-terra` (block 2 live request). Seed
  assistant plus `epictetus`, `markus_aurelius`, and `seneca` select
  `chatgpt`. Existing `terra` API-key provider is retained. Browser e2e
  fixtures still use the fake `test` provider.
- Commands, tests, package/runtime-smoke results, and output path:
  - `cmake --preset ninja`
  - `cmake --build build/ninja --target cha_tests cha_web_tests cha_macos_runtime`
  - `./build/ninja/cha_tests`: 431 passed, 2 skipped (live, env not set).
    New `WorkspaceConfigStore.ImportsPackageSeedWithoutApiKey` passed.
  - `./build/ninja/cha_web_tests`: 196 passed.
  - `cd webapp && npm run check && npm run build`: 186 vitest tests passed;
    production bundle built.
  - `oauth_package_dir=/tmp/cha-oauth-block5.4fYJp8`
    `./packaging/macos/package.sh oauth-block5 "$oauth_package_dir"`
    macOS 26.0 Apple Silicon. Runtime-smoke passed (keyless import, cookie
    gate, `/api/v1/openai/auth`). Linux package check and upgrade check
    passed. Playwright served: 20 passed, including signed-out OpenAI page.
    Output: `/tmp/cha-oauth-block5.4fYJp8/CHA.app` and
    `/tmp/cha-oauth-block5.4fYJp8/CHA-macos-oauth-block5.zip`.
- Disposable-profile Mac login/browser/storage smoke:
  `$HOME` does not override Application Support, so a one-off binary
  identical except `applicationName = "CHA-oauth-block5"` used
  `~/Library/Application Support/CHA-oauth-block5`. Product `CHA.app` was
  not launched against the active profile. Keyless launch created
  `cha.sqlite3` with no `.env`, bound `127.0.0.1:62371`, and loaded the
  welcome session. Device login started/succeeded in `cha.log`. Private
  sibling `cha.sqlite3.openai-auth.json` appeared in Application Support
  with mode 0600 and keys `access_token`, `refresh_token`, `expires_at`,
  `account_id`; contents were not printed. File was not in either bundle.
  Credentials survived quit/relaunch. Logs did not contain token values.
- Existing API-key/data preservation checks:
  SHA-256 of the active `~/Library/Application Support/CHA`
  `{cha.toml,.env,cha.sqlite3,cha.sqlite3.bac,launcher.log,runtime}` was
  unchanged throughout. No auth file was written there. A dummy saved
  `.env` in the disposable profile was not rewritten on launch or when
  `OPENAI_API_KEY` was inherited. **Change API Key…** remains in the
  packaged binary; the save path is unchanged. The Change API Key dialog
  was not clicked (System Events/Accessibility blocked automation).
- Remaining work, unavailable checks, and safe cleanup performed:
  Linux packaged-app login is block 6, not inferred from this Mac build.
  Disposable profile and one-off app were deleted after the smoke,
  including the live sibling credential file. Active profile left in place.
- Ready for block 6: yes.
