# CHA architecture fix plan

**November 19, 2026** · Plan for the fixes from the architecture reviews

## Sources

- [Carbon review](carbon-arch-review.md) (November 17, 2026): the original findings.
- [Opus opinion](opus-arch-review.md) and [Astra opinion](astra-arch-review.md)
  (November 18, 2026): checks of the Carbon findings against commit
  `11ca9039`, and findings that Carbon missed.
- [P0-D1 design](../specs/p0-d1-design.md) (November 18, 2026): the D1 fix
  that is in progress.

This plan uses only these documents. File paths and line numbers come from the
reviews at commit `11ca9039`. Lines can move. Find the code by the function
name.

Out of scope:

- Carbon Flaw 2 (several processes open one vault). That function is removed.
- ChaWeb-only work: cha-daemon concurrency, the voice-input wait loop, ChaWeb
  routes, and `docs/chaweb.md`. ChaWeb development is on hold.

## Rules for all items

- **Keep it simple.** Make the smallest fix that solves the problem
  (`AGENTS.md`). Where a review proposes more machinery, this plan uses the
  smaller fix and says why.
- **UI approval.** Before you change visible text or controls, show the
  proposed change to the user and get approval. Each item names its texts.
- **ChaWeb permission.** Most fixes change `cha_lib` code that cha-daemon also
  runs. Before such an item, get the user's explicit permission. The "ChaWeb"
  column shows which items need it. "Check" means: at the start, find out
  whether cha-daemon runs the changed path.
- **Tests.** Add a test only for real behavior. Each item names its test, if
  any. Run the tests of the changed area.
- **Measure first** where an item says so. If the measurement shows no real
  cost, close the item with no change.
- **Documents.** Code items do not change `docs/` or `specs/`. Phase 5 lists
  the document changes. Do them only on the user's request.

## Decisions

| Topic | Decision | Source |
| --- | --- | --- |
| D1 P0 scope | As in `specs/p0-d1-design.md`. Automatic eviction is removed (changed R46). | User, November 18 |
| Note request time limit | One finite overall limit of 7200 s for all note requests, with or without Flex. | User, November 19 |
| Welcome web tools | Keep `web_search` and `web_read` in Welcome sessions. They are required. | User, November 19 |
| Rename of a connected vault | Refuse it when the vault has D1 credentials. | User, November 19. The error text needs UI approval (F5). |
| `/cover` | Keep the current behavior. Correct the User Manual. | User, November 19 |
| Jev first reply | Start the reply when Jev picks a character. Do not wait for the session name. | User, November 19 |
| Google and Anthropic effort | Send thinking settings in the provider requests. | User, November 19 |
| `##folder` and `/detach` | Remove both (F18). | User, November 19 |
| `web_reader_api_key` | Remove the field (F19). | User, November 19 |
| Picture fetch to local network addresses | No change. | User, November 19 |
| First D1 pass at vault open | Keep it (R37). P0-D1 keeps it. | D1 design |

## Overview

Priorities: **P1** = do next, in parallel with P0-D1 or right after it.
**P2** = do soon. **P3** = do when you work in that area.
Size: **S** = a few lines. **M** = a larger change in one area.

