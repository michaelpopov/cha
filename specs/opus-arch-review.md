# Opinion on the carbon architecture review

**Date:** November 18, 2026
**Subject:** `docs/carbon-arch-review.md` (dated November 17, 2026)
**Code checked:** `main` at commit `11ca9039`
**Excluded by request:** Flaw 2 (multi-process vault sharing). Commit `220da282` removed that function.

## 1. Verdict

The carbon review has good research in some areas. Its main verdict is too
strong. It says that CHA has structural flaws that cause silent data loss,
permanent deadlocks and broken features. The current code does not support
that claim.

The review has four general problems:

1. **It ignores the approved D1 sync requirements.** Most of its D1 findings
   are requirements or accepted risks in `specs/d1-alt-design.md`: R9 and R23
   (no versions, no sequence IDs, duplicates accepted), R33 (one machine at a
   time), R37 (one sync pass before the screens load), R46 (drop the largest
   queue table when D1 is full), R52 (turn delete by position) and R54 (notes
   named by path). The review judges D1 sync against concurrent use of several
   machines. The owner rejected that scenario.
2. **It has factual errors.** Examples: it says that the provider client has
   no TCP keepalive, but it has one. It says that session open scans the full
   vault, but the scan is for one session. It says that a busy database
   destroys a session permanently, but the session can be opened again. It
   says that the note picture fetch is reachable from ChaWeb, but it is not.
3. **It is out of date for summaries.** The commits of November 18 removed
   the modal summary dialog, removed `compression_mutex` and the standalone
   compression path, and made each L3 section a saved note version.
4. **Many proposed fixes add machinery.** Examples: a C++ Markdown parser, a
   second audio database file, per-sender sequence numbers in D1, an immutable
   sync ID for notes, S3 metadata headers, and a thread pool for `cha-daemon`.
   These go against `AGENTS.md` and, for D1, against the owner's decisions.

The review also missed some real problems. Section 2 includes them.

Rough count of the review's separate claims (about 60):

- About 5 are real defects that are worth a fix soon.
- About 18 are real but small. They are low-priority cleanups or performance
  points.
- About 20 describe approved design or accepted risks.
- About 15 are wrong, overstated, or already fixed.

Only one item is a clear "fix now" defect: a forum created in Settings does
not get its `forums` row, so D1 drops the incoming chats for that forum.

## 2. Findings to fix, by priority

### P1: fix now

#### P1.1 A forum created in Settings has no `forums` row, so D1 drops its incoming chats (carbon 1.3)

**Correctness of the review:** Correct. The trigger is narrower than the
review says.

**Evidence:** `Application::create_forum` (`src/app/application.cpp:1193-1199`)
calls `workspace::create_forum`. That function goes to `Impl::edit`, which
calls `commit_and_publish` with `surviving_forums = nullptr`
(`src/workspace/workspace_config_store.cpp:1681-1683`). The `INSERT INTO
forums` loop runs only inside `if (surviving_forums)` (`1623-1630`). A machine
that receives the configuration record passes `&surviving`, so the receiver
gets the row. The creator does not. When a chat record arrives for a forum
that has no row, the Worker skips it and deletes it from D1
(`src/app/d1_sync.cpp:305-308`).

**Real impact:** The creating machine gets the row at the next vault open
(`synchronize_forums`), at the next maintenance reopen, or when it creates a
chat in that forum. Until then, records for chats in that forum are lost. A
normal sequence: create a forum on the Mac, leave CHA open (or asleep), chat
in that forum on Windows, return to the Mac. The Mac drops every record of
that chat. After a restart, later records create the chat with only the later
turns.

**Recommended fix:** Call `sessions->synchronize_forums(...)` in
`Application::create_forum`, as `assistant_service.cpp:932` already does.
Another option is to move the `INSERT` loop out of the `if` block, so every
local edit inserts its forum rows. Both are a few lines.

### P2: should fix, or needs an owner decision

