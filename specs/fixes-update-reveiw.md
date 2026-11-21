# Post-Fix Architecture Audit & Remediation Plan

**Audited commit:** `79255e0e` (`main`) · **Date:** November 19, 2026
**Baseline documents:** [carbon-arch-review.md](carbon-arch-review.md), [fix-plan.md](fix-plan.md), [p0-d1-design.md](../specs/p0-d1-design.md), [fix-block-1.md](../specs/fix-block-1.md)–[fix-block-5.md](../specs/fix-block-5.md)

---

## Executive Summary

The major work selected after the Carbon, Opus, and Astra architecture reviews has landed cleanly on `main`:

- **Shared-vault multi-process mode removed (`220da282`):** `secondary` mode, `.cha-main-lock`, `.cha-sessions`, and dynamic role re-election are gone; `SessionLease` holds a single exclusive `.cha-lock` for the open vault's lifetime.
- **Desktop D1 sync reworked (`P0-D1`, `F5`, `F6`):** `q2/<vault>/<machine>` tables carry `(op_id, piece, pieces)` framing, `sync_seen(op_id)` receipts make replay idempotent, `full_database` pauses sending without `DROP TABLE` eviction on Desktop, `Settings → Events` surfaces sync stops/skips/pauses, Settings forum creation writes relational `forums` rows transactionally, and Desktop R2 replacement checks `sync_seen` coverage before installation.
- **Non-blocking `Summarize` & persistent L3 sections (`42a60bbf`–`11ca9039`, `F1`):** `NoteSummaryDialog` closes immediately when a batch starts, standalone `compress_note` / `compression_mutex` are gone, per-note L3 sections persist at `level = 3`, and summary generation uses a finite `7200 s` timeout with TCP keepalive. The separate `test_flex` probe uses `900 s`.
- **Targeted runtime, storage, security, and cleanup fixes (`F3`–`F19`):** `Redactor` includes `d1->api_token`, provider credential guards check `base_path`, workspace configuration is validated before vault switch and Desktop R2 install, Jev starts first replies without waiting on session naming, `make_snapshot()` reuses `SessionJournal`'s keyed SQLCipher handle, `note_list`/`note_get` use read transactions, `note_save` skips unchanged annotation rebuilds, `note_import_folder` copies audio outside the tree transaction, OpenAI Chat Completions uses `max_completion_tokens`, `##folder`/`/detach` and `web_reader_api_key` are removed, and deleted session `provider_id` overrides are cleared on load.

### What still needs work

The first implementation priority is U6. On a vault switch, an incoming chat for the destination can reach the previous vault's Markdown mirror. U1, U3, U4, U5, U7b, U7c and U8 also have concrete code changes. U2 still needs provider evidence. The U7a repair is deferred. [Implementation steps and acceptance tests](../specs/fixes-update-plan.md)

### Confirmed scope decisions

| Item | Decision |
| --- | --- |
| U5 | Keep same-origin Jev key reuse for note audio. Match the endpoint path for Jev and voice input. |
| U6 | Limit the UI refresh to windows recovering a live session. Mirror isolation and forum mapping updates remain in scope. |
| U7a | Defer repair of existing folder summaries misclassified by older receivers. The v2 receiver already restores the flag when recreating a missing row. |

**Implementation permissions.** Get explicit ChaWeb permission before changing its behavior, changing a shared path it runs, or running its tests. Show the Provider Settings form and obtain UI approval before exposing the effort selector in U1. Apply the same visual approval rule to any other proposed UI change. Unrelated backend work may proceed when implementation is requested and its own permissions are satisfied. D1–D3 below are separate optional changes. [Project rules](../AGENTS.md)

## Part 1 — Findings and scope

### U1. Provider effort for Google and Anthropic

