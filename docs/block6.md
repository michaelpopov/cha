# Block 6 — Verify packaged Linux and macOS end to end

Execute this final block, then record the final handoff below. All acceptance
requirements are included here; earlier conversation or separate design
reading is not required.

## Goal and boundaries

Verify the implemented OpenAI subscription feature in packaged Linux console
`chaweb` with a remote browser and in packaged macOS CHA. Confirm login,
generation, persistence, refresh, disconnect, credential isolation, and
continued API-key support.

This is verification plus small fixes for demonstrated integration bugs.
Do not add features, general OAuth machinery, model retry/fallback, background
refresh, new concurrency infrastructure, or an enterprise recovery system.
Keep manual reconnect and brief serialized auth waits as accepted limitations.

## Input conditions and session setup

1. From the repository root, read applicable `AGENTS.md`/`CLAUDE.md`, run
   `git status --short`, and preserve unrelated work.
2. Blocks 1–5 must be implemented. Inspect their current source/tests and the
   Handoffs in `docs/block1.md` through `docs/block5.md` for actual status,
   interfaces, live-tested model, package changes, and any outstanding checks.
   The complete acceptance contract is below; handoffs provide implementation
   evidence rather than missing task instructions.
3. Use a Linux machine, a supported Apple Silicon Mac with the required build
   tools, and a browser on another machine for the Linux test. The user must
   approve fresh logins. Do not assume this session has access to both hosts.
4. Use disposable databases/test profiles and fresh package output parents.
   Never overwrite the active Mac Application Support profile, real databases,
   or existing package outputs to make testing convenient. Prefer a separate
   Mac OS test user/VM or an existing safe isolation facility.
5. Do not inspect Pi/Codex credential files or require their installation.
   Real tokens must not enter chat, command lines, shell history, logs, fixtures,
   screenshots, or documentation. The user approves login in their browser.
6. If a platform or live approval is unavailable, complete available checks,
   record exactly what remains unverified, and request the missing access.
   Do not label a cross-platform feature verified after testing one platform.
7. Stay below 300K session tokens, targeting a checkpoint before 200K. Avoid
   large log dumps and persist unfinished verification here.

## Expected implementation

One C++ `OpenAiOAuth` is owned by the shared `ApplicationRuntime`; all
subscription providers in a workspace use it. CHA obtains fresh credentials
through device login with public client ID `app_EMoamEEZ73f0CkXaXp7hrann`.
It does not depend on separately registered CHA credentials or the Pi/Codex
runtime. The compatibility gate is successful operation.

The **OpenAI** page has three states: signed out/Connect ChatGPT, waiting with
`https://auth.openai.com/codex/device`, a user code and Cancel, and connected/
Disconnect. It uses `GET /api/v1/openai/auth` and POSTs with `{}` to
`/api/v1/openai/auth/login`, `/poll`, and `/disconnect`.

The browser schedules one poll at a time while the page is open. C++ enforces
the next-poll time and 15-minute pending deadline. Leaving the page pauses
polling; returning resumes an unexpired attempt. Restart loses pending login.
There is no server-side browser launch, stdin prompt, incoming OAuth callback,
browser token storage, or per-tab auth owner.

Connected credentials are stored only in
`<normalized absolute database path>.openai-auth.json`, an atomically written
regular file with mode 0600. Its four fields are `access_token`,
`refresh_token`, `expires_at` (Unix seconds), and `account_id`.
On Mac it is beside the database in Application Support, outside the bundle.
Missing/invalid data leaves signed-out state without fatal startup; valid
expired data loads for first-use refresh.

Refresh occurs on model use with five minutes remaining and saves the complete
returned bundle before dispatch. One mutex covers auth and saving, not model
streaming; auth network work has an overall 15-second deadline. A refresh
already sent finishes/saves even if generation is canceled. Renewal/save
failure clears memory and attempts file removal, with honest removal errors
and manual reconnect. Model 401 ends one request without replay, billing
fallback, or clearing a newer login. Disconnect removes local credentials but
need not cancel already-credentialed streams or revoke a remote session.

