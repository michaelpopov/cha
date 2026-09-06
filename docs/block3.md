# Block 3: Vault protocol, switch route, and browser selector

## Task and prerequisites

Connect CHA's implemented vault switching to the browser. This is block 3 of
four sequential implementation blocks. This file is a self-contained brief;
no previous conversation or block file is required. Confirm these prerequisites
in the repository before editing:

- `--config` takes a directory with required `app.toml` (`vault`, `[web]`,
  `[logging]`) and one `.toml` per vault (`vault_name`, `data`, optional
  `mirror`/`modify`). The validated registry is fixed until restart, sorted
  by ASCII-folded name, with canonical spelling preserved.
- `ApplicationCommand` holds that registry, configuration directory, and
  startup vault. `ApplicationRuntime::current_vault() const` returns the active
  `VaultDefinition` copy. `CurrentVault` is the stable shared selection owner.
- `void ApplicationRuntime::switch_vault(std::string_view name)` is implemented
  and directly tested. It throws `UnknownVaultError` for an unknown name.
  Switching drains live sessions, retargets stable store/repository/mirror
  objects, republishes the workspace, and persists selection without changing
  the HTTP listener or port.
- Invalid target pre-checks leave the old sessions untouched. Busy leases or
  drain failures report ordinary maintenance errors. Failed reopening reports
  `WorkspaceRestartRequiredError`; the runtime requires restart. Mirror rebuild
  and config persistence failures are logged after a successful switch and do
  not fail it. Selecting the active vault is a no-op.
- OAuth and `Providers` are application-wide. Old provider requests are
  cancelled when their sessions drain; switching does not replace either owner.
- Block 2 provides coherent guarded repository/lobby access, including a
  bootstrap access scope that cannot mix workspaces and sessions across a switch.

If these capabilities are missing, identify the missing prerequisite instead
of silently implementing other blocks. Inspect the actual guarded APIs left
by block 2 and use them. This block adds the HTTP route, wire fields, browser
validation, stale-page checks, selector, and their tests together.

## Working rules and behavior

Read applicable `AGENTS.md`/`CLAUDE.md`, inspect `git status`, and preserve
unrelated changes. Keep the implementation small; no custom selector framework,
vault-management UI, background discovery, or new recovery architecture.
Implement this block only. Do not edit `docs/` or general user documentation,
and do not commit credentials, databases, `.env`, or runtime output. Smaller
commits are optional. Paths below are repository-relative.

`docs/vaults-design.md` is the reference; its relevant contract follows. One
vault is active for the whole application process, including all browser tabs
and the native web view. The selector switches that shared runtime. After
success, the initiating page navigates to `/` with a full reload and starts in
the target vault's Welcome session. Do not preserve its previous conversation,
forum URL, draft, or screen across vaults.

Other tabs retain the existing bounded SSE recovery behavior. An old session
absent from the new database may end in the existing retry state. If recovery
finds matching IDs in another vault, the first snapshot's vault name forces
reload. In particular, `entrance/welcome` IDs exist in every vault. No broadcast
channel, activation epoch, polling endpoint, or appended-event identity is
needed. Returning to the same vault uses its complete current snapshots.

## Protocol contract

`GET /api/v1/bootstrap` adds two required properties to its existing response:

```json
{
  "vault_name": "Personal",
  "vaults": ["Personal", "Projects"],
  "initial_forum_id": "entrance",
  "initial_session_id": "welcome",
  "personas": [],
  "characters": [],
  "forums": [],
  "recent_sessions": []
}
```

`vault_name` is the non-empty canonical active name. `vaults` is the sorted
registry's canonical name list and contains `vault_name`. Other bootstrap
properties describe that same vault; they must not be assembled across a switch.

Every `SessionSnapshot`, including initial SSE, stopping, and finished
snapshots, carries required `vault_name`. Its value is captured when that live
session opens, not looked up when an old snapshot is serialized. Append events
remain unchanged: each stream begins with a checked snapshot and is closed
when its live session drains.

```http
POST /api/v1/vault/switch
Content-Type: application/json

{"vault_name":"Projects"}
```

Use existing mutation-origin/content-type/body-limit validation. Match names
through the runtime's ASCII case-insensitive lookup. Success is `204 No Content`
after the operation completes; selecting the active name also returns 204.
Malformed bodies and unknown names return `400` with `bad_request`. Maintenance
or pre-check failures return `500` with `internal_error` and the maintenance
message. A fatal reopen error is reported through that existing error format;
do not promise that a runtime requiring restart remains usable. Do not extend
the protocol's closed error-code set.