| ID | Fix | Priority | Size | Depends on | UI approval | ChaWeb |
| --- | --- | --- | --- | --- | --- | --- |
| P0-D1 | D1 data integrity (in progress) | P0 | — | — | Yes | Yes (§6) |
| F1 | Finite time limit for note requests | P1 | S | — | No | Yes |
| F3 | D1 token in `Redactor` | P2 | S | — | No | Yes |
| F4 | Base path in the credential destination check | P2 | S | — | No | Yes |
| F5 | Refuse a rename of a connected vault | P1 | S | P0-D1 | Yes | Check |
| F6 | R2 upload text when the worker is stopped | P2 | S | P0-D1 | Yes | No |
| F7 | Check the workspace before a vault switch | P2 | S | — | Check | Check |
| F8 | Check the workspace of a staged R2 copy | P2 | S | F7, P0-D1 | Check | No |
| F9 | R2 replacement without the `.toml` object | P3 | S | — | No | No |
| F10 | Jev: start the first reply at once | P2 | M | — | No | Yes |
| F11 | Read transactions for `note_list` and `note_get` | P2 | S | — | No | Yes |
| F12 | One keyed connection for the audio lookup (measure first) | P3 | S | — | No | Yes |
| F13 | Skip the annotation rebuild when nothing changed (measure first) | P3 | S | — | No | Yes |
| F14 | Shorter write transactions in folder import | P3 | S | — | No | Check |
| F15 | Thinking settings for Google and Anthropic | P2 | M | — | Mapping check | Yes |
| F16 | `max_completion_tokens` for OpenAI Chat Completions | P3 | S | — | No | Yes |
| F17 | Drop a stale provider override at load | P3 | S | — | No | Yes |
| F18 | Remove the dead `##folder` attachment | P3 | M | P0-D1 | Yes | Yes |
| F19 | Remove the unused `web_reader_api_key` | P3 | S | — | Yes | Check |

## Phase 0: P0-D1 (in progress)

The P0-D1 design covers these review findings. Do not plan them again.

| Review finding | P0-D1 coverage |
| --- | --- |
| Pieces of two senders mix in one queue (Carbon 1.1, Astra 1.1) | `q2` tables with `op_id`, `UNIQUE (op_id, piece)`. Each piece is confirmed before the final piece (§1, §2). |
| Row IDs restart after a delete, so unread rows are deleted (Astra 1.1) | Every page starts at the head. Acknowledgement deletes exact `op_id` values (§2). |
| Full D1 drops the largest queue table in any vault (Carbon 1.2, Astra 1.2) | No automatic eviction. A full D1 stops sending until space is free (§2 Capacity). |
| The worker stops with only a log line (Carbon 1.2d) | Events report a missing table, skipped records, and transport or capacity trouble (§2 Diagnostics). |
| Settings-created forum has no `forums` row (Carbon 1.3, Opus P1.1, Astra 1.3) | Forum rows in the config transaction, and a repair at workspace open (§3). |
| `folder_summary` is lost when `note.update` makes a note (Carbon 1.4b, Astra 1.4) | `note.update` carries the flag (§3). |
| Duplicate apply after a retry or a lost acknowledgement; a repeated turn delete hits another turn (Carbon 1.5, Astra 1.5) | Receipts in `sync_seen`. Apply and receipt in one transaction (§1, §2). |
| Transient storage errors are acknowledged as skips; rejected config deltas are silent (Astra 1.5) | Transient errors stay in D1 for a retry. Permanent failures are skipped with an Event (§2 Failure classes). |
| Desktop R2 replacement applies queued records again (Carbon 1.6a, Astra 1.6) | Receipt check in staging. Queued records that the copy contains are acknowledged by receipt (§4). |
| ChaWeb R2 download clears the D1 inbox (Carbon 1.6a) | cha-daemon does not run D1 sync (§6). |

Open gates of P0-D1 (from the design):

1. Before implementation, test three D1 behaviors through the REST API:
   `DELETE` on a full database, the batch result of
   `INSERT ... ON CONFLICT DO NOTHING`, and `DELETE ... WHERE op_id IN (...)`
   with 50 parameters (§7).
2. Show the Event texts (§2) and the R2 replacement error texts (§4) for UI
   approval before implementation.
3. Get ChaWeb permission for the cha-daemon change (§6).

D1 findings that P0-D1 does not cover are F5 and F6 in this plan, or are in
"Not planned".

## Phase 1: Safety and D1 follow-ups

### F1. Finite time limit for note requests

**Sources:** Carbon Flaw 3, Opus P2.3, Astra §3.