Subscription providers use fixed HTTPS
`https://chatgpt.com/backend-api/codex/responses`, Responses streaming,
Bearer/account headers, and CHA identity. They send full conversation,
nonempty instructions, `stream: true`, `store: false`. First-version
configuration forbids `api_key_env`, explicit temperature/max-token settings,
enabled web search, and non-off cache retention, including effective overrides.
Fresh package seeds select a separately named subscription provider with the
block-2-tested model; the API-key provider remains available. Existing
database selections are not automatically migrated.

## Files and tools in scope

- Existing C++/web tests, `webapp` tests and Playwright harness.
- `scripts/package-linux.sh`, `scripts/check-linux-package.sh`,
  `scripts/test-linux-package-upgrade.sh`.
- `packaging/macos/package.sh`, `runtime-smoke.c`, wrapper/runtime code.
- Shared `packaging/linux/import-seed/` and package fixtures.
- Auth/provider/runtime/UI implementation only for a demonstrated defect,
  accompanied by a focused regression test. Do not broadly refactor.

## Implementation and verification steps

1. Run the full automated checks below. Investigate failures using focused
   logs/tests. Distinguish pre-existing failures from regressions; do not
   report either as passed. Tests must use synthetic auth and mock models,
   never a real subscription.
2. Build each package on its intended platform with a fresh output parent.
   Reuse the existing package/runtime-smoke/upgrade checks. Inspect scripts
   before running because package assembly can replace named output artifacts.
3. On Linux, create disposable data and configuration with the packaged
   `chaweb` import/start options (inspect `--help` or the current scripts).
   Start console mode without an API key, Pi, or Codex. Connect a browser from
   another machine through an existing private arrangement, preferably
   loopback plus SSH forwarding. Do not expose an unauthenticated `chaweb`
   publicly; OpenAI login is not browser-to-CHA authentication.
4. On Mac, launch the packaged app in a disposable profile without an API key.
   Confirm it reaches the UI without a key prompt, opens verification in the
   system browser, and keeps the embedded browser on CHA with its existing
   cookie protection.
5. Run the acceptance sequence below on both platforms. Record actual results
   per platform, not a single combined assumption. Allow the user to approve
   device login; do not display or save raw authentication responses.
6. To exercise renewal without waiting for token expiry, stop the disposable
   runtime and change only its saved `expires_at` to an already-expired Unix
   timestamp, then restart and request generation. Do not edit access/refresh
   token strings, JWT claims, account ID, or the system clock. Inspect only
   necessary metadata and report booleans, never dump the file. Alternatively,
   wait for real expiry. Verify the full returned bundle persists across
   another restart; do not assume the refresh token's string must change.
7. Use synthetic marker tokens to test leakage through browser responses/
   storage, logs, SQLite, export/import, R2 transfers, transcript mirrors, and
   package assets. Real credentials are not suitable search markers or test
   fixtures. Reuse existing tests and transfer facilities; unavailable external
   transfer infrastructure is an explicitly unverified integration check.
8. Transfer only a disposable database/export to a fresh installation and
   verify it is signed out. Existing import/maintenance on an already connected
   workspace must leave its local connection alone. Check private file mode
   and location without printing contents.
9. Recheck generation with an API-key provider and preservation of an existing
   database/provider selection across package replacement. Use mock keys for
   automated checks; any live API-key smoke requires a user-approved test key.
   Do not silently switch a subscription request onto paid API billing.
10. Fix only observed in-scope bugs and add focused regression tests. Rerun
    affected checks and full suites after the final fix. Remove only the exact
    disposable credentials/data created for testing when no longer needed,
    and report cleanup or intentionally retained test locations.

## Manual acceptance sequence

Run this sequence on packaged Linux with a remote browser and on packaged Mac:

| Check | Expected result |
| --- | --- |
| Fresh keyless startup | UI opens and connection is signed out; no Pi/Codex dependency |
| Fresh device login | User approves in browser; common page becomes connected |
| Streamed conversation | Tested model streams, receives prior conversation, and reports completion/usage |
| Restart after login | Connection loads from the private sibling file without another login |
| First-use refresh | Expired/near-expiry access is renewed; complete returned bundle is saved and generation works |
| Restart after refresh | Saved replacement bundle remains usable |
| Leave and return while waiting | Polling pauses and an unexpired attempt resumes |
| Cancel pending login | Attempt is cleared; no late response reconnects it |
| Disconnect | Local connected state/file are removed; new subscription generation asks to reconnect |
| Existing API-key path | Existing key/provider behavior still works without automatic billing fallback |
| Package replacement | Existing database, provider selections, and separately stored connection survive |

For pending/cancel checks, start a separate attempt after disconnecting if
necessary; do not confuse an existing connected account with a pending login.
For deletion failure, the expected behavior is a reported error and honest
warning, not a fabricated successful disconnect. Deterministic tests from
earlier blocks cover concurrency and failure injection; no new stress harness
is required here.

## Verification commands

From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure
```

Use `ctest --test-dir build/ninja -N` to confirm tests exist. Zero tests is
not a pass. For focused diagnostics, discover GoogleTest names with
`./build/ninja/cha_tests --gtest_list_tests` or
`./build/ninja/cha_web_tests --gtest_list_tests`.

From `webapp`, run `npm ci` if dependencies are missing, then:

```sh
npm run check
npm run build
npm run e2e
```

E2E needs the built `chaweb` and Playwright browser dependencies. Use its
existing disposable-data/mock-model harness.

On Linux, from the repository root:

```sh
oauth_linux_package_dir=$(mktemp -d)
./scripts/package-linux.sh oauth-block6 "$oauth_linux_package_dir"
./scripts/check-linux-package.sh "$oauth_linux_package_dir/cha-linux-oauth-block6"
./scripts/test-linux-package-upgrade.sh "$oauth_linux_package_dir/cha-linux-oauth-block6"
```

The package script already invokes package/upgrade checks during assembly;
separate invocations are useful only when testing a rebuilt/existing artifact.
Do not double-run an unchanged expensive check just to produce another log.

On supported macOS, from the repository root:

```sh
oauth_macos_package_dir=$(mktemp -d)
./packaging/macos/package.sh oauth-block6 "$oauth_macos_package_dir"
```

Record the script's runtime-smoke results and perform the real wrapper smoke;
a successful build alone does not verify system-browser navigation or login.

## Completion verification

Complete means all automated suites and applicable package/upgrade checks pass,
both packaged platforms pass the acceptance sequence, and credential isolation
and existing API-key behavior are checked. Any unavailable platform, live login,
or transfer smoke remains explicitly unverified.

Record actual refresh outcomes rather than claiming observed token rotation
from parser behavior. If a compatibility difference is discovered, update
`docs/oauth-protocol.md` with secret-free evidence as well as the handoff.
Do not extend the implementation merely to avoid accepted manual recovery.

## Handoff

This is the durable final verification record. Update it even if work is
incomplete; the final response must summarize the outcome without relying on
earlier chat.

- Status: not started.
- Final changed files and regression fixes: none yet.
- C++/web/browser commands, test counts, and results: not run.
- Linux package path/platform and package/upgrade checks: not run.
- Linux remote-browser acceptance sequence, including refresh/restart: not run.
- Mac package path/platform and runtime/wrapper checks: not run.
- Mac acceptance sequence, including browser approval and refresh/restart: not run.
- Credential-isolation, transfer, permissions, and API-key regression results:
  not run.
- Disposable-data cleanup or intentionally retained test paths: none recorded.
- Outstanding failures/unavailable checks: not assessed.
- Overall implementation verified: no. Change only when required work and
  actual checks are complete; otherwise give the exact remaining actions.