## Backend and schema implementation

- `src/web/protocol.h` and `.cpp`: add `std::string vault_name` to both
  `Bootstrap` and `SessionSnapshot`; add `std::vector<std::string> vaults` to
  `Bootstrap`. Update both JSON serializers.
- `src/session/opened_session.h`: add `std::string vault_name`. The production
  opener in `ApplicationRuntime::Impl` fills it with the active canonical name
  while the opener is protected by live-session admission/drain semantics.
- `src/web/live_session.h` and `.cpp`: copy/move the opened name into a session
  member during `open_controller()`. Stamp it in `make_snapshot()` after
  `to_snapshot(...)`. Preserve `to_snapshot`'s signature and ensure all
  snapshot-producing paths pass through this method. Low-level test openers
  can leave the field empty when not exercising wire validation; production
  snapshots and shared wire fixtures must have a non-empty name.
- `src/web/lobby_routes.h` and `.cpp`: provide access to `CurrentVault` and
  the immutable canonical name list, updating every constructor call. Build
  the name, workspace details, and recent sessions in the **same guarded
  access scope** established in block 2. A mutex-protected name read outside
  that scope is insufficient. Update `tests/web/unit_lobby_routes.cpp` fixtures
  along with runtime construction.
- `src/web/application_runtime.cpp`: install the switch route in `start()`
  beside the lobby routes. Parse a string `vault_name` using
  `validate_json_mutation` and `parse_route_json_body`, call `switch_vault`,
  and map the exceptions above with `set_error_response`. Keep it a thin
  adapter; do not duplicate switching or maintenance logic in the handler.
- `resources/cha.yaml`: add bootstrap's two properties to `required`, add
  required snapshot `vault_name`, and use non-empty string schemas. Add
  `/api/v1/vault/switch`, a `VaultSwitchRequest` object with required string
  `vault_name`, and 204/400/500 responses using the existing `ErrorResponse`.

Regenerate `webapp/src/api/schema.d.ts` once the schema changes are complete:

```bash
(cd webapp && npm run api-types)
```

## Browser implementation

### Client, validation, and fixtures

In `webapp/src/api/client.ts`, add
`switchVault(vaultName: string): Promise<void>` to `ChaClient`, implemented with
the existing `requestEmpty`, `jsonMutation`, and `/api/v1/vault/switch` endpoint.
Handle 204 and errors the same way as existing empty-response mutations.

`isSessionSnapshot` must reject a missing/invalid vault-name field. In
`webapp/src/state/bootstrap.ts`, `validateBootstrap` requires a non-empty
`vault_name` and a non-empty list of non-empty strings containing the active
name. Names received from the server are canonical: compare them exactly in
the browser rather than introducing another case-fold implementation.

Update `webapp/src/test/fixtures.ts`: bootstrap defaults can use `Personal`
and `['Personal', 'Projects']`, snapshots use `Personal`, and `fixtureClient`
stubs `switchVault`. Find additional inline snapshot/bootstrap literals and
hand-built client implementations using `rg`; change the schema, consumers,
and fixtures in this block so no incompatible intermediate handoff is needed.

### Page identity and snapshot admission

In `webapp/src/components/App.tsx`, accept an optional testable
`reload?: () => void` prop, defaulting to `window.location.assign('/')`.
Record the first successfully validated bootstrap's canonical name as the
identity of this page. Do not silently replace that identity when a later
`bootstrap-refreshed` arrives; if that response names another vault, reload
before applying it. A same-vault refresh keeps existing behavior.

Use one small boolean check for incoming snapshot names. When a name differs,
discard the snapshot, stop the relevant stream/recovery path through existing
cleanup, request reload, and do not continue dispatching or attaching streams
from that result. Apply the check to normal `getSessionSnapshot` results,
recovery probes, and all SSE `onSnapshot` callbacks before publishing state.

Keep the callers' existing success actions. `performOpen` currently dispatches
`conversation-opened`, whereas stream/probe updates dispatch `session-snapshot`.
The shared check must not replace both with one dispatch; doing so would lose
normal navigation/state transitions. Preserve navigation-generation checks and
the existing bounded retry behavior. Use existing cleanup paths rather than
adding a second recovery system.