**Problem:** `prepare_note_provider` sets `timeout_s = 0` for every note
request: Compressed, L1, L2, L3 sections, L3 overview, and Save summary. It
does this also when Flex is off. The idle timer starts only at the first body
byte. A server that keeps the connection open and sends nothing holds the
single summary batch slot. A new Summarize then reports that summarization is
already running. Only a window reload, a vault switch, or quit frees the slot.
TCP keepalive does not help, because the connection stays alive.

**Fix:**

1. In `prepare_note_provider` (`src/notes/notes_operations.cpp:103`), replace
   `timeout_s = 0` with 7200 s. `CURLOPT_TIMEOUT` also limits the streamed
   body, so the value must cover a long L3 overview.
2. Do not change the idle timer in `ProviderClient`. Keep cancellation as it
   is.

This changes the Flex design rule that keeps an unlimited first-byte wait
(`specs/flex-design.md`).

**Test:** No new test. If the existing timeout test
(`tests/bridge/unit_bridge_router.cpp:4783`) expects the unlimited value,
change its expectation.

### F3. D1 token in `Redactor`

**Sources:** Carbon 7.1, Opus P3.1, Astra 7.1.

**Problem:** `Redactor` collects the provider keys, the R2 secret, and the
OAuth secrets. It does not collect the D1 API token. The Assistant log and
Event tools send their text to the model. No code logs the token today, so
there is no known leak.

**Fix:** Add `d1_storage()->api_token` to the secret list
(`src/app/assistant_service.cpp:180-211`).

**Test:** Add a dummy D1 token to the existing redaction test
(`tests/app/unit_assistant_service.cpp:1120`).

### F4. Base path in the credential destination check

**Sources:** Carbon 7.3b, Opus P3.11, Astra 7.3.

**Problem:** The saved-key guard compares the provider's scheme, host, and
port. It ignores the base path. On a shared gateway that routes by path, a
`base_path` change can send a saved key to a different service on the same
host.

**Fix:** Add the base path to the destination string in
`credential_destination_pairs` (`src/workspace/workspace_config_store.cpp:1283,
1339`; the guard is at `2055`).

**Test:** One test: a `base_path` change with a saved key is refused.

### F5. Refuse a rename of a connected vault

**Sources:** Carbon 1.6b, Opus P2.2, Astra 1.6. Not in P0-D1.

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
3. Keep a rename that changes only letter case. Vault names compare without
   ASCII letter case (P0-D1 §1).

Do not add queue migration or a coordinated rename.

**Depends on:** P0-D1 (`q2` table names and the name comparison).

**UI approval:** The error text.

**Test:** One test: the rename of a connected vault is refused, and a
case-only rename passes.

### F6. R2 upload text when the worker is stopped

**Sources:** Opus P2.1 (Carbon missed it). Not in P0-D1.

**Problem:** When the queue table of this machine is missing, the worker
stops. Local edits still add outbox records. R2 upload then refuses with
"Changes are not sent to D1 yet. Try again when D1 can be reached." Waiting
does not help. The only remedies are "Save D1 credentials" and "Disconnect".
After P0-D1, an Event reports the missing table, but the upload text stays
wrong. These stop cases remain after P0-D1: Add from R2, a vault copied from a
local vault, and a queue table that the user deleted by hand.

**Fix:** When the worker is stopped, use a different refusal text that names
"Save D1 credentials" (`src/app/vault_operations.cpp:1136-1142`).

**Depends on:** P0-D1.

**UI approval:** The new text.

**Test:** No new test.

## Phase 2: Recoverability

### F7. Check the workspace before a vault switch

**Sources:** Carbon 6.4, Opus P3.13, Astra 6.1.

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
errors (Astra). The precheck covers the realistic case with less machinery.

**UI approval:** Only if the refusal needs a new text.

**Test:** One test: a switch to a vault with invalid configuration is
refused, and the current vault stays usable.

### F8. Check the workspace of a staged R2 copy

**Sources:** Astra 6.1.

