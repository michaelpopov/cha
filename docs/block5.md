# Block 5 job — Remove workspace reload and backup

## Mission

Implement Block 5 from `docs/plan.md`: remove the broad workspace-reload operation,
directory backup machinery, reload-specific live-session state, protocol values,
OpenAPI operation, web client call, and UI behavior.

This is a deletion/simplification job. Do not replace the removed operation with a
second reload model. The new database-backed narrow mutation path is wired in Block
6.

## Source of truth and prerequisites

- Blocks 1 through 4 must already be implemented and passing.
- Read `docs/design.md`, especially **Workspace reload and backup removal** and
  **Backup and API surface** under required code changes.
- Read Block 5 and the global invariants in `docs/plan.md`.
- Inspect all references to `workspace_reload`, `workspace_reloading`,
  `workspace_reload_failed`, `backup_dir`, and `WorkspaceBackup` before editing.
- Preserve unrelated worktree changes.

## Required result

At the end of this job:

- `POST /api/v1/workspace/reload` does not exist;
- workspace directory `.tar.gz` backup code/configuration is gone;
- `LiveSessionManager` has no global workspace-reload reservation/state;
- C++ protocol enums/mappings contain neither removed value;
- `resources/cha.yaml` and generated TypeScript schema contain neither removed value
  nor reload operation;
- the web client/UI exposes no workspace reload; and
- the separate session-scoped `reloading` reason used by character setting changes
  still behaves as before.

## Expected implementation areas

- `src/web/lobby_routes.*`
- `src/web/workspace_backup.*` and its test/CMake entries
- `src/web/live_session_manager.*`
- `src/web/live_session.*`
- `src/web/protocol.h` and `src/web/protocol.cpp`
- `src/web/application_config.*` for `backup_dir` removal
- `resources/cha.yaml`
- `webapp/src/api/schema.d.ts`, client, components, and tests
- backend route/process/unit tests and build definitions

## Work 1 — Remove backend route and backup machinery

- Remove `POST /api/v1/workspace/reload` and its route registration.
- Remove route state and constructor arguments used only by reload/backup.
- Remove `backup_dir` from application settings and runtime wiring.
- Delete `src/web/workspace_backup.*`, its unit test, and all source/test build
  entries.
- Remove reload-specific error translation and responses.
- Remove `WorkspaceReloadReservation`, `WorkspaceReloadResult`,
  `reserve_workspace_reload`, global reload flags, and related manager branches.
- Remove focused tests for the deleted global operation.
- Preserve forum/session maintenance fencing and all behavior unrelated to the
  removed global reload.

Do not add a database backup command. A correct future database backup would need
SQLite backup semantics and is outside this change.

## Work 2 — Remove hand-maintained C++ protocol values

There is no C++ protocol generator. Hand-edit the C++ sources:

- remove `ShutdownReason::workspace_reloading` from `src/web/protocol.h`;
- remove `ErrorCode::workspace_reload_failed` from `src/web/protocol.h`;
- remove both string mappings from `src/web/protocol.cpp`;
- remove the `workspace_reloading` shutdown-priority case from
  `src/web/live_session.cpp`; and
- remove every route/manager/test use of those values.

Keep `ShutdownReason::reloading`. It is a distinct session-scoped reason used after
character provider/style changes.

## Work 3 — Update the OpenAPI source and generated TypeScript schema

Edit `resources/cha.yaml` as the source of truth:

- remove the workspace reload operation;
- remove `workspace_reloading` and `workspace_reload_failed` enum values;
- remove reload references from bootstrap and unrelated descriptions; and
- ensure remaining route/error descriptions are coherent without the operation.

Then run from `webapp/`:

```sh
npm run api-types
npm run api-types:check
```

`webapp/src/api/schema.d.ts` is generated and must not be hand-edited. The C++ files
in Work 2 are hand-maintained and must be edited directly.

## Work 4 — Remove web client and UI behavior

- Remove the reload client method and request/response types.
- Remove `workspace_reload_failed` from error allowlists/maps.
- Remove reload controls, callbacks, state, notifications, screen cases, comments,
  and tests.
- Simplify UI conditions that treated `workspace_reloading` specially.
- Preserve character-setting UI and its ordinary session reload behavior.
- Update API/client/component tests to describe the remaining behavior rather than
  merely deleting assertions that still matter.

## Required tests

- Build native protocol and server targets after the hand edits.
- Run route, live-session, live-session-manager, application-config, and process tests
  affected by the removal.
- From `webapp/`, run:

  ```sh
  npm run api-types:check
  npm run check
  npm run build
  ```

- Search executable/generated code for:
  - `workspace_reload`;
  - `workspace reloading`;
  - `workspace_reloading`;
  - `workspace_reload_failed`;
  - `backup_dir`; and
  - `WorkspaceBackup`.

Remaining matches may be design/history documentation scheduled for Block 8, but
must not be production code, generated schema, tests of live behavior, or UI.

Run `git diff --check` and inspect generated diffs against `resources/cha.yaml`.

## Out of scope

- Database backup functionality
- Final `--data` CLI and runtime cutover
- SessionRepository ownership changes
- General user/source documentation cleanup, which belongs to Block 8
- Changes to the three supported narrow runtime mutations

## Completion gate

This job is complete only when the route and backup code are absent end to end, C++
protocol hand edits and TypeScript generation agree with the OpenAPI source, all
focused tests pass, and the session-specific `reloading` path remains intact.

In the final session report, list deleted/changed files, generation commands, test
commands, and every intentional remaining textual match for removed terms.