### Switch state and sidebar

In `webapp/src/state/view.ts`, add the planned switch state:

```ts
vaultSwitch: {
  status: 'idle' | 'pending' | 'failed';
  message: string | null;
}
```

Initialize/reset it to idle on `bootstrap-loaded`. Add `vault-switch-started`
and `vault-switch-failed` actions, with the failure carrying its public message.
In `App.tsx`, the switch handler dispatches started, awaits `client.switchVault`,
and reloads on success. On failure, dispatch the existing `publicErrorMessage`
treatment with fallback `The vault could not be switched.` Pass the callback
to `Sidebar`; do not add optimistic vault/workspace replacement.

In `webapp/src/components/Sidebar.tsx`, put a native
`<select aria-label="Vault" className="cha-vault-select">` and the existing
Settings gear in a `cha-sidebar-footer` row. Options are the bootstrap names;
its controlled value is bootstrap's `vault_name`. Selecting the same name does
nothing. Disable the control until bootstrap is ready and while switching.
Render failure text in a `role="alert"` line above the footer. On a recoverable
failure the controlled value stays with the active vault, so no separate
selection rollback is needed.

In `webapp/src/styles/app.css`, use existing Settings/control tokens. Let the
select take available row width while the gear stays on the right. Check the
narrow sidebar layout. No custom popup, extra progress screen, or preserved
cross-vault view state is required.

## Tests and verification

Establish baseline results with `make test` and
`(cd webapp && npm run check)`. Install locked browser dependencies with `npm ci`
in `webapp` only if needed. Keep unrelated pre-existing failures separate.

Extend backend tests:

- `tests/web/unit_protocol.cpp`: both serializers emit the new fields.
- `tests/web/unit_lobby_routes.cpp`: bootstrap reports active name and the
  registry list with matching workspace/session data; health remains free of
  session data. Update every route constructor fixture.
- `tests/web/unit_live_session.cpp`: an opener-provided vault name appears on
  snapshots throughout the session lifecycle, including the first SSE snapshot.
- `tests/web/unit_application_runtime.cpp`: use the real route to switch A to
  B case-insensitively; assert 204, unchanged origin, B's bootstrap and newly
  opened snapshot, old-session closure, and persisted canonical name. Cover
  active no-op, malformed/unknown names (400), pre-check/maintenance failures
  (500), and preservation of existing mutation validation. Reuse block-2
  lifecycle tests rather than duplicating every low-level failure test.

Extend browser tests in `state/bootstrap.test.ts`, `api/client.test.ts`,
`state/view.test.ts`, `components/App.test.tsx`, and
`components/Sidebar.test.tsx`:

- Validators reject missing/invalid fields and inconsistent bootstrap names.
- `switchVault` sends the expected JSON POST, accepts 204, and reports errors.
- Selector options, current value, disabled state, same-name no-op, selection
  callback, pending state, and error display are correct.
- Successful switching requests reload; recoverable failure keeps the current
  selection and re-enables the control.
- Matching snapshots retain normal open/probe/SSE behavior. Mismatched ones
  trigger reload and never reach application state, history changes, or a new
  stream attachment. Include `entrance/welcome` across two vault names.
- A bootstrap refresh naming another vault reloads rather than changing the
  page's comparison identity. Same-vault refresh and A-to-B-to-A recovery retain
  the existing full-snapshot behavior.
- Recovery of a missing old session remains bounded and ends in the existing
  retry state; no new retry ladder is introduced.

Add a served browser test in `webapp/e2e/shell.spec.ts` or a focused vault test
file. Extend `webapp/e2e/start-cha.mjs` with a second temporary imported vault
if needed. Exercise the real switch endpoint, full reload to `/`, and the new
vault's Welcome/bootstrap data; retain unique database filenames and cleanup.
A route mock returning 204 can supplement but cannot alone prove integration.
Inspect the footer in normal and narrow layouts.

Run after implementation:

```bash
make test
(cd webapp && npm run check)
make web-e2e
```

Report unavailable browser/platform checks explicitly. If backend locking was
necessarily changed, also run the `tsan` configure/build preset and its CTest
suite; otherwise block 2 owns that check. Do not hand off stale generated types.

Finish with a concise report of wire/API changes, browser behavior, and check
results. The feature must work through the test-served browser at this point;
block 4 only integrates native setup, packages, launchers, and documentation.