**Problem:** R2 replacement validates the staged SQLite file, installs it,
and loads the workspace only at reopen. A file that passes the integrity check
but has invalid configuration makes the application unavailable after the
install. The `.bac` pair still holds the previous files.

**Fix:**

1. Run the F7 workspace check on the staged copy before the install.
2. On failure, refuse with the load error. Nothing is installed.
3. Put the check next to the P0-D1 receipt check in staging (P0-D1 §4,
   step 2).

**Depends on:** F7 and P0-D1 (implementation step 5).

**UI approval:** Only if the refusal needs a new text.

**Test:** One test: a staged copy with invalid configuration is refused, and
the current vault does not change.

### F9. R2 replacement without the `.toml` object

**Sources:** Carbon 6.3a, Opus P3.12, Astra 6.2.

**Problem:** Upload puts the database first and the `.toml` definition after
it. If the `.toml` upload fails on the first push, replacement download
refuses with "R2 vault definition was not found" until the next good upload.
The new-vault download already works without `.toml`.

**Fix:** In the replacement path (`src/app/r2_database_transfer.cpp:1160`),
when `.toml` is missing, use the definition of the active vault.

Do not move the definition into S3 object metadata (Astra). Opus advises
against it, and the fallback is smaller.

**Test:** Change the existing companion-failure test
(`tests/app/unit_r2_database_transfer.cpp:487`) so that it expects a good
replacement.

## Phase 3: Responsiveness

### F10. Jev: start the first reply at once

**Sources:** Carbon 5.3, Opus §3, Astra 5.3. User decision, November 19.

**Problem:** For the first message of an unnamed chat, Jev classification
(5 s limit) and the session name request (10 s limit) start together. The
reply starts only after both finish. The reply can wait up to 10 s, also when
Jev picks the character in 100 ms. Chats without Jev already name the session
in parallel with the reply.

**Fix:**

1. In `SessionController` (`src/session/session_controller.cpp:753, 789`),
   start the reply when classification gives a result.
2. Apply the name when it arrives: in `finish_classification` if it is
   ready, else in `poll_session_name`.
3. Make the cancel, expiry, and rejected-submission paths call
   `cancel_session_name()`. A late name must not rename a chat whose first
   message was cancelled.
4. Keep the current rule for the first name of a shared session.

**Test:** Change the existing behavior test (`tests/runtime/unit_jev.cpp:717`)
to the new order. Add one test: a cancelled first message gets no late name.

### F11. Read transactions for `note_list` and `note_get`

**Sources:** Carbon 5.2 item 3, Opus P3.2, Astra 5.2.

**Problem:** `note_list` and `note_get` use the default `SqliteTransaction`
mode, which is `BEGIN IMMEDIATE`. Each read takes the write lock and waits
behind D1 apply and session writes.

**Fix:** Use `SqliteTransaction::Mode::read` in both functions
(`src/storage/note_storage.cpp:1406-1418, 1511-1546`). Keep
`synchronous = FULL`.

**Test:** No new test. Run the notes storage tests. Run the ChaWeb notes tests
only with ChaWeb permission.

### F12. One keyed connection for the audio lookup (measure first)

**Sources:** Carbon 5.2 item 1, Opus P3.3, Astra 5.2.

**Problem:** `SessionRepository::cached_audio_entries` opens a new database
connection for each full snapshot
(`src/storage/session_repository.cpp:813-828`). In a protected vault, each
open runs the SQLCipher key derivation (PBKDF2, 256,000 iterations). A turn
makes about 2 to 4 snapshots. Nobody has measured the cost.

**Fix:**

1. Measure the snapshot time on a protected vault.
2. If the cost is visible, run the same query on the journal's open
   connection. That connection belongs to the same thread.
3. Keep fresh audio state and the epoch-safe teardown.

**Test:** No new test.

### F13. Skip the annotation rebuild when nothing changed (measure first)