The encoders use effort for supported models. The provider loader and Settings Save/Test parser still discard it, the form hides the selector, and the compiled Welcome instructions say effort has no effect. The provider validator and character-settings writer also skip effort validation for these APIs. Fix all six paths so a character with no override inherits its provider's value. [Loader](../src/workspace/workspace.cpp#L288) · [Save/Test parser](../src/runtime/request_parser.cpp#L419) · [Provider validation](../src/characters/character_config.cpp#L219) · [Character settings](../src/workspace/workspace_config_editor.cpp#L1175) · [Form](../webapp/src/components/ProviderSettings.tsx#L305) · [Instructions](../resources/assistant-maintenance.md#L124)

Old Google and Anthropic TOML may contain an unknown string or a value of the wrong type for this formerly ignored field. The strict TOML accessor throws on a wrong type, and the loader can then omit the provider. Warn and use the default when loading those old values. Validate new Settings input strictly. Both provider unit suites currently accept `"obsolete"` in direct validation and need revised assertions. [TOML accessor](../src/workspace/workspace.cpp#L106) · [Loader error handling](../src/workspace/workspace.cpp#L1417) · [Google test](../tests/providers/unit_google_api.cpp#L51) · [Anthropic test](../tests/providers/unit_anthropic_api.cpp#L64)

### U2. Gemini 2.5 thinking budget with a short output cap

CHA emits an effort-based `thinkingBudget` independently of `maxOutputTokens`. Flex Test sets an output cap of 1,024. This establishes the request shape. The audit has no Gemini rejection or failing Flex result for this combination. Omitting `thinkingConfig` selects dynamic thinking on 2.5 Pro and Flash, and defaults to thinking off on Flash-Lite. A blanket cutoff would therefore change an explicit effort selection, including in ordinary requests with a manual cap. [Gemini encoder](../src/providers/google_api.cpp#L53) · [Flex Test](../src/notes/notes_operations.cpp#L445) · [Google's budget ranges and defaults](https://ai.google.dev/gemini-api/docs/generate-content/thinking)

**Status.** Verify the affected model and actual outcome before proposing a change. Distinguish an invalid request, a valid response truncated at the token limit, and a successful Flex probe. A mock assertion about the JSON alone cannot settle this. Keep the existing encoder mapping pending evidence. If a confirmed provider limit forces a choice between an explicit effort and a manual output cap, ask which setting should take precedence.

### U3. Disable D1 sync in cha-daemon

Desktop has moved to v2. The daemon still supplies a sync machine name, records outgoing changes, starts the legacy sender, and can evict another machine's queue on a capacity error. Welcome also clears a D1 inbox during remote open or download. The approved daemon design calls for treating the vault as disconnected and logging one warning per open when it contains D1 credentials. [Host wiring](../src/app/application.cpp#L353) · [Eviction](../src/app/d1_sync.cpp#L299) · [Welcome inbox clear](../src/app/vault_operations.cpp#L336) · [Approved design](../specs/p0-d1-design.md#L149)

Use the existing empty-machine gate and remove the v1 sending mode when its last production caller goes away. Desktop still drains its legacy inbox before receiving v2. Retain the Welcome shutdown test's saved-but-unopened assertions when changing how the test triggers shutdown. Before upgrading a deployed daemon, the operator retires its machine's old D1 queue tables as specified in §6. Otherwise peers keep sending work that the daemon will never receive. [Upgrade behavior](../specs/p0-d1-design.md#L142) · [Operator step](../specs/p0-d1-design.md#L153) · [Welcome test](../tests/app/unit_vault_maintenance.cpp#L574)

### U4. Background provider requests can read another vault's key

Flex Test, Settings Test and page formatting can construct a client after releasing the lifecycle lock. A vault switch can then resolve the earlier request's key ID against the new vault. Page import also waits for a network fetch. Even within one vault, resolving a legacy key name after the fetch can select a replacement key with the same name. A successful legacy-name lookup already works today. [Flex and page import](../src/notes/notes_operations.cpp#L445) · [Page formatting](../src/notes/notes_operations.cpp#L1041) · [Settings Test](../src/app/application.cpp#L1328) · [Client key lookup](../src/providers/provider_client.cpp#L499)

Resolve credentials under admission for the original vault. Use the page import's original workspace snapshot to identify the legacy key, and recheck the epoch before constructing the formatting client. Pages that need no formatting must still import with no compression provider. Keep network I/O outside the lock and preserve errors returned through `OperationReply`, including Flex client construction failures. Verify a switch and a same-name replacement during pending work. [Captured snapshot](../src/notes/notes_operations.cpp#L1009) · [Existing reply handling](../src/notes/notes_operations.cpp#L463)

### U5. Saved-key destination checks for Jev, voice input and note audio

The Welcome guard includes a model provider's base path, but compares Jev, voice input and note audio by URL origin. Jev and voice input can use path-specific endpoints. Note audio accepts an origin, and an existing test explicitly permits using a Jev key for note audio at that same origin. The compiled Assistant instructions also omit the provider base-path check. [Guard](../src/workspace/workspace_config_store.cpp#L1295) · [Audio origin validation](../src/media/note_audio.cpp#L109) · [Existing cross-service test](../tests/workspace/unit_workspace_config_store.cpp#L3681) · [Assistant instructions](../resources/assistant-maintenance.md#L740)

Keep the saved Jev key available to note audio at the same origin. A bare audio origin and its `/` spelling both qualify. For a Jev or voice-input destination, match the endpoint path as sent, including a trailing slash on a non-root path. Document these rules in the compiled Assistant guidance.

### U6. Vault ownership and runtime updates of the session mirror

**Vault switch.** The new vault's initial D1 pass runs before the mirror is rebuilt for that vault. `run_once(false)` suppresses note notifications but still delivers changed chats. With a forum ID shared by both vaults, this can already put the destination transcript under the old root. Registering new forums during the pass would extend the problem to newly seen IDs. Select the destination mirror, or disable it for a protected vault, before the worker receives. Test a real switch with distinct roots and inspect both. [Switch order](../src/app/vault_operations.cpp#L1033) · [Initial pass](../src/app/application.cpp#L435) · [Notification behavior](../src/app/d1_sync.cpp#L490) · [Protected vaults](../src/app/application_config.cpp#L759)

**Forum changes.** The mirror registers forums at rebuild and silently skips writes for unknown forums. Settings, Welcome apply/undo, D1 `config.edit` and configuration import can change the forum set. Settings can reuse a deleted forum ID. Refresh the directory and session mappings for the affected forums after commit and before their next mirror write. Keep existing paths for surviving forums and leave archived Markdown files on disk. [Mirror](../src/session/session_mirror.cpp#L101) · [ID allocation](../src/workspace/workspace_config_store.cpp#L2433) · [Import](../src/app/vault_operations.cpp#L1354)

**UI recovery.** On a live session's `reloading` snapshot, recovery can finish without refreshing the sidebar, rosters, personas or R2 capability. Limit the bootstrap refresh to windows recovering a live session. Refresh when the current reopen attempt settles, including when its target was deleted. [Recovery](../webapp/src/useLiveSession.ts#L376) · [Bootstrap refresh](../webapp/src/components/App.tsx#L737)

### U7. Folder summary compatibility, vault rename and upload checks

- **U7a — Deferred compatibility repair.** The v2 receiver restores `folder_summary` when recreating a missing note. Older receivers could have recreated a folder summary as an ordinary note, and later v2 updates to that existing row leave the stale flag in place. Repair of those existing rows is deferred. [Approved requirement](../specs/p0-d1-design.md#L107) · [Creation](../src/storage/note_storage.cpp#L1753) · [Receiver](../src/storage/note_storage.cpp#L2581)
- **U7b — Active D1 vault rename.** The current refusal comes after sessions have drained. Checking under a previous lock acquisition still leaves time for credentials to change. Check in `maintain_database`'s `before_drain` callback under the lock that also starts the drain. Keep the current case-only rename behavior, inactive-vault checks and `std::invalid_argument` error. [Rename](../src/app/vault_operations.cpp#L738) · [Maintenance lock](../src/app/vault_operations.cpp#L550)
- **U7c — R2 upload check.** A remote ETag read can span a local upload. Either the first or the second local ETag can disagree with a remote observation from another instant. Use the existing upload-in-progress flag at admission and the epoch check after the network call. Upload preparation advances the epoch. A check overlapping a transfer should return the existing busy or stale-context result. [Upload state](../src/app/vault_operations.cpp#L1152) · [Epoch publication](../src/app/vault_operations.cpp#L376) · [ETag check](../src/app/vault_operations.cpp#L1243)

### U8. Welcome instructions, attachment references and redaction

Remove the obsolete `web_reader_api_key` and attached-folder guidance from the compiled prompt. `assistant_inspect` currently emits `ref_id` for every item. Folders cannot be attached, while both notes and annotations support **Copy Ref ID** and are accepted by the chat reader. Omit the field for folders only and test all three kinds. [Prompt](../resources/assistant-maintenance.md#L75) · [Inspection](../src/app/assistant_service.cpp#L610) · [Chat reader](../src/storage/note_storage.cpp#L1569)

The current redactor collects saved keys, the D1 token, OAuth secrets and private workspace roots. Extend the same collection and replacement logic to the active nonempty vault password and configured external vault, mirror, modify and log roots. The separate host-file read/write contract still requires exact file contents. [Redactor](../src/app/assistant_service.cpp#L180) · [Runtime paths](../src/app/application.cpp#L267) · [Prior decision](../specs/fix-block-1.md#L85)

## Part 2 — Optional product and documentation decisions

### D1. Meaning of explicit None

The Gemini and Anthropic encoders omit thinking settings for `none`. The resulting default depends on the model. Keep this meaning for U1. A distinct “minimize or disable thinking” choice would require a product decision and model-specific support. [Gemini encoder](../src/providers/google_api.cpp#L53) · [Anthropic encoder](../src/providers/anthropic_api.cpp#L52)

### D2. Stop a running Summarize batch

The dialog closes on start and the batch holds the summary slot until it finishes or is cancelled. The frontend stores only `{ source, epoch }` in `summaryBatch.current`. The `AbortController` belongs to the wider Notes context. A batch-specific Stop action would need to stop scheduling further steps for that batch and call the existing `finish_summary_batch` for its connection and epoch, while keeping unrelated Notes operations running. This requires a product decision and visual approval for the control. [Batch state](../webapp/src/components/App.tsx#L782) · [Shared signal](../webapp/src/components/App.tsx#L1141) · [Backend cancellation](../src/notes/notes_operations.cpp#L806)

### D3. Phase 5 Documentation Sync (`docs/*.md` — Carbon Flaw 9 + post-fix updates)
Per `AGENTS.md`, `docs/` was intentionally left untouched during Fix Blocks 1–5 ([fix-plan.md](../docs/fix-plan.md#L501-L528)). Note that **one** item from Carbon Flaw 9 is no longer drift: single-process `.cha-lock` in [MaintainerGuide.md](MaintainerGuide.md#L1496) and [tutorial.md](tutorial.md#L526) is accurate again after commit `220da282`.

When the user explicitly authorizes updating `docs/`, update:
1. **[docs/README.md](README.md):** Add `AdminManual.md` / `AdminManual.html`; describe the supported Welcome `/open` and `/download` commands in place of the obsolete R2 transfer routes.
2. **[docs/UserManual.md](UserManual.md):** Remove `##folder` attachment and `/detach`; document transient in-memory `/cover` scope; update Note `Summarize` (non-blocking background run, persistent per-note L3, Events reporting); update Google/Anthropic `reasoning_effort` behavior (`U1`).
3. **[docs/AdminManual.md](AdminManual.md):** Remove `web_reader_api_key`; update D1 sync section (`q2/` queues, pause-on-full instead of queue eviction, `Settings → Events` diagnostics, disconnecting D1 before renaming a vault, and `cha-daemon` not running D1 sync once `U3` lands); update Google/Anthropic `reasoning_effort`.
4. **[docs/MaintainerGuide.md](MaintainerGuide.md):** Fix ElevenLabs voice output section (`lines 1043, 1062`); update Google/Anthropic `reasoning_effort` (`lines 780-785`); add concise sections for D1 sync v2 (`q2/`, `sync_seen`), Notes / `NotesService`, Forum Library (`FolderStoreClient`), `web_reader`, and `AssistantService`.
5. **[docs/tutorial.md](tutorial.md):** Update repository/target map (`src/notes/`, `src/daemon/`, `google_api`, `anthropic_api`, `d1_sync`), `/clear` support (`lines 245-247`), and tool-call loop limits (`4` vs `16` vs Welcome limit).
6. **[docs/chaweb.md](chaweb.md):** Remove deleted `/vault/*` R2 transfer routes and `DELETE .../audio`; add read-only `GET /api/cha/v1/notes` & `GET /api/cha/v1/notes/{id}`, Welcome `/open` & `/download` commands, `daemon_token`/`context_epoch` query parameters, ElevenLabs TTS, and D1 disabled status (`U3`). Regenerate `.html` manuals if tracked.

---

## Part 3 — Implementation order

First correct mirror ownership during vault switch. The plan then groups the remaining work by provider, sync and storage tests. U2 starts with evidence gathering. The vault-operation fixes in U7b and U7c can proceed independently of the deferred U7a repair. [Implementation and verification](../specs/fixes-update-plan.md)

---

## Part 4 — Complete Status Matrix vs. [carbon-arch-review.md](carbon-arch-review.md)

| Carbon Review Section | Status on `HEAD` (`79255e0e`) | Remaining Action / Intentional Scope Decision |
| :--- | :--- | :--- |
| **1.1** Interleaved D1 pieces & `seq` reuse | **Fixed on Desktop** (`q2/` `(op_id, piece, pieces)` + head reads) | **`U3`** turns off legacy `q/` D1 worker in `cha-daemon`. Perpetual legacy `q/` `missing_table` poll on Desktop is **by design** ([p0-d1-design.md](../specs/p0-d1-design.md#L145) §5) until old tables are retired. |
| **1.2** Auto-eviction & silent `missing_table` stop | **Fixed on Desktop** (no `DROP TABLE` eviction; `Settings → Events` + `F6` upload text) | **`U3`** removes the remaining `cha-daemon` `handle_full_database()` eviction path. |
| **1.3** Settings `create_forum` missing `forums` row | **Fixed** (`ensure_forum_rows` in commit transaction & at open) | **`U6`** addresses mirror ownership and forum changes. Its UI refresh applies only to windows recovering a live session. |
| **1.4** Path-addressed notes & `folder_summary` | **Approved v2 missing-row fix implemented** | **`U7a`** repair of existing rows misclassified by older receivers is deferred. Mutable path identity (`1.4a`) **intentionally kept** under single-active-machine rule ([p0-d1-design.md](../specs/p0-d1-design.md#L106)). |
| **1.5** Non-idempotent replay & error skipping | **Fixed** (`sync_seen` receipts + `SqliteError` transient retry + skip Events) | Concurrent positional `chat.delete_turn` shift & rejected `config.edit` delta divergence **accepted** ([p0-d1-design.md](../specs/p0-d1-design.md#L191-L197)). |
| **1.6** R2 replace vs D1, vault rename, vault copy | **Fixed** (`sync_seen` coverage check on replace; `F5` rename refusal) | **`U7b`** moves active-vault rename refusal before session drain; **`U7c`** fixes `check_database_upload` `r2_etag` TOCTOU. "Add from R2" requiring `"Save D1 credentials"` & `copy_from` keeping D1 key **intentionally kept** ([fix-block-4.md](../specs/fix-block-4.md#L161-L163)). |
| **1.7** `note.update` 5-blob payload, JSON escaping, D1 IDs | **Unchanged** | **Intentionally unchanged** ([fix-plan.md](fix-plan.md#L539)). |
| **2.1–2.4** Multi-process `main`/`secondary` vault locking | **Fixed** (`220da282` deleted `secondary` mode; exclusive `.cha-lock`) | None. |
| **3.1–3.3** `timeout_s = 0`, `compression_mutex`, OpenAI `max_tokens` | **Fixed** (`F1` `timeout_s = 7200`, `54b35bdd` mutex removal, `F16` `max_completion_tokens`) | **`U2`** requires evidence of a Gemini 2.5 failure with a short output cap. **`U4`** fences `ApiKeyStore` under `lifecycle_mutex`. `7200 s` non-Flex timeout is **by explicit user decision** ([fix-block-1.md](../specs/fix-block-1.md#L31-L32)). |
| **4.1** Blocking `Summarize` modal & volatile L3 sections | **Fixed** (`42a60bbf`–`11ca9039` background dialog close + persistent `level = 3`) | **`D2`** (optional UI Stop button). Browser-driven two-phase RPC orchestration **intentionally kept** under KISS ([fix-plan.md](fix-plan.md#L545)). |
| **4.2** `epubImport.ts` repeated `unzipSync` & `16 MiB` cap | **Unchanged** | **Intentionally unchanged** ([fix-block-3.md](../specs/fix-block-3.md#L110)). |
| **5.1** Synchronous D1 pass on vault open | **Unchanged** | **Intentionally unchanged** (R37, [fix-plan.md](fix-plan.md#L541)). |
| **5.2** SQLCipher PBKDF2, `BEGIN IMMEDIATE`, annotations, import, runtime I/O | **Selected hotspots fixed** (`F11`–`F14`) | `synchronous = FULL`, cold-open validation, fail-closed `LiveSession` on 5s `SQLITE_BUSY`, and runtime file/mirror I/O **intentionally kept** ([fix-block-3.md](../specs/fix-block-3.md#L103-L108)). |
| **5.3** Jev 10s naming stall on first message | **Fixed** (`F10`) | None. |
| **5.4** `lifecycle_mutex` network holds, `cha-daemon` SCGI / voice spin | **Unchanged** | **`U4`** fixes unguarded background `ApiKeyStore` reads; `cha-daemon` SCGI / `serve_voice_input` **deferred under ChaWeb hold** ([fix-plan.md](fix-plan.md#L566-L567)). |
| **6.1–6.4** Base64 avatars, audio blobs, R2 3-object layout, `app.unusable` | **Selected items fixed** (`F7` switch precheck, `F8` staged R2 precheck, `F9` `.toml` fallback) | Base64 avatars, in-DB audio + upload `VACUUM`, `.uploader` OS-ID, protected Add-from-R2, and single `.bac` **intentionally kept** ([fix-block-4.md](../specs/fix-block-4.md#L164-L166)). |
| **7.1–7.4** `Redactor`, `host_config_read`, Welcome tools/guard, SSRF | **Selected items fixed** (`F3` D1 token redaction, `F4` provider `base_path`) | **`U5`** adds path checks for Jev and voice input and preserves same-origin Jev key reuse for note audio. **`U8`** extends redaction to external roots and the vault password. Unredacted `host_config_read`, Welcome `web_search`/`web_read`, and LAN/loopback URL rules **intentionally kept** ([fix-block-1.md](../specs/fix-block-1.md#L85-L89)). |
| **8.1–8.9** Dead/no-op features & protocol rough edges | **Selected items fixed** (`F15` encoders, `F17` provider reset, `F18` `##folder`/`/detach` removal, `F19` `web_reader_api_key` removal) | **`U1`** completes provider-level `F15`; **`U8`** cleans residual prompt/inspect strings; **`D1`** covers `"none"` effort semantics. Ephemeral `/cover`, `history_epoch`, `"gemini-3.8-flash"`, store list/delete, `NotesService` shims, `model_locked`, and SSE/multicast items **intentionally kept** ([fix-block-2.md](../specs/fix-block-2.md#L112-L119), [fix-block-5.md](../specs/fix-block-5.md#L106)). |
| **9** `docs/*.md` documentation drift | `.cha-lock` drift resolved by `220da282`; rest **deferred** | **`D3`** lists all `docs/*.md` updates to apply when explicitly requested (`AGENTS.md`). |