#### P2.1 A stopped D1 Worker blocks R2 upload with a misleading message (missed by the review)

**Evidence:** When the queue table of this machine is missing, the Worker
sets `stopped_` and writes only a log line (`src/app/d1_sync.cpp:77-81`).
After that, `flush_send` and `run_loop` return at once (`98-108`). Local edits
still add records, because `sync_connected` checks only the credentials and
the machine name. R2 upload then refuses with "Changes are not sent to D1 yet.
Try again when D1 can be reached." (`src/app/vault_operations.cpp:1136-1142`).

**Real impact:** The silent stop is an accepted risk (design section 13). The
upload refusal is not in the design. Its text tells the user to wait for D1,
but waiting does not help. The only way out is "Save D1 credentials" or
"Disconnect". All stop cases lead here: a dropped queue table (1.2), a vault
downloaded from R2 as a new vault (1.6a), a renamed vault (1.6b) and a vault
copy (1.6c).

**Recommended fix:** Use a different refusal text when the Worker is stopped,
for example one that names "Save D1 credentials". This is a UI text change, so
it needs your visual approval first.

#### P2.2 Renaming a connected vault breaks sync in both directions (carbon 1.6b)

**Correctness of the review:** Correct, and the real effect is worse than the
review says.

**Evidence:** `update_vault` ends maintenance with `SyncStart::resume`. The new
Worker uses the new name (`src/app/application.cpp:418`). The first pass finds
no `q/<new name>/<machine>` table and stops. Design section 12.3 does not list
a vault rename as a stop case.

**Real impact:** Vault names are local to each machine. Other machines keep
writing to `q/<old name>/<this machine>`, which nobody reads, until D1 is full
and eviction drops it. If you then select "Save D1 credentials", this machine
finds no `q/<new name>/*` recipients. In that case `send_pass` deletes the
local records without sending them (`src/app/d1_sync.cpp:150-160`). A rename
that changes only letter case is harmless.

**Recommended fix:** The smallest fix is to refuse a rename of a connected
vault, with an error that says to disconnect first. That error is a visible
change and needs approval. Another option is to document "rename the vault
on every machine, then select Save D1 credentials on each". Do not add queue
migration.

#### P2.3 Note requests have no time limit (carbon Flaw 3)

**Correctness of the review:** Partly correct. The mechanism is real. The
impact is much smaller than the review says, and two parts are wrong or
already fixed.

**Evidence:** `prepare_note_provider` sets `provider.config.timeout_s = 0`
(`src/notes/notes_operations.cpp:103`) for every note request (Compressed,
L1, L2, L3 section, L3 overview and Save summary), with or without Flex. The
idle timeout starts only after the first body byte
(`src/providers/provider_client.cpp:132-135`). The provider client has no
low-speed limit.

**What the review gets wrong:**

- `CURLOPT_TCP_KEEPALIVE` is set (`provider_client.cpp:1157`), so a dead peer
  or a dropped route is found after about 10 minutes. Keepalive does not help
  when a live server or proxy holds the socket and sends nothing.
- `compression_mutex` and standalone compression no longer exist (commit
  `54b35bdd`). No lock is held during the LLM call.
- The "leaked `Application` at exit" is a deliberate safety choice. Cancel
  works before the first byte, because the progress callback reads the cancel
  flag about once per second. Exit therefore stops a stuck request.

**Real impact:** A silent server keeps the single summary batch slot until you
reload the window, switch the vault or quit. A new Summarize then reports that
summarization is already running. The batch has no Cancel action by design.

**Recommended fix:** Replace `timeout_s = 0` with a large finite limit in one
place, for example 7200 seconds. `CURLOPT_TIMEOUT` also limits the streamed
body, so the value must cover a long L3 overview. Do not change the
`ProviderClient` idle-timer logic as the review proposes.

#### P2.4 Welcome sessions combine web tools with configuration write tools (carbon 7.3)