**Sources:** Carbon 5.2 item 4, Opus P3.4, Astra 5.2.

**Problem:** In a folder with an ANNOTATIONS note, each `note_save` calls
`rebuild_folder_annotations`. It decompresses and parses every note of the
folder inside the write transaction. Autosave runs every 5 seconds while you
type.

**Fix:**

1. Measure with a real annotated folder.
2. If the cost is visible, compare the old and new annotations of the saved
   note. Skip the rebuild when they are equal
   (`src/storage/note_storage.cpp:619-684, 1589`).

**Test:** One test: a save that changes no annotation does not rebuild.

### F14. Shorter write transactions in folder import

**Sources:** Carbon 5.2 item 5, Opus P3.5, Astra 5.2.

**Problem:** A journal write that waits more than 5 s for the write lock
throws, and the live session stops. The reply in progress is lost. The chat
can be opened again. The clear cause is `note_import_folder`. It copies all
notes and all audio in one write transaction.

**Fix:**

1. Copy the notes in one transaction, as today.
2. After that, copy the audio in separate short transactions
   (`src/storage/note_storage.cpp:2235-2345`). If an audio copy fails, the
   notes stay. Audio is a cache that CHA can make again.
3. Keep the current fail-closed session behavior.

Do not keep a session alive after a failed journal write. That needs
reconciliation logic.

**Test:** No new test. Run the folder import tests.

## Phase 4: Provider fixes and cleanup

### F15. Thinking settings for Google and Anthropic

**Sources:** Carbon Flaw 8 row 3, Opus P3.7, Astra 8.3. User decision,
November 19.

**Problem:** The Chat Effort picker shows for Google and Anthropic
characters. Neither encoder sends thinking settings, so the value has no
effect. `workspace.cpp` logs that effort is ignored.

**Fix:**

1. Before code, write a short table that maps each effort value to the Gemini
   `thinkingConfig` and the Anthropic `thinking` request fields. Use the
   current API documentation. Show the table to the user.
2. Send the mapped values in `src/providers/google_api.cpp` and
   `src/providers/anthropic_api.cpp`.
3. Remove the "effort is ignored" warnings
   (`src/workspace/workspace.cpp:288-296`).
4. Keep `provider_supports_reasoning_effort` as it is. If it returns false
   for Google, Gemini Summarize fails for every effort value other than None.

**Test:** One encoder test for each provider: one effort value gives the
expected request field. Run the Gemini summary tests.

### F16. `max_completion_tokens` for OpenAI Chat Completions

**Sources:** Carbon Flaw 3 (related bug), Opus P3.6, Astra §3.

**Problem:** Chat Completions sends `max_tokens` when `config.max_tokens` is
set (`src/providers/chat_completions_api.cpp:179-181`). OpenAI reasoning
models reject it. This affects chats and the Flex test
(`src/notes/notes_operations.cpp:453`). It occurs only when `max_tokens` is
set by hand. Settings has no such field.

**Fix:** For direct OpenAI hosts, send `max_completion_tokens` in place of
`max_tokens`, in the encoder. Keep `max_tokens` for other Chat Completions
backends.

**Test:** One encoder test.

### F17. Drop a stale provider override at load

**Sources:** Carbon Flaw 8 row 8, Opus §4, Astra 8.8.

**Problem:** When the saved `provider_id` of a session names a deleted
provider, `initialize()` keeps the override and warns again on each turn.

**Fix:** Reset the missing `provider_id` at load
(`src/session/session_controller.cpp:382-387`). Do not change the
`model_locked` behavior.

**Test:** No new test.

### F18. Remove the dead `##folder` attachment

**Sources:** Carbon Flaw 8 row 1, Opus P3.8, Astra 8.1.

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
   applied: write a log warning and a receipt. Old records then cause no
   Event under the P0-D1 failure rules.

Do not make `/detach` remove notes and files. That is a new feature.

**Depends on:** P0-D1 (record handling and receipts).

