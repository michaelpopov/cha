# ChaWeb vault commands handoff

The implementation of `specs/vaults-1pager.md` is saved in the working tree. The native Welcome input handles `/open <name>`, `/download`, and `/download confirm`. It uses the existing vault maintenance and backup paths, validates new downloads before clearing the downloaded vault's local D1 inbox, and starts the worker in recovery mode on ChaWeb vault switches. The browser pins requests and audio URLs to the daemon and epoch. A changed context remounts ChaWeb at Welcome and drops prior drafts, routes, and media.

Before the first live run, the ChaWeb production build, all 1,834 web tests, and all 15 nginx integration tests passed. The native suite passed all new vault tests. The full run had 1,673 passes, two live-provider skips, and two existing tests that failed on temporary database access under parallel execution. Both passed when rerun individually. The latest development results and integration starting point are below.

To repeat local verification, run `cmake --build build/ninja --target cha_tests cha_app_tests cha_daemon_tests cha_prepare_test_vault`, `ctest --test-dir build/ninja -L daemon --output-on-failure`, `npm --prefix webapp run check`, `npm --prefix webapp run build:chaweb`, and `python3 tests/integration/daemon_integration_test.py --build-dir build/ninja`. The native and nginx tests need local sockets. The staged removal of `specs/vaults-design.md` and the untracked `specs/vaults-test.md` are separate work present in the workspace.

## Acceptance run checkpoint, November 13, 2026

Stopped at the user's request during preflight. No live acceptance cases ran, no implementation changes were made in this continuation, and no Cloudflare resources or temporary daemons were created.

- The earlier D1 campaign documents the Cloudflare credential variables in `~/.zprofile`. The expected D1 and R2 variable names are present. Their values were not printed and their access was not tested. The configured R2 endpoint's suitability as a dedicated test bucket is unverified.
- The prior campaign used the SSH alias `vm`, configured for the host name `ubuntuserver`. At this checkpoint the local SSH probe could not resolve it. The resumed run below records how access was restored.

## Resumed acceptance run, November 13, 2026

The Ubuntu VM was suspended. After resuming it, SSH worked and its clock synchronized. The two Linux daemons ran with separate private profiles, sockets, and loopback HTTPS origins. The live run used the user-designated R2 bucket, the object prefix `VA72e491`, and a disposable D1 database.

The live harness reported PASS for L01–L07 on one Atlas checkpoint and for L08–L11 on a rebuilt checkpoint. Some steps used HTTP where the spec calls for browser interaction, so those browser steps remain open for formal acceptance. L11 cleared a queued edit from B's halted Delta inbox, preserved its table and index, and restored sync in both directions. The live Chromium checks rendered a D1 edit in each direction on Delta and observed an Atlas tab return to Welcome and clear its draft.

The development fix limits the browser bootstrap epoch to the daemon's unsigned 64-bit range. On the corrected production bundle, Chromium verified stale history after local Open and same-vault Download, reload after a same-epoch daemon restart, and rejection and retry of seven invalid bootstrap payloads. The web suite passed all 1,840 tests. Native regression finished with 1,675 passes and two provider skips. All 15 nginx integration tests passed.

The already-started tests have finished. The remaining acceptance work belongs to the integration stage, including the prescribed live browser steps and unexecuted controlled, desktop, provider, and media variants. The evidence is in [build/vault-acceptance-72e491](build/vault-acceptance-72e491). `live.jsonl` contains the snapshot ETags, queue names, results and cleanup checks. `browser-control.jsonl` contains the Chromium checks against the corrected bundle. The partial controlled evidence is in `controlled.jsonl`, `timing.jsonl`, `d1.jsonl`, `dirty.jsonl`, and `context-matrix.jsonl`.

Cloudflare confirmed deletion of the disposable D1 database and all six R2 objects. The private Ubuntu test directory, test daemons, and SSH forwards were removed. The Ubuntu VM remains running.

## Development fixes before integration

- **Stale session requests.** Session selection and cleanup use the request's original epoch. Vault maintenance advances the runtime epoch before releasing its session reservation. A stale request receives `vault_changed` and cannot select or retire a session with the same ID in the new vault. [Application session opening](src/app/application.cpp#L650) · [Session manager](src/runtime/live_session_manager.cpp#L583)
- **Missing R2 database.** The asynchronous Welcome result includes `error_code: "not_found"` in the transcript snapshot and retains the human-readable message, including exact-name guidance for remote Open. The input POST still acknowledges dispatch immediately with HTTP 204. The code stays with the notice when its transcript is read back from storage. [Welcome result handling](src/app/vault_operations.cpp#L317)

**Focused verification.** All 33 unit tests passed, including 10 native vault and session-runtime tests, one ChaWeb adapter test, and 22 ChaWeb client tests. The new regressions cover stale selection and cleanup with overlapping session IDs, missing R2 objects in both command paths, transcript readback, and the browser client's handling of the result code. The native test starts with a vault that lacks the new metadata column. The generated API types check and TypeScript typecheck passed. No integration tests were run after these fixes.

**Integration starting point.** Build the current working tree, verify `vm` access, and provision fresh disposable fixtures in the designated R2 test bucket `mpopov2012` and disposable D1 databases. Follow the current [acceptance spec](specs/vaults-test.md) from L01 through L12, then run its command, failure, browser, and desktop checks. The earlier L01–L11 logs use a previous revision of the acceptance spec and include HTTP substitutions for prescribed browser steps. Use them as reference evidence for the new run.