**Correctness of the review:** Correct.

**Evidence:** In Welcome, only provider-native web search is off. CHA's own
`web_search` and `web_read` tools stay on when they are configured
(`src/session/session_controller.cpp:922-940, 1015-1017`). They sit next to
`vault_config_apply`, `host_config_write` and `assistant_logging`. No write
needs a confirmation from the user.

**Real impact:** Text on a page that `web_read` fetches can tell the model to
change the configuration or turn on debug logging. Secrets are masked in tool
output, `vault_config_apply` has undo, and host changes need a restart. These
limits reduce the damage but do not remove it.

**Recommended fix:** Do not pass `web_search_tool` and `web_read_tool` when
`maintenance` is true (about two lines). This removes web lookups from
Welcome. It is your decision whether Welcome needs them.

#### P2.5 `/cover` is lost when you switch chats or save Settings (carbon Flaw 8, row 2)

**Correctness of the review:** The facts are correct. The cause is wrong, and
the review misses the most common case.

**Evidence:** The cover boundary is only in memory (`src/chat/transcript.cpp:294-315`).
Nothing stores it or replays it at load. The review says that eviction at 8
sessions loses it. In fact, CHA refuses a ninth session; it does not evict.
The real cause: when you select another chat, the previous chat is retired
when it is idle (`src/runtime/live_session_manager.cpp:605-610, 664-667, 284-299`).
A Settings reload also restarts sessions.

**Real impact:** `docs/UserManual.md:437-438` says that a restart removes the
cover. It does not say that a chat switch removes it. After you switch away
and back, the covered messages go to the model again. A user who covered
messages to hide them from the model does not expect this.

**Recommended fix:** Choose one:

- Store `covered_until` in one column on `sessions`, read it at load, and keep
  it local to the machine (no D1 record).
- Or keep the current behavior and correct the User Manual.

### P3: low priority (real, but small or rare)

Do these as small cleanups when you work in the same area.

#### P3.1 `Redactor` does not include the D1 API token (carbon 7.1)

Correct. `assistant_service.cpp:180-211` collects API keys, the R2 secret and
OAuth secrets, but not `d1_storage()->api_token`. No code logs this token
today (it is only in the `Authorization` header), so there is no known leak.
The fix is about three lines of defense in depth.

#### P3.2 `note_list` and `note_get` take the write lock (carbon 5.2, item 3)

Correct. `SqliteTransaction` defaults to `Mode::write` (`BEGIN IMMEDIATE`).
`note_list` (`src/storage/note_storage.cpp:1406-1418`) and `note_get`
(`1511-1546`) use that default for single reads. These calls are not on the
session runtime thread. The effect is short waits behind D1 apply or session
writes. The fix is `Mode::read`. These functions are shared with ChaWeb, so
the change and its ChaWeb tests need your permission.

#### P3.3 A new keyed database connection for every session snapshot (carbon 5.2, item 1)

Correct for protected vaults only. `SessionRepository::cached_audio_entries`
opens a new connection on each call (`src/storage/session_repository.cpp:813-828`).
SQLCipher 4 defaults apply (PBKDF2, 256,000 iterations). The review
overstates the frequency: snapshots are coalesced and are not per token, so
a turn has about 2 to 4 of them. The fix is to run the same query on the
journal's open connection, which belongs to the same thread.

#### P3.4 Each autosave rebuilds the folder annotations (carbon 5.2, item 4)

Correct for folders that have an ANNOTATIONS note. `note_save` calls
`rebuild_folder_annotations` (`note_storage.cpp:1589`), which decompresses
and parses every note in the folder inside the write transaction. Autosave
runs every 5 seconds while you type. The review proposes no fix. The smallest
fix: compare the old and new annotations of the saved note, and skip the
rebuild when they are equal.

#### P3.5 A long folder import can stop a running chat reply (carbon 5.2, item 5)