**UI approval:** `##folder` and `/detach` stop working. Show what the user
then sees and get approval.

**Test:** One test: an incoming `chat.add_folder` record is acknowledged with
no Event.

### F19. Remove the unused `web_reader_api_key`

**Sources:** Carbon Flaw 8 row 5, Opus P3.9, Astra 8.5.

**Problem:** Settings, the TOML configuration, the key-in-use checks, and the
credential guards handle `web_reader_api_key`. The web reader never sends it.
CHA logs "web_reader API key is saved but unused".

**Fix:**

1. Remove the field from the Settings UI, the bridge, and the key checks.
2. When a configuration still has the value, ignore it and write a log
   warning (`AGENTS.md` rule for obsolete settings).

Do not add a Bearer header. That is a new service contract.

**UI approval:** Settings without the field.

**Test:** One test: a configuration with the old key loads and logs a
warning.

## Phase 5: Documents (only on the user's request)

Changes that come from this plan:

- `docs/UserManual.md:437-438`: say that a switch to another chat, a Settings
  save, and a restart remove the `/cover` boundary. The user chose this on
  November 19.
- `docs/UserManual.md`: remove `##folder` and `/detach` (F18).
- `docs/AdminManual.md`: the P0-D1 changes (no eviction, Events, `q2` tables,
  replacement refusal, cha-daemon without D1), the rename refusal (F5),
  effort for Google and Anthropic (F15), and the removed `web_reader_api_key`
  (F19, line 933).
- `specs/flex-design.md`: the finite time limit (F1).
- `specs/current-design.md`: the Jev reply starts before the name (F10).

Existing drift (Opus P3.17, Astra §9):

- `docs/README.md`: add the Admin Manual link. Remove "R2 vault actions" for
  ChaWeb.
- `docs/MaintainerGuide.md`: ElevenLabs is supported (line 1043). Add D1 to
  the subsystem overview.
- `docs/tutorial.md`: add `src/notes` and `src/daemon` to the repository map.
  Lines 245-247 say that `/clear` is not available, but it exists.
- `docs/UserManual.md`: it still describes standalone compression and the
  older summary cancel and folder behavior (near line 934).
- `docs/AdminManual.md`: the sync payload table omits L3.
- `docs/chaweb.md`: stale routes. Defer while ChaWeb is on hold.

## Not planned

