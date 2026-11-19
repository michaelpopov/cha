# Fix block 4: Vault and R2

**Fixes:** F5, F6, F7, F8, F9 · **Written:** November 19, 2026

## Before you start

- This file is complete. Do not read `docs/fix-plan.md`, the architecture
  reviews in `docs/`, or `specs/p0-d1-design.md`. The P0-D1 facts that this
  block needs are below.
- **Start condition:** P0-D1 (the D1 sync rework) is finished and in `main`,
  all five of its steps. Start from the latest `main`. Check in the code that
  these exist: `q2/` queue table names, the `sync_seen` receipt table, and the
  receipt check in R2 replacement staging. If one is missing, stop and tell
  the user.
- File paths and line numbers come from commit `11ca9039`. Lines can move,
  and P0-D1 changed some of these files. Find the code by the function name.

## P0-D1 facts that this block needs

- Each D1 queue table is named `q2/<vault>/<machine>`. Vault names compare
  without ASCII letter case.
- The worker stops with the missing-table error when the `q2` table of this
  machine is missing. An Event reports it. These cases still stop the worker:
  - Add from R2: the new vault has no table for this machine yet.
  - A new vault copied from a local vault: the new name has no table.
  - A queue table that the user deleted by hand in the Cloudflare console.
  The remedies are "Save D1 credentials" (it makes the table), an R2
  download (it also makes the table), or "Disconnect".
- R2 upload (`src/app/vault_operations.cpp`, near line 1123) collects the note
  flags, attempts the final send, and refuses while outbox rows or note flags
  remain.
- R2 replacement of the active vault has four steps:
  1. Stop the worker, drain local writers, attempt the final send. Refuse if
     outbox rows or note flags remain.
  2. Download and validate the candidate in staging. Check that every receipt
     of the current vault exists in the staged copy. If not, refuse. Nothing
     is installed.
  3. Clear the staged outbox and note flags. Install through the file
     replacement and `.bac` handling.
  4. Reopen and start the worker.
- cha-daemon runs no D1 sync, has no R2 transfer, and has no Settings.

## Approvals

- **ChaWeb permission:** F6, F8, and F9 do not need it. For F5 and F7, find
  out first whether cha-daemon runs the changed path. If it does, ask the user
  for explicit permission before you change code. If the user does not give
  it, stop that fix.
- **UI approval:** before you write code, show the user these texts and get
  approval:
  - F5: the rename refusal text.
  - F6: the new upload refusal text.
  - F7 and F8: the refusal text, only if the existing load error is not used
    as it is.

## Rules

- Make the smallest fix. Do not add machinery.
- Add only the tests that this file names. Run the tests of the changed area.
- Do not change `docs/` or `specs/`. The document updates are a separate task.
- Do not commit unless the user tells you to.
- Order: F5, F6, F7, F8 (it uses F7), F9.

## F5. Refuse a rename of a connected vault

**Decision (user, November 19):** refuse the rename when the vault has D1
credentials.

**Problem:** Vault names are local to each machine. `update_vault` restarts
the worker with the new name. The worker finds no queue table for the new name
and stops. Other machines continue to send to the old table, and nobody reads
it. If the user then selects "Save D1 credentials", this machine finds no
recipients and deletes its outbox without sending it.

**Fix:**

1. In `update_vault` (`src/app/vault_operations.cpp`), refuse the rename when
   the vault has D1 credentials and the new name differs from the old name in
   more than letter case.
2. The error tells the user to disconnect D1 first.
3. Keep a rename that changes only letter case. Use the same ASCII
   case-insensitive name comparison as the `q2` table names.

Do not add queue migration or a coordinated rename.

**Test:** One test: the rename of a connected vault is refused, and a
case-only rename passes.

## F6. R2 upload text when the worker is stopped

**Problem:** When the queue table of this machine is missing, the worker
stops. Local edits still add outbox records. R2 upload then refuses with
"Changes are not sent to D1 yet. Try again when D1 can be reached." Waiting
does not help. The only remedies are "Save D1 credentials" and "Disconnect".
The P0-D1 Event reports the missing table, but the upload text is still
wrong.

**Fix:** When the worker is stopped, use a different refusal text that names
"Save D1 credentials" (`src/app/vault_operations.cpp:1136-1142`). Keep the
current text for the other cases.

**Test:** No new test.

## F7. Check the workspace before a vault switch

**Problem:** The switch precheck verifies the database identity, schema, and
contents. It does not load the workspace configuration. The switch then
drains the current vault and loads the new workspace. If that load fails, the
application becomes unavailable until a restart. A restart opens the previous
vault, because CHA saves the selection only after success.

**Fix:**

1. In the precheck (`require_switchable_database`,
   `src/app/application_config.cpp:695`), also load the candidate workspace
   with the normal `Workspace::load` rules.
2. On failure, refuse the switch with the load error. The current vault stays
   open.

Do not keep the previous target and its lease open as a fallback for all later
errors. The precheck covers the realistic case with less machinery.

**Test:** One test: a switch to a vault with invalid configuration is
refused, and the current vault stays usable.

## F8. Check the workspace of a staged R2 copy

**Problem:** R2 replacement validates the staged SQLite file, installs it,
and loads the workspace only at reopen. A file that passes the integrity check
but has invalid configuration makes the application unavailable after the
install. The `.bac` pair still holds the previous files.

**Fix:**

1. Run the F7 workspace check on the staged copy before the install.
2. Put the check next to the receipt check in staging (replacement step 2
   above).
3. On failure, refuse with the load error. Nothing is installed.

**Test:** One test: a staged copy with invalid configuration is refused, and
the current vault does not change.

## F9. R2 replacement without the `.toml` object

**Problem:** Upload puts the database first and the `.toml` definition after
it. If the `.toml` upload fails on the first push, replacement download
refuses with "R2 vault definition was not found" until the next good upload.
The new-vault download already works without `.toml`.

**Fix:** In the replacement path (`src/app/r2_database_transfer.cpp:1160`),
when `.toml` is missing, use the definition of the active vault.

Do not move the definition into S3 object metadata.

**Test:** Change the existing companion-failure test
(`tests/app/unit_r2_database_transfer.cpp:487`) so that it expects a good
replacement.

## Not in this block

- Add from R2 stops sync until "Save D1 credentials": no change. F6 fixes
  only the upload text.
- A vault copy keeps the D1 credentials: no change.
- The uploader marker after reinstall, the protected new-vault download, and
  one `.bac` pair: intended behavior. No change.
- Torn reads across uploads: no change. One uploader at a time.
- Do not change the P0-D1 receipt rules, the queue format, or the worker.

## Done when

- F5 to F9 are in the code, with the approved texts.
- The named tests pass, and the tests of the changed areas pass.
- Your final report says, for each fix, what changed, the ChaWeb check result
  for F5 and F7, and which tests ran.
