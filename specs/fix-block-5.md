# Fix block 5: Removals

**Fixes:** F18, F19 · **Written:** November 19, 2026

## Before you start

- This file is complete. Do not read `docs/fix-plan.md`, the architecture
  reviews in `docs/`, or `specs/p0-d1-design.md`. The P0-D1 facts that this
  block needs are below.
- **Start condition:**
  - P0-D1 (the D1 sync rework) is finished and in `main`. Check in the code
    that the `sync_seen` receipt table and the `q2/` queue tables exist. If
    not, stop and tell the user.
  - Fix block 1 (it changes the credential checks in
    `src/workspace/workspace_config_store.cpp`) and fix block 2 (it changes
    `src/session/session_controller.cpp`) are in `main`. If not, ask the user
    before you start.
  - Start from the latest `main`.
- File paths and line numbers come from commit `11ca9039`. Lines can move,
  and other work changed some of these files. Find the code by the function
  name.

## P0-D1 facts that this block needs

- Each D1 operation has an `op_id`. A receiver applies the record and writes
  its receipt (a row in `sync_seen`) in one local transaction, then
  acknowledges the operation.
- A permanent failure, for example an unknown record kind, skips the
  operation: a log warning and an Event, no receipt, and acknowledgement.
- The worker also reads this machine's legacy `q/` table, if it exists, with
  the old receive code. Legacy records write no receipt.

## Approvals

- **ChaWeb permission:** F18 changes `cha_lib` code that cha-daemon also
  runs. For F19, find out first whether ChaWeb shows or handles
  `web_reader_api_key`; if it does, F19 also needs the permission. Before you
  change code, ask the user for explicit permission. If the user does not give
  it, stop. Run ChaWeb tests only with this permission.
- **UI approval:** before you write code, show the user and get approval:
  - F18: what the user sees after typing `##<folder>` or `/detach`.
  - F19: the Settings screen without the web reader key field.

## Rules

- Make the smallest change. Do not add machinery.
- Add only the tests that this file names. Run the tests of the changed area.
- Do not block an operation because the configuration has an obsolete
  setting. Ignore it and write a log warning.
- Do not change `docs/` or `specs/`. The document updates are a separate task.
- Do not commit unless the user tells you to.

## F18. Remove the dead `##folder` attachment

**Decision (user, November 19):** remove `##folder` and `/detach`.

**Problem:** `##<folder>` stores a `session_folders` row, sends
`chat.add_folder` to D1, and says "Added folder ... to this chat."
Generation never reads that row. The forum Library replaced it.

**Fix:**

1. Remove the `##folder` command, `/detach` (it detaches only folders), their
   messages, the `session_folders` writes, and `post_welcome_folder_prune`
   (`src/session/session_controller.cpp:520-570`,
   `src/storage/session_database.cpp:781-859`,
   `src/storage/folder_store_storage.cpp:96-105`).
2. Drop the `session_folders` table in the schema upgrade. Remove the code
   that reads or clears it.
3. Treat an incoming `chat.add_folder` or `chat.detach_folder` record as
   applied: change nothing, write a log warning, and write the receipt in the
   normal apply transaction. Old records then cause no Event.
4. If the legacy `q/` read path also handles these kinds, make it ignore them
   with a log warning.

Do not make `/detach` remove notes and files. That is a new feature.

**Test:** One test: an incoming `chat.add_folder` record is acknowledged with
no Event. Also open a vault made by the current build to check the schema
upgrade.

## F19. Remove the unused `web_reader_api_key`

**Decision (user, November 19):** remove the field.

**Problem:** Settings, the TOML configuration, the key-in-use checks, and the
credential guards handle `web_reader_api_key`. The web reader never sends it.
CHA logs "web_reader API key is saved but unused".

**Fix:**

1. Remove the field from the Settings UI, the bridge, and the key checks.
2. When a configuration still has the value, ignore it and write a log
   warning.

Do not add a Bearer header. That is a new service contract.

**Test:** One test: a configuration with the old key loads and logs a
warning.

## Not in this block

- Do not change the web reader or the web tools. Welcome must keep
  `web_search` and `web_read`. They are required.
- Do not change the `history_epoch` filters. Vaults that older builds cleared
  need them.

## Done when

- F18 and F19 are in the code, with the approved UI.
- The named tests pass, and the tests of the changed areas pass.
- Your final report says, for each fix, what changed, the ChaWeb check result
  for F19, and which tests ran.