| Finding | Source | Reason |
| --- | --- | --- |
| Several processes open one vault | Carbon Flaw 2 | Function removed. |
| Stable note sync IDs; path collisions | Carbon 1.4a, Astra 1.4 | R54. P0-D1 keeps paths. Accepted risk (P0-D1 §8). |
| Session and forum ID collisions on two machines | Astra 1.5 | R48 and the one-machine-at-a-time rule (R33). |
| Add from R2 stops sync until "Save D1 credentials" | Carbon 1.6a, Astra 1.6 | P0-D1 §4: no change. An Event reports it. F6 fixes the upload text. |
| Vault copy keeps D1 credentials | Carbon 1.6c, Astra 1.6 | R56. The copy uses its own queue names. |
| Each note update sends all blobs; one copy for each recipient | Carbon 1.7a-c, Astra 1.7 | R47 and R23. Measure only if D1 traffic becomes a problem. |
| Input checks for D1 account ID, database ID, and machine name | Carbon 1.7d-e, Astra 1.7 | The values come only from the owner's Settings and `app.toml`. |
| Background first D1 pass | Carbon 5.1, Astra 5.1 | R37. P0-D1 keeps the pass at vault open. The macOS window cannot respond during the pass. Revisit if startup is slow in practice. |
| Network calls under `lifecycle_mutex`; quit waits for maintenance | Carbon 5.4, Opus P3.14, Astra 5.4 | The wait is deliberate. These run only on user actions. Revisit if quit hangs in practice. |
| `synchronous = NORMAL` | Carbon 5.2 | Durability choice. Keep `FULL`. |
| Double validation at session open | Carbon 5.2 item 2 | The scan is for one session. Small cost. |
| File and mirror I/O on the runtime thread | Carbon 5.2 item 6 | Overstated. Measure first if chats stall. |
| C++ Markdown converter; durable summary jobs | Carbon Flaw 4.1 | Fixed on November 18. Both opinions reject the converter. |
| EPUB unzip cost and the 16 MiB limit | Carbon 4.2 | Change only when a real book fails. A message about skipped pictures needs UI approval. |
| Pictures in `Workspace::source_files_` | Carbon 6.1 | Overstated. Avatars are small. |
| Audio in the vault database; R2 upload preparation cost | Carbon 6.2, Astra 6.4 | A sidecar file is a large change. Measure first. One small option (Opus): skip the first integrity check. |
| R2 definition in S3 metadata; torn reads across uploads | Carbon 6.3a, Astra 6.2 | F9 is the smaller fix. One uploader at a time. |
| Uploader marker after reinstall; protected new-vault download; one `.bac` | Carbon 6.3b-d, Astra 6.3 | Intended behavior. |
| Keep the previous target and lease open after any reopen error | Astra 6.1 | F7 and F8 cover the realistic cases. |
| Redact `host_config_read` | Carbon 7.2 | Breaks `host_config_write`. |
| Local network addresses in picture fetch and `web_read` | Carbon 7.4, Astra 7.4 | User decision, November 19. |
| Remove the web tools from Welcome | Carbon 7.3, Opus P2.4, Astra 7.3 | User requirement, November 19: Welcome must search and read the web. |
| Confirm each Welcome write | Astra 7.3 | Not requested. It adds a step to each write. |
| Persist `/cover` | Carbon 8.2 | User decision, November 19. Phase 5 corrects the manual. |
| Remove the `history_epoch` filters | Carbon 8.4 | Needed for vaults that older builds cleared. |
| Hardcoded Gemini Library model | Carbon 8.6a, Opus P3.10 | Change the constant when Google retires the model. No new setting for a future need. |
| Gemini store metadata calls | Carbon 8.6b | Narrow. |
| OpenAI store delete removes shared files | Carbon 8.6c | Accepted decision. |
| `Application` to `NotesService` forwarders | Carbon 8.7 | Cosmetic. |
| Provider override on a `model_locked` forum | Carbon 8.8, Astra 8.8 | Product decision. The forum provider already wins. |
| SSE and response buffer limits; multicast queue | Carbon 8.9, Astra 8.9 | No failure seen. Revisit if memory use grows in practice. |
| `body["include"]` overwrite; tool-limit message; prompt rows per target | Carbon 8.9 | Works today. Keep. |
| `/detach` removes notes and files | Carbon 8.1 | New feature, not a fix. |
| Keep a session alive after a failed journal write | Carbon 5.2 item 5 | Needs reconciliation logic. F14 shortens the competing write. |
| cha-daemon thread pool; voice-input wait loop | Carbon 5.4 | ChaWeb on hold. |

## Order of work

1. Finish P0-D1 (Phase 0).
2. Do F1, F3, and F4. These do not depend on P0-D1.
3. After P0-D1: do F5 and F6. Do F8 with P0-D1 implementation step 5, or
   after it.
4. Do F7, then F9.
5. Do F10, F15, and F11.
6. Do the P3 items when you work in their areas: F12 and F13 (measure first),
   F14, F16, F17, F18, and F19.
7. Do Phase 5 when the user asks for it.

## Approvals to get

- **UI approval:** the P0-D1 Event and R2 replacement texts, F5, F6, F7 and
  F8 if they need new texts, F18, and F19. Show the F15 mapping table before
  code.
- **ChaWeb permission:** P0-D1 §6, F1, F3, F4, F10, F11, F12, F13, F15,
  F16, F17, and F18. Check F5, F7, F14, and F19 at the start.