Partly correct. A journal write that waits more than 5 seconds for the write
lock throws, and the session stops with "This session stopped after a
failure." The conversation is saved and the session can be opened again. Only
the reply in progress is lost. The review names book import and annotation
rebuild as causes. They are normally short. The clear cause is
`note_import_folder` (`note_storage.cpp:2235-2345`), which copies all notes
and audio in one write transaction. The review's fix (keep the session after
a failed write) needs reconciliation logic. It is better to copy audio in
separate short transactions.

#### P3.6 OpenAI Chat Completions sends `max_tokens` (carbon 3e)

Partly correct. Responses API requests are correct. Chat Completions sends
`max_tokens` when `config.max_tokens` is set
(`src/providers/chat_completions_api.cpp:179-181`). OpenAI reasoning models
reject it. This affects chats too, not only Flex. It occurs only if you set
`max_tokens` by hand, because the Settings UI has no such field. The fix is
to send `max_completion_tokens` for direct OpenAI hosts, in one place.

#### P3.7 Google and Anthropic ignore reasoning effort (carbon Flaw 8, row 3)

The fact is correct. The review's first fix is unsafe. If
`provider_supports_reasoning_effort` returns false for Google, Summarize
fails for Gemini with any effort other than None, which breaks the Gemini
Flex summary path. The Chat effort picker does not use that function. The
correct fix is to send the thinking settings in `google_api.cpp` and
`anthropic_api.cpp`.

#### P3.8 Dead `session_folders` (carbon Flaw 8, row 1)

Correct. `##folder` stores a row, sends `chat.add_folder` to D1 and tells the
user "Added folder ... to this chat.", but generation never reads it. Removal
is valid cleanup, with two conditions: the Worker must still accept old
`chat.add_folder` and `chat.detach_folder` records and skip them with a
warning, and the shared chat input code affects ChaWeb (permission needed).
The review also proposes that `/detach` remove notes and files. That is new
behavior, not cleanup. It needs your requirement.

#### P3.9 Unused `web_reader_api_key` (carbon Flaw 8, row 5)

Correct, and documented in `docs/AdminManual.md:933`. Removal is plain
cleanup.

#### P3.10 Hardcoded Gemini Library model (carbon Flaw 8, row 6a)

Correct: `folder_store_client.cpp:39` has `"gemini-3.8-flash"`. Library search
breaks when Google retires that model. The review's fix ("use the forum's
Google provider") does not always apply, because the Library can serve
characters of other providers. A configuration value is the simple fix. Part
6b is narrower than stated (it affects only the duplicate-name check at
upload start). Part 6c is your accepted decision.

#### P3.11 `credential_destination_pairs` ignores `base_path` (carbon 7.3b)

Correct, narrow. It matters only for shared gateways on one host, not for
`api.openai.com`. Adding the path to the destination string is a small fix.

#### P3.12 R2 replacement download needs the `.toml` object (carbon 6.3a)

Correct, narrow. A failed `.toml` upload on the first push blocks replacement
download until the next good upload. The new-vault download already works
without `.toml`. The smallest fix is the same fallback in the replacement
path. Do not move metadata into S3 headers.

#### P3.13 A vault switch failure makes the application unavailable (carbon 6.4)

Correct, rare. A `Workspace::load` failure on the target vault sets
`app.unusable` (`src/app/vault_operations.cpp:500-512`). The review misses one
mitigation: the vault selection is written only after success, so a restart
opens the previous vault. Configuration is checked at each commit, so the
trigger is rare. A fallback to the old vault is sound but not urgent.

#### P3.14 Native quit waits behind D1 HTTP calls (carbon 5.4)

Correct, but the wait is deliberate. `cha_runtime_join_shutdown` waits for
maintenance without a limit, so that the app does not exit during a file
replacement. The realistic long holders are `save_d1_storage` and
`disconnect_d1_storage`, which make HTTP calls under `lifecycle_mutex`. These
run only when you select those actions. Keep the wait. Fix it only if quit
hangs in practice.

