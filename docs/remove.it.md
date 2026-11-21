# CHA Feature & Functionality Removal Report

## Executive Summary

At its core, **CHA** is a personal, single-user desktop application for multi-character AI conversations in customizable forums, paired with voice interaction and reference notes.

Because v1 served as an experimental testbed, the repository has grown to **~111,000 lines of production code** (`64.7k` C++, `36.0k` TypeScript/CSS, `10.5k` host/packaging/resources) and **~112,000 lines of tests** (**~223,000+ lines total**), spanning **15 Settings screens**, **17 SQLite tables**, **3 operating systems**, **2 frontend clients**, and **more than a dozen external cloud/API integrations**. Along the way, CHA accumulated an e-book publishing pipeline, an audiobook batch transcoder, two competing custom cloud sync/backup engines, a cloud RAG admin console, a Linux multi-user SCGI server, an AI agent that edits the app's own config files, a custom Russian input method, and hardcoded USD pricing tables that only write to log files.

This report catalogs the features, subsystems, duplicate integrations, and dead weight that should be removed (or collapsed) to bring CHA back in line with `AGENTS.md` (*"a personal, toy-like application, not an enterprise-grade program"*).

---

## My List

1. Export EPUB
2. Pictures panel


## Summary Matrix

| # | Feature / Subsystem | Area | Removal Priority | Primary Reason to Remove |
| :--- | :--- | :--- | :--- | :--- |
| **1.1** | **Export to EPUB & Import EPUB** | Notes | **Prime Cut** | Turns a chat app into an e-book publisher/reader; Calibre/Pandoc already do this; drops `fflate`. |
| **1.2** | **Note Audiobook Batch Service & OS Codecs** | Notes / Media | **Prime Cut** | Third TTS stack; hardcoded to Gemini Batch API; links Apple AudioToolbox & Win32 MediaFoundation; bloats SQLite DB. |
| **1.3** | **5-Version Note Summarization (`L1`–`L3`, `Summary`, `Flex`)** | Notes | **Prime Cut** | 5 versions per note (`L1`–`L3` unused by chat); fragile C++↔JS ProseMirror loop; stale copies on edit; 2-hour Flex timeouts. |
| **1.4** | **YouTube & Long-Paragraph LLM Page Formatting** | Notes | **Prime Cut** | Hidden LLM call during URL import; Outlook SafeLinks unwrapping; fails page import when compression provider is unset. |
| **1.5** | **Notes Prompt Bar, `ANNOTATIONS`, Comments, `Copy JSON`, Folder Archive** | Notes | **High** | Redundant input bar under a WYSIWYG editor; quirky comment UX; O(folder) autosave rebuilds; leaks TipTap AST JSON. |
| **2.1** | **Cloudflare D1 Multi-Machine Sync** | Storage / Sync | **Prime Cut** | Custom distributed queue engine; #1 source of data-loss bugs and architectural complexity in a single-user app. |
| **2.2** | **Cloudflare R2 Whole-Vault Storage** | Storage / Sync | **High** | Custom S3 SigV4 client, `.uploader` machine locks, ETag races, audio stripping + `VACUUM` for a local SQLite file. |
| **2.3** | **Continuous Session Markdown Mirror (`mirror`)** | Storage | **High** | Write-only `.md` projection on runtime thread; leaves orphaned files; duplicates manual session **Download**. |
| **2.4** | **Config Export/Import (`modify`), Vault Merge & Template DSL** | Workspace | **High** | 4 ways to edit config; dumps secrets in plaintext; custom `$$(...)`/`$${...}` DSL for settings with no UI. |
| **2.5** | **Multi-Vault System & Irreversible SQLCipher Encryption** | Vaults | **Medium–High** | Source-built SQLCipher amalgamation, PBKDF2 snapshot overhead, unchangeable passwords on an OS with FileVault. |
| **3.1** | **ChaWeb Browser Client & `cha-daemon` Linux Server** | Platform | **Prime Cut** *(On Hold)* | Already frozen; unauthenticated multi-user server; imposes a heavy shared-code tax on desktop changes. |
| **3.2** | **Windows Native Port (`packaging/windows/`)** | Platform | **Conditional** | 2,055-line Win32/WebView2 host + COM/ACL/MSVC build matrix; cut if CHA is only used on macOS. |
| **4.1** | **Welcome Assistant Self-Modifying Config/Log Agent** | App / AI | **High** | ~2,600+ C++ LOC + embedded manuals to edit settings that already have GUI forms; prompt-injection & redaction surface. |
| **4.2** | **Cloud "Library" / Remote RAG Vector Stores** | Providers / Notes | **High** | 4th way to pass context; ~3,000+ LOC; non-updating cloud copies; global file-deletion footgun; locks chat model picker. |
| **4.3** | **Jev External Recipient & Current-Note Classifier** | Runtime / AI | **High** | External alpha API on every message; 5–10s turn latency; solves what `@Name` and an "Attach open note" button do simply. |
| **4.4** | **Cross-Session Summaries (`Save summary` & `summary_*` tools)** | Session / Notes | **Medium–High** | Couples forums to managed Notes folders (cascade-deleting notes with forums); adds 2 tools + cache to every chat. |
| **5.1** | **xAI Native WebSocket Dictation (Duplicate STT)** | Voice | **High** | Second STT stack (~2,000 LOC C++ WebSocket + AudioWorklet + fake server) alongside OpenAI WebRTC. |
| **5.2** | **Dual TTS Providers & Live WebSocket/MSE Audio Streaming** | Voice | **Medium–High** | FishAudio *and* ElevenLabs, HTTP *and* mid-reply WebSocket streaming + MP4 wrapping + 2.5s silence synthesis. |
| **5.3** | **Redundant Search/Reader Backends (`web_reader`, Brave+Tavily)** | Providers | **Medium** | DIY undocumented `web_reader` microservice + dual search vendors alongside provider-native search. |
| **5.4** | **ChatGPT Subscription Codex OAuth (`openai_oauth.cpp`)** | Auth | **Medium–High** | ~1,150 LOC device-code OAuth against internal ChatGPT Codex endpoint (`chatgpt.com/backend-api/codex`); API keys suffice. |
| **6.1** | **Hardcoded USD Token & Voice Cost Estimators** | Providers / Logs | **Prime Cut** | Zero UI (log-only); ~85 hardcoded model rates + hardcoded Dec 2026 / Jan 2027 promo expiry dates; pollutes wire DTOs. |
| **6.2** | **Built-in Latin-to-Russian Transliteration (`A→Я`)** | UI | **Prime Cut** | Custom IME wrapper across inputs (`shh → щ`, `+` escapes) duplicating native macOS/Windows phonetic keyboards. |
| **6.3** | **Custom Typography Styles (`Settings → Styles`)** | UI / Config | **High** | Full CRUD entity, TOML files, and reference-count checks just to toggle 5 CSS font/color classes. |
| **6.4** | **Random character variants** | Workspace | **Removed** | Numbered files are ordinary Markdown. |
| **6.5** | **Editable System Task Prompts (`Settings → Prompts`)** | Settings | **High** | Exposes 9 internal prompts with no "Reset to default" button and fragile heading rules that break callers if edited. |
| **6.6** | **Character Picture Side Panel (`View → Pictures`)** | UI / Storage | **Medium** | Base64-encodes up to 8 MB avatars inside SQLite `config` rows (copied in RAM on config edits) to cycle portraits. |
| **6.7** | **Transient `/cover` & Background `Events` Screen** | Chat / Settings | **Medium** | `Cover` silently resets when switching chats; `Events` table exists only to compensate for headless background jobs. |
| **6.8** | **`libuv` Dependency & Legacy v1 Migration Shims** | Build / Storage | **Prime Cut** | Fetches & compiles all of `libuv` for **one** `uv_fs_mkstemp` call; carries dead schema-v1 and obsolete TOML shims. |

---

## 1. Notes "E-Book & Publishing Suite" Bloat

CHA's Notes feature expanded from a place to keep reference material for chats into a miniature Calibre + Readwise + Audible pipeline. Most of that machinery does not help conversations at all.

### 1.1 Export to EPUB (`Export EPUB…`) and Import EPUB (`Import EPUB…`)
- **Code & docs:** [webapp/src/notes/epubExport.ts](../webapp/src/notes/epubExport.ts), [webapp/src/notes/epubImport.ts](../webapp/src/notes/epubImport.ts), [webapp/src/download.ts](../webapp/src/download.ts), [webapp/src/components/NotesTree.tsx](../webapp/src/components/NotesTree.tsx), `fflate` in [webapp/package.json](../webapp/package.json), [specs/export-epub-design.md](../specs/export-epub-design.md), [UserManual.md §11](UserManual.md#import-a-book).
- **Why remove:**
  - **Outside the product's purpose:** Packaging notes into reflowable EPUB 3 archives (building OPF 3.0 manifests, Dublin Core metadata, `nav.xhtml` spines, XML 1.0 control-character scrubbing, data-URI image extraction, ZIP32 size guards, and ` (L1)`/` (L2)` book-title suffixes) turns a chat companion into an e-book authoring tool. Dedicated tools like Pandoc and Calibre already do this far better.
  - **Symmetric bloat on import:** `epubImport.ts` (374 lines) implements custom XML DTD/named-entity recovery, UTF-16 BOM detection, NCX/EPUB3 TOC resolution, chapter-title heuristics, repeated synchronous `unzipSync` archive scans on the UI thread, and a 16 MiB bridge request ceiling that silently drops book images once exceeded.
  - **Clean dependency cut:** Removing both EPUB import and export eliminates `fflate` from the frontend bundle, removes WebView Blob-URL retention workarounds, and drops ~1,200+ lines of production and test code.

### 1.2 Note Audiobook Batch Service (`Generate audio`) & Native OS Audio Transcoders
- **Code & docs:** [src/media/note_audio.cpp](../src/media/note_audio.cpp), [src/media/note_audio_codec.cpp](../src/media/note_audio_codec.cpp), [src/notes/note_audio_operations.cpp](../src/notes/note_audio_operations.cpp), [webapp/src/components/NoteAudioDialog.tsx](../webapp/src/components/NoteAudioDialog.tsx), [webapp/src/components/NoteAudioVoicePicker.tsx](../webapp/src/components/NoteAudioVoicePicker.tsx), [webapp/src/notes/useNoteAudio.ts](../webapp/src/notes/useNoteAudio.ts), `note_audio` / `note_audio_attempts` SQLite tables, [AdminManual.md §15](AdminManual.md#note-audio-service).
- **Why remove:**
  - **A third, incompatible TTS stack:** CHA already implements TTS for FishAudio and ElevenLabs. Note audio adds a completely separate Google Gemini Batch API pipeline (`upload/v1beta/files` resumable upload → `batchGenerateContent` → polling `batches/` → downloading multi-megabyte JSONL base64 WAV results), hardcoded to `gemini-3.8-flash-tts` and filtered to English/Russian voices only.
  - **Heavy OS-native codec coupling:** Solely to compress Gemini's 24 kHz PCM16 WAV clips into 48 kbps `.m4a` paragraphs, `cha_lib` links Apple `AudioToolbox` on macOS and initializes multithreaded COM + Windows `MediaFoundation` (`mfplat`, `mfreadwrite`, `mfuuid`, `ole32`) on Windows ([note_audio_codec.cpp](../src/media/note_audio_codec.cpp)).
  - **Persistent background-job recovery & DB bloat:** Stores up to 1,000 M4A blobs per note directly inside `<vault>.sqlite3`, tracks unfinished remote batches across app restarts (`note_audio_attempts`, including ambiguous *"Submission status unknown"* states), forces R2 vault uploads to copy the database, delete `note_audio`, and run `VACUUM`, and forces D1 sync to invalidate audio on note edits.
  - **Narrow usability:** Works only on read-only notes, only in `Full text` or `L1` mode, and is unavailable on Linux/ChaWeb.

### 1.3 Five-Version Note Summarization Matrix (`L1`, `L2`, `L3`, Folder `Summary`, & `Flex` Processing)
- **Code & docs:** [webapp/src/notes/summaries.ts](../webapp/src/notes/summaries.ts), [webapp/src/notes/summaryImport.ts](../webapp/src/notes/summaryImport.ts), [webapp/src/components/NoteSummaryDialog.tsx](../webapp/src/components/NoteSummaryDialog.tsx), [src/notes/notes_operations.cpp](../src/notes/notes_operations.cpp), [UserManual.md §11](UserManual.md#versions-of-a-note), [AdminManual.md §15](AdminManual.md#flex-processing-for-summarize).
- **Why remove:**
  - **Massive conceptual & storage sprawl unused by Chat:** Every note can store **five separate documents** (`Full text`, `Compressed`, `L1`, `L2`, `L3`), plus a generated read-only folder `Summary` note (`Summary (2)` collision rules, `Exclude from summaries` flags, global **Note text mode** toolbar state). Yet **chats never use `L1`, `L2`, or `L3` at all**—only `Compressed` or `Full text`!
  - **Split-brain C++ ↔ Browser orchestration:** Because notes are stored as zlib-compressed TipTap/ProseMirror JSON and C++ has no Markdown-to-ProseMirror parser, batch summarization bounces every step from C++ (`summaryGenerate`) → React/TipTap (`markdownNoteDocument`) → back to C++ (`summaryCommit` / `summaryAbandon` / `summaryLine`).
  - **Silent staleness:** Editing a note's `Full text` does **not** update or invalidate `Compressed`, `L1`, `L2`, or `L3`. Attaching an edited note to a chat silently feeds the character the *old* `Compressed` version until the user remembers to re-run **Summarize** with **Overwrite** enabled.
  - **Brittle Flex tier machinery:** Automatically forces OpenAI/Gemini `flex` processing with **no standard-tier fallback** (unsupported models simply fail every summary step), requires a 2-hour (`7,200 s`) per-request timeout, has **no Cancel command**, and adds a dedicated **Test Flex API** UI button with a 15-minute (`900 s`) timeout.
  - **Simpler alternative:** Keep one document per note. If a user wants a summary of a note, they can attach the note to a chat and ask a character to summarize it.

### 1.4 YouTube Transcript & Long-Paragraph LLM Reformatting on Page Import
- **Code & docs:** `is_youtube_url()`, `has_long_page_paragraph()`, `format_note_page()` in [src/notes/notes_operations.cpp](../src/notes/notes_operations.cpp), `resources/youtube-transcript.md`, `resources/page-formatting.md`, [AdminManual.md §13](AdminManual.md#page-import-into-notes).
- **Why remove:**
  - CHA has no YouTube fetcher of its own; it hopes Firecrawl/`web_reader` returns transcript text, includes custom URL unwrapping for Microsoft `safelinks.protection.outlook.com` links, scans imported Markdown with a custom parser for paragraphs over 2,000 characters, and silently runs a blocking LLM rewrite pass using the Notes compression provider.
  - Creates a surprising failure mode: importing a web page fails if no Notes compression provider is configured (or if the model refuses/truncates the page). Page import should just import the extracted Markdown directly.

### 1.5 Notes Editor Feature Creep: Bottom "Notes Prompt", `ANNOTATIONS` Rollup, Quirky Comments, `Copy JSON`, & Folder Archives
- **Code & docs:** [webapp/src/components/NotesScreen.tsx](../webapp/src/components/NotesScreen.tsx), [webapp/src/notes/clipboard.ts](../webapp/src/notes/clipboard.ts), `rebuild_folder_annotations` & `note_export_folder`/`note_import_folder` in [src/storage/note_storage.cpp](../src/storage/note_storage.cpp), [UserManual.md §11](UserManual.md#the-notes-prompt).
- **Why remove:**
  - **The bottom "Notes prompt" bar:** Puts a chat-style message box underneath a WYSIWYG document editor. Pressing Enter inserts a paragraph at the cursor, pasting a URL/path triggers an import, and **Add comment** requires an unintuitive ritual (*"Type the comment in the prompt. Do not send it. Right-click the paragraph… Click Add comment"*). Standard editor typing + explicit menu actions for Import are much clearer.
  - **Auto-generated `ANNOTATIONS` virtual items & inline Comments:** Whenever any note in a folder has a highlight or comment, every 5-second autosave runs `rebuild_folder_annotations()` inside a write transaction—zlib-decompressing and JSON-parsing every sibling note in the folder to rebuild a read-only `ANNOTATIONS` rollup item (which works only for `Full text`, ignoring other modes).
  - **`Copy JSON` in the right-click context menu:** Copies raw internal ProseMirror AST JSON to the clipboard ([clipboard.ts](../webapp/src/notes/clipboard.ts))—a developer debugging aid left in user-facing UI.
  - **Binary/SQLite `Export folder…` / `Import folder…`:** Exists primarily to carry `Compressed`, `L1`, `L2`, `L3`, and `Summary` links between vaults in a custom SQLite container, holding a single large write transaction on import that can stall live chats. Plain Markdown files (or cutting folder transfer) are simpler and standard.

---

## 2. Cloud Sync, Backup & Vault Sprawl

For a single-user personal desktop app, CHA has an extraordinary amount of persistence and replication infrastructure: **multi-vault switching**, **SQLCipher encryption**, **vault merging**, **filesystem TOML export/import**, **continuous Markdown mirroring**, **Cloudflare R2 S3 snapshots**, *and* **Cloudflare D1 event queues**.

### 2.1 Cloudflare D1 Multi-Machine Synchronization (`D1 sync`)
- **Code & docs:** [src/app/d1_sync.cpp](../src/app/d1_sync.cpp), [src/app/d1_client.cpp](../src/app/d1_client.cpp), [src/storage/sync_storage.cpp](../src/storage/sync_storage.cpp), [src/storage/sync_chat.cpp](../src/storage/sync_chat.cpp), [webapp/src/components/D1SyncSettings.tsx](../webapp/src/components/D1SyncSettings.tsx), `sync_queue` & `sync_seen` tables, [AdminManual.md §20](AdminManual.md#20-d1-synchronization).
- **Why remove:**
  - **Bespoke distributed replication in a toy app:** Implements per-vault/per-machine tables (`q2/<vault>/<machine>` plus legacy `q/` polling), 1.9 MB piece chunking, outbox/receipt tables (`sync_queue`, `sync_seen`), 30-second background polling, synchronous startup sync passes, and shutdown flushes across config, chats, and 5-blob notes.
  - **Highest defect density in the repository:** D1 sync and its edge cases (piece interleaving, queue full stops, path-based note collisions, forum creation gaps, vault rename lockouts, R2 upload/download interlocks) dominated multiple architectural reviews and multi-commit fix campaigns.
  - **Operational footguns:** If a second computer isn't opened for a while, its `q2/` inbox accumulates unconsumed chunks until the Cloudflare D1 database fills up and **pauses sync across all machines**. Vaults with D1 enabled cannot even be renamed without disconnecting first, and `cha-daemon` doesn't support D1 at all.

### 2.2 Cloudflare R2 Whole-Vault Storage (`R2 storage`)
- **Code & docs:** [src/app/r2_database_transfer.cpp](../src/app/r2_database_transfer.cpp) (1,361 LOC), [src/app/vault_operations.cpp](../src/app/vault_operations.cpp), [webapp/src/components/R2StorageSettings.tsx](../webapp/src/components/R2StorageSettings.tsx), [AdminManual.md §19](AdminManual.md#19-r2-storage).
- **Why remove (especially if D1 is removed, or if single-machine):**
  - Implements a custom AWS S3 SigV4 HTTP client, non-atomic two-object uploads (`<db>.sqlite3` + `<db>.sqlite3.toml`), immutable `<db>.sqlite3.uploader` hardware-fingerprint lock files (requiring manual Cloudflare dashboard deletion if you switch uploader machines or reinstall Windows), ETag pre-checks, `.bac` rollback files, and temporary DB copying + audio-table `DELETE` + `VACUUM` on every upload.
  - A vault is just a single SQLite file on disk (`~/Library/Application Support/CHA`). Standard OS backup or copying the file (when CHA is closed) accomplishes the same goal with zero C++ cloud transfer code. *(If remote transfer between two personal Macs is genuinely needed, keeping **only** a simplified snapshot upload/download while deleting D1 still removes the worst interlocks.)*

### 2.3 Continuous Session Markdown Mirror (`mirror` in `app.toml`)
- **Code & docs:** [src/session/session_mirror.cpp](../src/session/session_mirror.cpp), [AdminManual.md §24](AdminManual.md#the-session-mirror).
- **Why remove:**
  - Automatically writes every session as a `.md` file into `mirror/<vault>/<forum>/` on the shared `SessionRuntime` thread after every completed turn or rename.
  - **Write-only and leaky:** CHA never reads mirror files back; deleting a session or forum leaves stale files on disk (causing `<forum> (1)` numbered directory collisions later); protected vaults silently skip mirroring; and enabling vault protection leaves earlier plaintext mirrors behind on disk.
  - **Already redundant:** The session menu already has **Download**, which exports any conversation to a clean Markdown file on demand.

### 2.4 Filesystem Config Export/Import (`modify`), Vault Merge, & the Custom `$$(...)` / `$${...}` Template DSL
- **Code & docs:** [src/workspace/workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp) (3,235 LOC), [src/util/text_template.cpp](../src/util/text_template.cpp) (634 LOC), [src/workspace/workspace.cpp](../src/workspace/workspace.cpp), [AdminManual.md §18, §21, §30](AdminManual.md#21-configuration-files-export-edit-import).
- **Why remove:**
  - **Four ways to mutate configuration:** (1) Settings GUI screens, (2) Export/Edit/Import via `modify/<vault>/`, (3) Vault **Merge** (`parent` tracking), and (4) Welcome Assistant tools.
  - **Dual representation tax:** Although SQLite is the runtime source of truth, CHA serializes all settings inside SQLite as virtual `.toml` and `.md` file paths (`system/providers/provider_1/config.toml`, `forums/forum_1/members/character_1/character.toml` with `# Required placeholder`) solely to preserve round-trip compatibility with filesystem Export/Import!
  - **Plaintext secret exposure:** Clicking **Export** dumps all API keys, R2 secrets, and D1 tokens into `modify/<vault>/system/keys/` as unencrypted TOML—even for password-protected vaults—right after wiping `<modify>/<vault>` without confirmation.
  - **Hidden file-only template language:** The custom `$$(...)` file-include and `$${variable}` substitution engine (`text_template.cpp`), `CHARACTER.md` vs. `PROFILE.md` split, `character_defaults.toml`, and per-forum member `[prompt]` variable tables have **no Settings UI** and exist only for hand-edited file imports. Plain Markdown prompts edited directly in the UI are vastly easier to understand and maintain.

### 2.5 Multi-Vault System & Irreversible SQLCipher Password Encryption
- **Code & docs:** `sqlcipher_source` in [CMakeLists.txt](../CMakeLists.txt#L189-L275), [src/app/vault_operations.cpp](../src/app/vault_operations.cpp), [webapp/src/components/VaultSettings.tsx](../webapp/src/components/VaultSettings.tsx), [webapp/src/components/PasswordDialog.tsx](../webapp/src/components/PasswordDialog.tsx), [AdminManual.md §18](AdminManual.md#protect-a-vault-with-a-password).
- **Why consider removing:**
  - **Heavy build & runtime cost for SQLCipher:** CMake fetches SQLCipher 4.19 and shells out to `./configure && make sqlite3.c` (or `nmake` on Windows) at configure time, links OpenSSL Crypto into SQLite, and runs 256,000-iteration PBKDF2 key derivation when opening connections.
  - **Hostile UX constraints:** Once a vault is password-protected, **you can never change or remove the password** (except by cloning to a new vault name); losing the password permanently bricks the vault; new-vault R2 downloads fail on protected vaults; ChaWeb requires a plaintext `password` file on disk; and `Export` writes secrets in plaintext anyway. On macOS, FileVault already encrypts `~/Library/Application Support/CHA` transparently, and API keys could live in macOS Keychain if OS-level secret storage is desired.
  - **Multi-vault complexity:** Supporting runtime vault switching (`context_epoch` invalidation across every subsystem, `.cha-lock` leases, Vault Copy, Vault Merge) adds substantial lifecycle complexity (`vault_operations.cpp` is 1,384 LOC) when one database (or launching with `--config` for testing) covers almost all personal use.

---

## 3. Secondary Clients & Platform Ports

### 3.1 ChaWeb Browser Client, `cha-daemon` SCGI Server & Linux Packaging *(Already on Hold)*
- **Code & docs:** [src/daemon/](../src/daemon/) (`main.cpp`, `scgi.cpp`, `chaweb_adapter.cpp`), [webapp/src/chaweb/](../webapp/src/chaweb/) (~25 files), [packaging/linux/](../packaging/linux/), [scripts/run_daemon.py](../scripts/run_daemon.py), Welcome `/open` & `/download` commands in [src/app/vault_operations.cpp](../src/app/vault_operations.cpp), [docs/chaweb.md](chaweb.md), [AdminManual.md §25](AdminManual.md#25-chaweb).
- **Why remove:**
  - Already placed on indefinite hold in `AGENTS.md`, yet its code, tests (`tests/daemon/`, `webapp/src/chaweb/*.test.*`, `daemon_integration_test.py`), build targets (`cha-daemon`, `build:chaweb`), and shared-code constraints remain active across the repository.
  - Architectural mismatch: deploys a multi-user systemd/nginx SCGI server with **zero authentication**, serial single-request SCGI handling, polling-based snapshots, and chat-input slash commands (`/open <name>`, `/download confirm`) to work around missing UI screens. Removing ChaWeb and `cha-daemon` unlocks major simplifications in `Application`, `SessionRepository`, audio streaming, and voice capture.

### 3.2 Windows Native WebView2 Port (`packaging/windows/`) *(Conditional)*
- **Code & docs:** [packaging/windows/main.cpp](../packaging/windows/main.cpp) (2,055 LOC), `package.ps1`, `make-icon.ps1`, [scripts/build-windows.ps1](../scripts/build-windows.ps1), Win32 branches in [CMakeLists.txt](../CMakeLists.txt), [private_filesystem.cpp](../src/util/private_filesystem.cpp), and [note_audio_codec.cpp](../src/media/note_audio_codec.cpp).
- **Why consider removing:**
  - If CHA is primarily a personal macOS application, maintaining a 2,055-line Win32/WebView2 C++ host, NuGet SDK fetching, static OpenSSL CRT matching, Windows ACL security descriptors, PowerShell packaging scripts, and `cha_windows_test_app` instrumentation doubles platform maintenance for little day-to-day value.

---

## 4. Over-Engineered AI Automation & Context Subsystems

### 4.1 Welcome Assistant as a Self-Modifying Configuration & Diagnostics Agent (plus Ephemeral Welcome DB)
- **Code & docs:** [src/app/assistant_service.cpp](../src/app/assistant_service.cpp) (1,245 LOC), [src/workspace/workspace_config_editor.cpp](../src/workspace/workspace_config_editor.cpp) (1,398 LOC), [resources/assistant-maintenance.md](../resources/assistant-maintenance.md) (839 lines), embedded manuals in [CMakeLists.txt](../CMakeLists.txt#L477-L496), [AdminManual.md §22](AdminManual.md#22-the-assistant-as-a-configuration-tool).
- **Why remove:**
  - **2,600+ lines of C++ plus ~1,000+ lines of embedded documentation** (including `docs/MaintainerGuide.md` and `packaging/linux/README.md` compiled directly into `cha_lib`!) just so an LLM in Welcome can inspect/modify settings that **already have clean GUI screens**, toggle runtime spdlog levels, read `cha.log`, and initiate ChatGPT OAuth.
  - **Security & maintenance burden:** Requires a custom `Redactor` over logs/Events/paths, complex endpoint-matching rules (`credential_destination_pairs`) so a prompt-injected web page cannot trick the Assistant into routing a saved API key to an attacker's host, optimistic version tokens, and single-step in-memory undo.
  - **Simplifies app startup:** Without the configuration Assistant, CHA no longer needs the built-in **Entrance** forum or the ephemeral temporary **Welcome** SQLite database created on every launch and wiped on quit—it can simply open directly to the user's last active conversation or forum list.

### 4.2 Cloud "Library" / Remote RAG Vector Store Management (`Settings → Cloud stores`, Forum `Library`, `Upload to store`)
- **Code & docs:** [src/providers/folder_store_client.cpp](../src/providers/folder_store_client.cpp) (1,255 LOC), [src/notes/folder_store_operations.cpp](../src/notes/folder_store_operations.cpp) (811 LOC), [src/storage/folder_store_storage.cpp](../src/storage/folder_store_storage.cpp), [webapp/src/components/CloudStoreSettings.tsx](../webapp/src/components/CloudStoreSettings.tsx), [webapp/src/components/FolderStoreDialog.tsx](../webapp/src/components/FolderStoreDialog.tsx), [UserManual.md §12](UserManual.md#12-library), [AdminManual.md §17](AdminManual.md#17-cloud-stores-and-the-library).
- **Why remove:**
  - **Redundant 4th context mechanism:** CHA already lets users attach Notes (`##RefID`), attach local text files, and search session summaries. Adding ~3,000+ lines of OpenAI Vector Store + Google File Search management on top is overkill.
  - **Poor lifecycle & dangerous deletion:** Uploaded stores never sync with edited notes (*"To update a store, an administrator must delete it. Then upload the folder again"*). Deleting an OpenAI store in CHA deletes every underlying file from the OpenAI account—even if used by other vector stores outside CHA.
  - **Awkward cross-provider & UI coupling:** Requires tagging API keys with a special **Cloud store** dropdown, hardcodes `gemini-3.8-flash` to proxy Gemini store searches for non-Gemini characters, disables web/summary tools on `gemini-2.5` models, and introduces a forum **Default provider** setting that overrides every character's model and locks the Chat toolbar's **Model** dropdown.

### 4.3 Jev External Recipient & Current-Note Classifier (`Recipient detection`)
- **Code & docs:** [src/providers/jev.cpp](../src/providers/jev.cpp), classification state in [src/session/session_controller.cpp](../src/session/session_controller.cpp), `notes.select` / `current_note_id`, [scripts/evaluate-jev.py](../scripts/evaluate-jev.py), `cha_jev_eval_requests` in [CMakeLists.txt](../CMakeLists.txt#L643), [docs/jev.md](jev.md), [UserManual.md §6–7](UserManual.md#automatic-recipient-detection).
- **Why remove:**
  - **External alpha dependency on the critical path:** Calls `https://openrouter.ai/api/alpha/decisions` (`typesafe/jev-1.13`) before dispatching every unaddressed message, adding up to a 5-second delay (and on the first message of a session, waiting up to **10 seconds** for session naming before starting generation).
  - **Unreliable vs. explicit UI:** Even after prompt tuning ([docs/jev.md](jev.md#L516-L539)), Jev failed 5/5 times on basic cases like `"Seneca and Marcus, please answer"` and duplicate character names. Users already have deterministic `@Name` addressing, `/mcast`, `@-`, and the **To:** target selector (plus `nicknames.txt` exists almost solely for Jev).
  - **Decouples "Current Note":** Currently, asking about the open note works *only* if Jev is configured (`use_current_note`). Replacing Jev with a simple **"Attach open note"** button in the chat composer is faster, deterministic, free, and works with zero setup.

### 4.4 Cross-Session Saved Summaries (`Save summary`, Managed `Forums/` Folders, `summary_search` / `summary_read`)
- **Code & docs:** `generate_session_summary` / `commit_session_summary` in [src/notes/notes_operations.cpp](../src/notes/notes_operations.cpp), [src/session/summary_prompt_cache.cpp](../src/session/summary_prompt_cache.cpp), `note_summaries` table, `summary_search` & `summary_read` in [src/providers/tool_calls.cpp](../src/providers/tool_calls.cpp), [UserManual.md §8, §11](UserManual.md#session-summaries).
- **Why consider removing:**
  - Creates auto-managed `Forums/<forum name>` folders inside the Notes tree with special permission rules (cannot create/rename/delete notes there, renaming a forum renames the Notes folder, and **deleting a forum permanently deletes its Notes folder and all notes inside it**).
  - Makes two sequential LLM calls (**Short session summary** + **Long session summary**), fails if either LLM output includes a `#` level-1 heading, bounces Markdown through the browser's TipTap parser to save, and injects a summary catalog + two extra function tools (`summary_search`, `summary_read`) into every subsequent chat in that forum.

---

## 5. Duplicate Provider, Voice, Search & Auth Integrations

Even for capabilities worth keeping, CHA often maintains two or three distinct implementations side by side.

### 5.1 Duplicate Voice Input Stack: xAI Native WebSocket STT (vs. OpenAI WebRTC)
- **Code & docs:** [src/app/xai_voice_session.cpp](../src/app/xai_voice_session.cpp) (687 LOC), [src/media/xai_curl_socket.cpp](../src/media/xai_curl_socket.cpp) (404 LOC), [src/media/xai_transcript.cpp](../src/media/xai_transcript.cpp), [webapp/src/xaiVoiceInput.ts](../webapp/src/xaiVoiceInput.ts) (423 LOC), [webapp/src/voiceInputCapture.worklet.js](../webapp/src/voiceInputCapture.worklet.js), `cha_xai_fake_server` test binary, [AdminManual.md §16](AdminManual.md#configure-voice-input).
- **Why remove one:**
  - CHA implements two completely different real-time STT architectures: **OpenAI WebRTC** (~264 LOC in `openAiVoiceInput.ts` + SDP exchange) and **xAI Grok WebSocket** (~2,000+ LOC across custom AudioWorklet 16 kHz PCM16 capture, base64 bridge batching, native `libcurl` WebSocket session management, interim/final timestamp word deduplication, and a dedicated C++ fake WebSocket test server).
  - Keeping a single STT provider (OpenAI WebRTC) deletes ~2,000 lines of concurrent audio/socket code.

### 5.2 Duplicate Voice Output Providers & Live WebSocket / MSE Audio Streaming
- **Code & docs:** [src/providers/voice_output.cpp](../src/providers/voice_output.cpp), [src/providers/voice_websocket.cpp](../src/providers/voice_websocket.cpp), [src/media/audio_download.cpp](../src/media/audio_download.cpp), [webapp/src/textToSpeech.ts](../webapp/src/textToSpeech.ts) (493 LOC), `mse-audio-wrapper` & `codec-parser` in [webapp/package.json](../webapp/package.json), [AdminManual.md §16](AdminManual.md#configure-voice-output).
- **Why simplify:**
  - Supports **both** FishAudio and ElevenLabs (with shared-key naming quirks like `elevenlabs_voice_id` used for FishAudio voices and model-version speed-field hiding), **and** supports **both** standard HTTP synthesis and mid-stream WebSocket TTS (`voice_websocket.cpp` + `AudioDownloadManager::update_live()`).
  - Growing MP3 playback pulls in `mse-audio-wrapper` and `codec-parser`, manages Safari `ManagedMediaSource` / MP3-in-MP4 wrapping, and dynamically synthesizes **2.5 seconds of valid MP3 silence frames** in TypeScript before auto-played replies.
  - Standardizing on **one TTS provider over plain HTTP** drops `voice_websocket.cpp`, `mse-audio-wrapper`, `codec-parser`, and hundreds of lines of streaming MSE edge cases.

### 5.3 Redundant Web Search & Page Reading Backends (`web_reader`, Brave + Tavily)
- **Code & docs:** [src/providers/web_search.cpp](../src/providers/web_search.cpp), [webapp/src/components/WebSearchSettings.tsx](../webapp/src/components/WebSearchSettings.tsx), [AdminManual.md §13](AdminManual.md#13-web-search-and-page-reading).
- **Why simplify:**
  - CHA supports **Provider web search** (OpenAI/OpenRouter native) *plus* **On-demand web search** supporting both **Brave** and **Tavily**, *plus* page reading supporting both **Firecrawl** and **`web_reader`**—an unshipped, undocumented DIY microservice (`POST <url>/extract`) with fallback chaining to Firecrawl.
  - Dropping `web_reader` and picking a single search/read provider (or relying on provider-hosted search where supported) simplifies `Settings → Search API` and `web_search.cpp`.

### 5.4 ChatGPT Subscription Codex OAuth (`openai_oauth.cpp`)
- **Code & docs:** [src/providers/openai_oauth.cpp](../src/providers/openai_oauth.cpp) (893 LOC), [webapp/src/components/OpenAiConnection.tsx](../webapp/src/components/OpenAiConnection.tsx) (257 LOC), `cha_oauth_test_helper`, [AdminManual.md §8](AdminManual.md#connect-a-chatgpt-account).
- **Why consider removing:**
  - Implements a full device-code OAuth client + background token refresh + cross-process `openai-auth.json.cha-lock` file locking using a hardcoded Codex client ID (`app_EMoamEEZ73f0CkXaXp7hrann`) against ChatGPT's internal `https://chatgpt.com/backend-api/codex/responses` endpoint.
  - Requires special-case rules across Provider Settings, Notes compression, cache-retention stripping, `temperature`/`max_tokens` stripping, and `--openai-login` daemon flags, and is inherently fragile if OpenAI changes its internal Codex client contract. Standard API keys (OpenAI, OpenRouter, Anthropic, Gemini) are stable, official, and already supported.

---

## 6. Niche Micro-Features, Log-Only Machinery & Legacy Cruft

### 6.1 Hardcoded USD Token & Voice Cost Estimators (Log-Only Pricing Tables)
- **Code & docs:** [src/providers/openai_token_cost.cpp](../src/providers/openai_token_cost.cpp), [src/providers/gemini_token_cost.cpp](../src/providers/gemini_token_cost.cpp), `voice_cost_rate()` in [src/providers/voice_output.cpp](../src/providers/voice_output.cpp), [webapp/src/voiceInputUsage.ts](../webapp/src/voiceInputUsage.ts), [AdminManual.md §23](AdminManual.md#cost-estimates).
- **Why remove:**
  - **Completely invisible in the UI:** CHA calculates estimated USD costs for voice output, voice dictation, Gemini Batch audio, and Note summaries, then writes them *only* to `logs/cha.log`.
  - **Hardcoded model names & calendar price-change dates:** Embeds ~85 model/tier rate rows in C++ alongside hardcoded promotional expiration dates (`december_2026`, `january_2027`, and `December 1 at 00:00 PT` in `gemini_token_cost.cpp` and `voice_output.cpp`). These tables are guaranteed to go stale constantly.
  - **Cross-layer coupling for a log line:** `voiceInputUsage.ts` tracks unsubmitted audio seconds in the browser and sends `voice_input_usage` in the chat `session.submit` payload solely so the C++ backend can multiply seconds by a hardcoded rate and log `"Speech-to-text cost:"`.

### 6.2 Built-in Latin-to-Russian Transliteration IME (`View → Russian` / `A→Я`)
- **Code & docs:** [webapp/src/components/transliteration.ts](../webapp/src/components/transliteration.ts), [webapp/src/components/TransliterationMode.tsx](../webapp/src/components/TransliterationMode.tsx), [UserManual.md §9](UserManual.md#type-russian-with-a-latin-keyboard).
- **Why remove:**
  - Implements a custom phonetic input method inside React (`shh → щ`, `+` disambiguation escapes, paste-offset tracking, `Ctrl+Shift+Y` hotkey, `A→Я` toggle buttons, and custom `TransliteratingInput` wrappers across forms) and couples dictation language switching (English vs. Russian) to the `A→Я` button.
  - macOS and Windows already provide system-wide Russian and Russian – Phonetic keyboard layouts that require zero application code.

### 6.3 Custom Typography Styles Subsystem (`Settings → Styles`)
- **Code & docs:** [webapp/src/components/StyleSettings.tsx](../webapp/src/components/StyleSettings.tsx), [webapp/src/components/characterAppearance.ts](../webapp/src/components/characterAppearance.ts), `system/styles/<id>/config.toml`, [AdminManual.md §12](AdminManual.md#12-styles).
- **Why remove:**
  - Dedicates an entire top-level Settings screen, workspace entity type, TOML directory (`system/styles/`), bridge CRUD methods, and deletion reference-counting (`Used by`) just to bundle 5 basic CSS options (`font`, `slant`, `weight`, `size`, `text_color`). Clean default message typography (or a simple inline choice on a character if ever needed) removes a whole entity class.

### 6.4 Random character variants

CHA removed automatic per-session selection. Existing files such as `_1_2.md`
can be edited and included like any other Markdown file. [Character files](AdminManual.md#files)

### 6.5 Editable System Task Prompts (`Settings → Prompts`)
- **Code & docs:** [src/workspace/prompt_catalog.cpp](../src/workspace/prompt_catalog.cpp), [webapp/src/components/PromptSettings.tsx](../webapp/src/components/PromptSettings.tsx), `system/prompts/*.md`, [AdminManual.md §15](AdminManual.md#prompts).
- **Why remove:**
  - Exposes 9 internal feature prompts (`note-compression`, `summary-l1`..`l3`, `session-short/long-summary`, `page-formatting`, `youtube-transcript`) as per-vault editable files with **no "Reset to default" button** in the UI (restoring a default requires exporting the entire vault configuration to disk, deleting the file, and re-importing!) and fragile undocumented output contracts (e.g., if a user edits a session summary prompt and the model outputs a `#` heading, summary generation fails).
  - If multi-level note summaries, session summaries, and YouTube/page reformatting are removed, **all 9 prompts disappear automatically**.

### 6.6 Character Picture Side Panel (`View → Pictures`)
- **Code & docs:** [webapp/src/components/ChatPicture.tsx](../webapp/src/components/ChatPicture.tsx), base64 image handling in [src/workspace/workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp), [UserManual.md §10](UserManual.md#10-pictures).
- **Why consider removing:**
  - Stores character PNG/JPEG/WebP/GIF files (up to 8 MB each) as base64-encoded strings inside SQLite's `config` text table and keeps them in memory inside `Workspace::source_files_` (copying them across candidate snapshots during Settings edits), all to show a toggleable portrait panel beside the chat that advances on each turn.

### 6.7 Transient Conversation Cover (`Cover` / `Uncover`) & Background `Events` Screen
- **Code & docs:** `Transcript::cover()` in [src/chat/transcript.cpp](../src/chat/transcript.cpp), [src/storage/event_storage.cpp](../src/storage/event_storage.cpp), [webapp/src/components/EventsScreen.tsx](../webapp/src/components/EventsScreen.tsx), [UserManual.md §3, §6](UserManual.md#cover-the-start-of-a-conversation).
- **Why consider removing:**
  - **`Cover` / `Uncover`:** Because the cover boundary lives only in `Transcript` memory and idle controllers are retired as soon as you select a different session (or save a setting), switching to another chat and back **silently uncovers** hidden messages and sends them to the model again. Starting a new session or `/clear` is predictable; transient in-memory covering is a trap.
  - **`Settings → Events` (`event_history` table):** Exists only because Note Audio, `Summarize`, `Save summary`, Cloud Store uploads, and D1 Sync run asynchronously with no inline completion UI. Removing those background subsystems eliminates the need for a 1,000-row database event log and screen.

### 6.8 `libuv` Build Dependency & Legacy Migration Shims
- **Code & docs:** `FetchContent_Declare(libuv ...)` in [CMakeLists.txt](../CMakeLists.txt#L176-L185), `uv_fs_mkstemp` in [src/storage/session_database.cpp](../src/storage/session_database.cpp#L472), [src/storage/session_storage_layout.cpp](../src/storage/session_storage_layout.cpp), `read_legacy_keys` in [src/providers/api_key_store.cpp](../src/providers/api_key_store.cpp), [AdminManual.md Appendix B](AdminManual.md#appendix-b--obsolete-settings).
- **Why remove:**
  - **Entire `libuv` library linked for 1 function call:** CMake downloads, compiles, instruments (for ThreadSanitizer), and links all of `libuv` 1.52.1 into `cha_lib` **solely for a single `uv_fs_mkstemp` / `uv_fs_close` call** in `session_database.cpp`! Replacing those 15 lines with POSIX `mkstemp` (or `std::filesystem` + random hex in the already-`0700` private directory) drops an entire C library dependency immediately.
  - **Accumulated v0/v1 migration & obsolete config shims:** Schema-v1 upgrade logic, `api-keys.json` migration, `has_legacy_session_databases()` directory scanning, `drop_legacy_folder_tables()` runtime DDL on open, `history_epoch` dead-row filtering, legacy `q/` D1 table polling, and the 13 obsolete TOML field shims in Appendix B (`default_agent`, `api_key_env`, `file_references`, `[web]`, etc.) add branching across startup and storage for formats a personal v1 install no longer needs once migrated.

---

## Suggested Pruning Roadmap (What a Lean CHA Looks Like)

If you execute the high-value cuts above, **60%–70%+ of the codebase and test suite can be deleted** while keeping every core strength of CHA intact:

1. **Keep the Core:** Native macOS desktop app, multi-character Forums, Characters & Personas (plain Markdown profiles edited in Settings), `SessionController` (`@Name`, `/mcast`, `@-`, `/clear`, per-session Model/Effort overrides, automatic session naming, manual session Markdown **Download**), the 4 model API protocols (Responses, Chat Completions, Anthropic, Gemini) with standard API keys, 1 STT provider + 1 HTTP TTS provider, 1 web search/page-read tool, and a clean single-version Notes tree with direct chat attachment (`##RefID` + an **Attach open note** button + local text file drop).
2. **Wave 1 — Pure Cuts (Zero Core Impact):** Remove EPUB export/import (`fflate`), Note Batch Audio + OS codecs, L1/L2/L3/Summary/Flex summarization, YouTube/page LLM reformatting, log-only USD cost tables, Russian `A→Я` transliteration, `Settings → Styles`, `Settings → Prompts`, `Copy JSON`, `libuv`, and legacy migration shims.
3. **Wave 2 — Subsystem Unloading:** Remove D1 Sync (and optionally R2 Storage), Cloud Library Vector Stores, Jev external classifier, Cross-Session Summaries (`Forums/` managed note folders), Continuous Session Mirror, Welcome Assistant maintenance agent (and the ephemeral Entrance/Welcome DB), and Filesystem Config Export/Import + the `$$(...)` template DSL.
4. **Wave 3 — Platform & Vendor Consolidation:** Remove ChaWeb/`cha-daemon`/Linux packaging (already on hold), drop Windows host support if unused, drop duplicate xAI WebSocket STT / streaming TTS WebSocket / Codex OAuth / `web_reader`, and optionally simplify multi-vaults/SQLCipher to a single local SQLite database.