#### P3.15 EPUB import (carbon 4.2)

Partly correct. The repeated `unzipSync` calls only read ZIP headers again;
they do not decompress other entries. The cost is small, and the review's
O(N²) impact is overstated. The 16 MiB request limit is real. Pictures that
do not fit are skipped and nothing tells the user. Change this only if real
books hit the limit. A message about skipped pictures needs UI approval.

#### P3.16 Optional: choose the D1 eviction victim by size (carbon 1.2b)

Section 11 of the design says "counts the rows". `SUM(LENGTH(record))` in
place of `COUNT(*)` is a one-line change that picks the largest table by
bytes. This is a design change, so it is your decision.

#### P3.17 Documentation drift (carbon Flaw 9)

Partly correct. Change the documents only when you ask for it.

- `docs/README.md`: it links the User Manual. It does not link the Admin
  Manual. It still lists "R2 vault actions" for ChaWeb.
- `docs/chaweb.md`: the vault upload, download and merge routes and
  `DELETE .../audio` are stale. These review claims are wrong: chaweb.md does
  not say that Notes are unavailable; Welcome and dictation are documented;
  `/notes/<id>/audio/*` routes and `/__welcome__` do not exist. The Welcome
  `/open` and `/download` commands are not documented.
- `docs/MaintainerGuide.md`: the lock text is now correct. "ElevenLabs output
  is no longer supported" (line 1043) is wrong. D1 is not described.
- `docs/tutorial.md`: the repository map omits `src/notes` and `src/daemon`.
  Lines 245-247 say that the app does not expose transcript clearing, but
  `/clear` exists.

## 3. Findings that describe approved design or accepted risks

These findings are true descriptions of the code. The behavior comes from
your decisions, mostly in `specs/d1-alt-design.md`. I do not recommend a
change. Where the review's fix goes against a decision, I say so.

- **1.1 Pieces from two senders mix in one queue.** The mechanism is correct.
  It needs two machines that send records larger than 1.9 MB to one queue at
  the same time. That is outside the one-machine-at-a-time rule (R33). The
  review also says that a mixed record always fails to parse. That is not
  certain: the cut can give valid JSON, but the content checks will probably
  reject it. The review's fix (record IDs and per-sender sequence numbers)
  goes against R9 and R23.
- **1.2a, 1.2c Eviction looks at the queue tables of all vaults.** Section 11
  says "each queue table in the D1 database". A vault filter would make stale
  tables of another vault impossible to evict. At most one table is dropped
  per loop, and the tables of this machine are never dropped.
- **1.2d A stopped Worker has no visible sign.** Accepted risk in section 13.
  A visible sign needs UI approval. Section 2 (P2.1) gives the related problem
  that the design missed.
- **1.4a Notes are named by path, so two notes with the same new name can
  overwrite each other.** This follows from R25 and R54. It needs both
  machines to create the same path before they exchange records. An immutable
  note sync ID goes against R54.
- **1.5a, 1.5b Records can arrive twice.** Accepted by R23 and section 13.
- **1.5c `chat.delete_turn` names the turn by position.** Your decision R52.
  It deletes the wrong turn only after an earlier duplicate.
- **1.5d A rejected `config.edit` record leaves the receiver different.**
  R12 and R14. It occurs only after an earlier difference.
- **1.6a(i) Desktop R2 download does not clear the D1 inbox.** Section 12.2
  says "The cleanup does not delete incoming records from D1". The result is
  duplicates, which R23 accepts. Clearing the inbox would lose the records
  sent after the upload, so this is a trade-off for you.
- **1.6a(ii) A vault downloaded from R2 as a new vault stops sync at first
  open.** Section 12.3 lists this case, with "Save D1 credentials" as the
  recovery. The replacement download creates the queue table. This is the
  most likely way to reach the silent stop and the misleading R2 message
  (P2.1). If you want, one `create_queue_table` call at the end of the
  new-vault download removes the case.
- **1.6c A vault copy keeps its D1 credentials.** R56. The copy uses its own
  queue table names, so nothing mixes.
- **1.7a, 1.7c Each note update sends all blobs, and one request carries a
  copy for each recipient.** R47 and R23.
- **5.1 One sync pass runs before the screens load.** R37. The review is right
  that the macOS window cannot be used until the pass ends, because
  `cha_runtime_create` runs on the main actor. The review's "30 s per request"
  is wrong: curl has a 10 s connect limit and stops below 1 byte per second
  for 30 s. Moving the pass to the background is one line, but it changes R37.
- **5.3 The first reply of a new Jev chat waits for the session name.**
  `specs/current-design.md` says "the answer starts after both finish". The
  wait is up to 10 s. The review's "one-line fix" is not complete: the cancel
  and expiry paths would also need `cancel_session_name()`, or a late name
  could rename a chat whose first message was cancelled. This is your
  decision.
- **`PRAGMA synchronous = FULL`.** A durability choice, not a defect.
- **6.2 Audio is in the vault database.** The cost of R2 upload grows with the
  audio size, and R2 drops audio by design. A second audio database is a large
  change. One small option: skip the first integrity check, because the copy
  is checked again after `VACUUM`.
- **6.3c A protected vault cannot be downloaded from R2 as a new vault.** The
  code refuses it on purpose (design section 12.4). Replacement download of
  the open protected vault works.
- **6.3d Only one `.bac` backup exists.** The Welcome notice describes one
  backup.
- **`cha-daemon` serves one request at a time.** Documented in the code and in
  `docs/chaweb.md`. ChaWeb is on hold.
- **8.6c OpenAI store deletion removes shared files.** Your accepted decision.

## 4. Findings that are wrong, overstated, or already fixed

- **Flaw 2 (multi-process vaults).** Excluded. Commit `220da282` removed it.
- **Flaw 3: "no TCP keepalive", "compression deadlock", "leaked Application".**
  Wrong or fixed. See P2.3.
- **Flaw 4.1 summaries in a blocking modal, L3 sections only in a JS array.**
  Fixed on November 18 (`42a60bbf`, `54b35bdd`, `bafeefce`). Summarize now
  runs in the background, the editor and chat stay usable, and each L3
  section is a saved note version. A window reload or vault switch cancels
  the batch, as the background summary design requires. Saved versions stay,
  and a new run reuses them. The loop still runs in the browser, and the only
  Markdown-to-ProseMirror converter is `webapp/src/notes/summaryImport.ts`.
  A C++ converter would duplicate it and add a parser dependency. I do not
  recommend it.
- **1.4b `folder_summary` is lost.** Partly correct but rare. `note.create`
  carries the flag. An update to an existing note keeps it. It is lost only
  when an update creates a missing note (R49). The review's fix is not
  complete, because the insert path writes 0 and the unique index can reject
  a second summary note.
- **1.5e `apply_config` does not publish capabilities or clean mirrors.**
  Wrong. Capabilities do not depend on the configuration rows, and the mirror
  has no remove operation, also for local deletes.
- **1.7b Double JSON escaping pushes rows past 2 MiB.** Wrong. D1 stores the
  decoded string, which stays at 1,900,000 bytes or less.
- **1.7d, 1.7e Unchecked account ID, database ID and machine name.** True, but
  these values come only from your own Settings and `app.toml`. No fix is
  needed.
- **5.2.2 A full-vault scan on each session open.** Wrong. The validation
  query is filtered by session. The double open is real but small.
- **5.2.6 File and mirror I/O on the runtime thread.** Overstated. File attach
  reads at most 256 KiB. The mirror write is skipped while a reply streams.
  Provider streaming continues during a stall; only delivery to the screen
  waits.
- **5.4 The D1 and R2 part of the `lifecycle_mutex` claim.** True, but these
  are actions that you start and wait for. Bridge tasks run on one worker
  thread anyway. Releasing the lock needs new checks for a vault switch. Low
  benefit.
- **6.1 Base64 pictures in `Workspace::source_files_`.** Overstated.
  `source_files()` serves the character pictures, so the copy is not unused.
  Avatars are small. Not worth a change.
- **6.3b `.uploader` changes after an OS reinstall.** Wrong for macOS, where
  `gethostuuid` gives the hardware UUID. True for Windows and Linux, and the
  error message tells you which object to delete.
- **7.2 `host_config_read` is not redacted.** True as a fact, wrong as a
  flaw. `host_config_write` needs the exact text from a fresh read. Redaction
  would make every write fail. These files hold paths, not secrets.
- **7.4 SSRF in `read_note_picture` and `web_read`.** Overstated.
  `read_note_picture` runs only from the desktop bridge after your own paste
  or import. `web_read` sends the URL to Firecrawl or to your own reader; it
  does not fetch the page itself. The impact on a single-user desktop is
  negligible.
- **8.4 Remove the `history_epoch` filters.** The fix is wrong. Vaults cleared
  by older builds still have rows with old epochs, and the filter hides them
  (`docs/tutorial.md:245`). Without the filter, cleared history comes back.
- **8.7 Application-to-NotesService forwarding methods.** True (about 41
  one-line methods), but cosmetic. The refactor touches bridge dispatch,
  tests and ChaWeb. Low value.
- **8.8 Stale `provider_id` and `model_locked`.** True, cosmetic. The locked
  forum's provider wins, and the UI disables the picker.
- **8.9 Streaming rough edges.** Mostly true, low impact. The
  `body["include"]` overwrite cannot lose data today, because `file_search`
  adds its include after all function tools.

## 5. Proposed fixes that conflict with AGENTS.md or your decisions

Do not take these remediation items from the carbon review:

- Record IDs and per-sender `last_applied_seq` in D1 (goes against R9, R23).
- An immutable `sync_id` for notes (goes against R54).
- Removing the `DROP TABLE` eviction (goes against R46).
- An asynchronous first sync pass (changes R37; your decision only).
- A C++ Markdown-to-ProseMirror converter for summaries.
- A separate `<vault>.audio.sqlite3` file.
- R2 metadata in `x-amz-meta-*` headers.
- A `character_pictures` table.
- A thread pool for `cha-daemon` (ChaWeb is on hold).
- Redacting `host_config_read` (breaks `host_config_write`).
- Returning false from `provider_supports_reasoning_effort` for Google and
  Anthropic (breaks Gemini summaries).
- Removing the `history_epoch` filters without a migration.
- Letting `/detach` remove notes and files (new behavior, not cleanup).
- Keeping a session alive after a failed journal write (needs reconciliation
  logic).

## 6. Suggested order

1. P1.1: add the `forums` row when a forum is created in Settings.
2. P2.3: give note requests a finite time limit.
3. Decide on P2.4 (Welcome web tools) and P2.5 (`/cover`). Each fix is small.
4. Decide on P2.1 and P2.2 (the D1 stop message and vault rename). Both need
   UI approval for new texts.
5. Do the P3 items as cleanups when you work in those areas. Ask for ChaWeb
   permission where a change touches shared code (P3.2, P3.8).

## 7. Method and limits

- I checked each claim against the code at `11ca9039`. Four parallel readers
  traced the claims by area (D1 sync; providers and Welcome; notes and
  storage; runtime, R2 and documents). I checked the main items again myself:
  P1.1, P2.1, P2.2, P2.3, P2.4, P2.5, 5.3, 6.4, 8.4 and the R2 download
  routes.
- I did not build, run tests, or start the application.
- I did not measure the SQLCipher key derivation time. The review's figure of
  50 to 150 ms is an estimate.
- I did not test D1 or R2 behavior against Cloudflare.
