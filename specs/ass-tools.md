# Assistant access to application settings and metadata

Status: technical design; no implementation in this change.

## 1. Required behavior and scope

In Welcome, the built-in Assistant must be able to read all application
settings and all metadata of the active vault. It does not need to read the
content of sessions or notes.

- The access is read-only. Existing write tools and protections do not change.
- The tools stay Welcome-only and use the existing admission, context-epoch,
  and cancellation checks. Desktop and ChaWeb use the same code.
- The data scope is the active vault plus the host's vault registry. Assistant
  does not open, unlock, or switch to another vault. For other vaults it sees
  only their registration.
- Reads make no network requests. They do not start, poll, retry, or cancel work.

Included data:

- All vault configuration rows and host TOML files. They are readable today;
  this change adds picture rows as metadata and rows larger than 64 KiB.
- Settings as the Settings screens show them, with defaults applied: providers,
  styles, voices, characters, personas, forums, services, API keys, R2 storage,
  and ChatGPT login status.
- The running application: version, capabilities, the active vault, the
  current vault registrations, and the current log level and file.
- Notes, folders, and annotations as catalog items, with CHA Ref IDs.
- Uploaded folder stores, whether or not anything references them.
- Sessions of a forum: the session rows, the live flag, saved toolbar
  overrides, attachments, and counts.
- Entry metadata of one session.
- Events and pending note work, through the existing `assistant_events`.

Excluded data:

- Content: entry text, model reasoning, note documents, compressed note text,
  annotations, summary prose, copied note text, attached file text, and audio
  or picture bytes.
- Secret values: API key values, the R2 secret key, OAuth tokens, and
  passwords. Key IDs, labels, presence, and uses stay readable.

Configuration prose, such as a character profile, `FORUM.md`, `PERSONA.md`, or
the voice-input prompt, is settings. It stays readable through
`vault_config_read`.

The access is complete within the size limits in sections 3 and 4. A
configuration row or an inspect item larger than one 256 KiB call, and a
directory with more than 500 rows, are only partly readable.

The following process-only state is deliberately not included. It is
short-lived, the user can see it on screen, or Events already record its result:

- Live session state other than the live flag: current recipient, cover
  boundary, generation phase, and tool counters.
- Media and transport objects: chat audio jobs, media resources, xAI voice
  sessions, and connection media.
- Folder-store upload, remove, and delete operation controls.
- Compiled `RuntimeSettings`, database file sizes, and schema versions.
- Built-in prompt resources. They are not user settings, and the maintenance
  references are already in Assistant's prompt.
- Expanded prompts and fresh loader diagnostics. The loader writes its
  warnings to the log, and `assistant_logs` reads them.
- The Welcome conversation's own temporary session.
- A note's saved scroll position.

## 2. Existing data and access gaps

| Data | Owner | Assistant access today | Change |
| --- | --- | --- | --- |
| Vault configuration rows | SQLite `config` rows; `Workspace::source_files()` | `vault_config_list`, `vault_config_read` | Picture rows are not listed. Rows larger than 64 KiB cannot be read, although Settings and import can create them. |
| Host files | Host configuration directory | `host_config_list`, `host_config_read` | None. |
| Settings with defaults | `app::settings` and `app::workspace` readers | Raw TOML only | `assistant_inspect` settings topics. |
| API keys, R2, ChatGPT login | `ApiKeyStore`, `OpenAiOAuth` | Per-file key metadata without uses; no R2 URL or access key ID; login status only through the login tool | `keys` and `services` topics. |
| Running application and vault registry | `ApplicationCommand`, `CurrentVault`, capabilities | None; `app.toml` shows only the startup selection | `application` topic. |
| Notes and folders | `note_items`, `note_summaries`, `note_audio` | Notes with pending work only | `notes` topic. |
| Folder stores | `folder_stores`, `folder_store_files` | None | `stores` topic. |
| Sessions, overrides, attachments | `sessions`, `session_character_settings`, `session_folders`, `session_notes`, `session_note_scopes`, `session_files`, `note_summaries` | None | `sessions` topic. |
| Entry metadata | `entries`, `turns`, `entry_audio` | None | `session_entries` topic. |
| Events and logs | `event_history`; log files | `assistant_events`, `assistant_logs` | None. |

The reported failure has a structural cause. An upload creates a store record
that does not depend on any character, forum, or session reference.
`Application::list_folder_stores()` gives these records to the UI, but not to
the Assistant. Configuration files cannot show them.

## 3. Changes to existing tools

`vault_config_list` lists picture rows as metadata, with `readable: false`,
`writable: false`, and the existing protection reason. In
`WorkspaceConfigStore::Impl::list_config`, do not skip unreadable rows. Picture
rows count toward the existing 500-entry limit. A prefix selects a directory or
an exact path; it cannot split one directory. If one directory has more than
500 rows, its listing stops with `truncated: true`, and the remaining names
cannot be listed. A known exact path stays readable. A personal vault is not
expected to have such a directory, so the listing gets no paging.

`vault_config_read` drops its 64 KiB bound for each file. A file is complete or
`too_large` under the existing 256 KiB bound for each call. A row larger than
256 KiB stays `too_large`. Write limits stay at 64 KiB: Assistant can read a
larger row, but cannot replace it. Text chunks are not added.

The other tools do not change. `assistant_events` stays the reader for Events
and pending note audio and compression. The saved details of a pending audio
attempt are in the `notes` topic.

## 4. New tool `assistant_inspect`

```
assistant_inspect(topic, forum_id, session_id, after)
```

| Argument | Contract |
| --- | --- |
| `topic` | One of `application`, `providers`, `styles`, `voices`, `characters`, `personas`, `forums`, `services`, `keys`, `notes`, `stores`, `sessions`, `session_entries`. |
| `forum_id` | Exact forum ID for `sessions` and `session_entries`; otherwise null. |
| `session_id` | Exact session ID for `session_entries`; otherwise null. |
| `after` | Null for the first page. Otherwise the `next_after` value of the previous page for the same topic and IDs. |

All four arguments are required in the provider schema. A wrong combination
returns `invalid_argument`. The tool description states that the tool reads
settings, the vault registry, notes and folders, OpenAI and Gemini stores, and
session metadata. It also states that the tool never returns session or note
text, or credentials.

A result has this form:

```json
{"topic": "stores", "items": [], "next_after": null}
```

Paging:

- Every topic except `application` and `services` returns a list. Each item
  has a key: an integer ID for `notes`, `stores`, and `session_entries`, and a
  string ID for the other topics.
- Items come in ascending key order. String keys compare by bytes.
- A page holds at most 64 KiB of encoded items. When more items remain,
  `next_after` holds the last returned key as a string. Otherwise it is null.
- The next page starts at the first key greater than `after`. If that item was
  deleted, the page continues with the next key. No cursor errors exist.
- A page holds at least one item. An item larger than 64 KiB is the only item
  on its page. An item that does not fit in the 256 KiB call limit is replaced
  by its key and `"too_large": true`, so paging continues past it. Its fields
  stay unreadable; for example, a store with very many `file_ids`.
- Each call is a new observation. Pages can show changes made between calls.

A missing forum or session returns `not_found`. The tool uses the existing
`stale_context`, `unavailable`, `cancelled`, and `too_large` errors.

## 5. Topics

### 5.1 `application`

One item from the application's host command and current vault:

- `version`: the package version.
- `capabilities`: `can_modify` and `can_transfer_r2`.
- `active_vault`: the name of the vault that is open now.
- `vaults`: every current registration with its name, source filename, data
  path, `password_protected`, `parent`, `mirror`, `modify`, and `r2_etag`.
- `mirror_base` and `modify_base`.
- `log_level` and `log_file`: the current process values.

These are the values in use. The saved startup selection and the saved
logging settings are in `app.toml`. Saved host files can differ from these
values until restart; the model compares them with `host_config_read`.

### 5.2 Settings topics

Each settings topic uses the current `store->snapshot()` and the existing
readers that the Settings screens use. The JSON is the existing `to_json`
output of those DTOs, so Assistant and Settings show the same values. Key
fields hold saved key IDs, never key values. Saved source text stays
available through `vault_config_read`. The instructions already say which
settings each provider API ignores; this tool does not calculate applicability.

| Topic | Items |
| --- | --- |
| `providers` | `settings::get_provider` for each provider: all `ProviderDetail` fields, including the saved key ID and `used_by`. |
| `styles` | `settings::list_styles`. |
| `voices` | `settings::list_voices`. |
| `characters` | `app::workspace::character_detail` for each character, without `character_markdown`, `editable_markdown`, and the `available_*` option lists. The item keeps the provider, style, voice, effort and search overrides, `markdown_files`, Files references, and writable flags. |
| `personas` | `app::workspace::persona_detail` for each persona, without `persona_markdown` and the `available_*` option lists. |
| `forums` | `app::workspace::forum_detail` for each forum, without `forum_markdown`. The item keeps members, the default character and persona, `markdown_files`, and Files references. |
| `services` | One item: `jev`, `session_naming`, `notes`, `web_search`, `voice_input`, and `voice_output` from their `settings::get_*` readers, with null for a service that is not configured; `r2_storage` from `settings::get_r2_storage`; and `openai_auth` with `status`, `attempt_expires_at`, and `error` from `settings::openai_auth_status`. The device code and verification URL stay in the login tool. |
| `keys` | `settings::list_api_keys` with `store->folder_store_usage()`: ID, label, `has_value`, and `used_by`, including store uploads. |

The lists include the built-in entries that the workspace holds, such as
Assistant, Guest, and Entrance.

### 5.3 `notes`

Every folder, note, and annotation, keyed by note ID. Each item is the
existing `NoteItem` JSON (`id`, `parent_id`, `kind`, `name`, `has_compressed`,
`read_only`, `is_summary`, `summary_linked`, `forum_id`, and the folder's
`store` summary), plus:

- `ref_id` from `note_item_ref_id`.
- `audio_clips` for a note, from `get_note_audio_info`.
- `audio_attempt` for a note with pending audio, otherwise null: the saved
  `id`, `clip_count`, `service_origin`, `api_key_id`, and `batch_id`. These are
  the values saved when the attempt started; current service settings can
  differ.

Source: `store->list_note_items()` and `store->get_note_audio_attempts()`. Do
not call `get_note`, `get_note_text`, or any document conversion.

### 5.4 `stores`

Every folder store record, keyed by store ID, from `store->list_folder_stores()`:
`id`, `folder_id`, the folder's `ref_id` and `folder_name`, `service`,
`api_key_id`, `remote_store_id`, `state`, `note_count`, `uploaded_at`,
`remote_outcome_unknown`, and `file_ids`.

This list includes incomplete stores and stores that nothing references. The
state is CHA's saved state, not a fresh check with the provider. `note_count`
is the count at upload, not the folder's current count. CHA does not keep a
mapping from notes to remote files.

### 5.5 `sessions`

Every active session of one forum, keyed by session ID:

- `session_id`, `label`, `updated_at`, `recent_pending`, `discardable`, and
  `history_epoch`.
- `live`: true when `LiveSessionManager::snapshot()` lists the session.
- `entry_count`, `input_tokens`, `output_tokens`, and `cached_audio_count`.
- `overrides`: the saved toolbar Model and Effort values as
  `character_id`, `provider_id`, and `reasoning_effort`.
  `SessionJournal::set_character_settings` writes each toolbar change at once,
  so the database is current for open and closed sessions.
- `folders`: attached folder IDs with their Ref IDs.
- `notes`: copied notes with `note_id`, `ref_id`, the captured `name`,
  `explicitly_added`, `source_exists`, and the character `scopes`.
- `files`: attached text file names with their byte lengths.
- `summary_note_id`: the linked summary note, or null.

Source: a new read-only `SessionRepository` method. It follows `list_forum`:
a shared operation lock, a read-only connection with the vault password, and
`archived_at IS NULL`. Entrance stores no sessions and returns `not_found`.

### 5.6 `session_entries`

Every entry of one session, keyed by entry ID: `entry_id`, `epoch`,
`request_id`, the turn `state`, `kind`, `participant_id`, `display_name`,
`addressed_to`, `addressed_to_name`, `status`, `created_at`, `input_tokens`,
`output_tokens`, `web_search_used`, `text_bytes`, and the cached audio
`content_type` or null.

Source: a second new read-only `SessionRepository` method with the same
connection rules.

### 5.7 No content in SQL

The new SQL uses explicit column lists. It never selects `entries.text`,
`session_notes.text`, `session_files.text`, `note_items.content_json`,
`note_items.compressed_text`, or audio and picture blobs. It calculates
lengths in SQL, for example `length(CAST(text AS BLOB))`, and presence with
`EXISTS`.

## 6. Implementation

- Add the `assistant_inspect` schema to `add_maintenance_tools` in
  `src/providers/tool_calls.cpp`.
- Add `inspect_tool` to `src/app/assistant_service.cpp`. Parse the topic with a
  plain switch. The tool is read-only, so it does not call `require_main`.
- Add two members to `AssistantService::Links`:
  - `ApiKeyStore* api_keys`, for providers, keys, and R2.
  - `std::function<nlohmann::json()> application`, called with the lifecycle
    lock held, like `pending_notes`. It reads `command.vaults` and
    `current_vault_.get()` directly. It must not call `vault_snapshot()`,
    because that method locks the lifecycle mutex.
- The existing `sessions` and `repository` links give the live snapshot and
  the session readers.
- Run every reader while the lifecycle lock is held. Each reader uses a lock
  order that already exists: `pending_notes` reads the store under lifecycle,
  and `Application::list_sessions` reads the repository and the live snapshot
  under lifecycle. The tool never releases the lifecycle lock. Its only wait is
  the `LiveSessionManager::snapshot()` reply from the session runtime thread,
  the same wait that `Application::list_sessions` does under lifecycle.
- Apply the existing `Redactor` to the encoded result, as `assistant_events`
  and `assistant_logs` do. Private root prefixes show as `[path]`; exact
  configured paths stay readable in host files.
- One paging helper serves all list topics: it takes the ordered keyed items
  and `after`, and returns the page and `next_after`.

No database migration, new table, cache, index, background work, or network
call is necessary.

## 7. Assistant instructions

Update `resources/assistant-maintenance.md`:

- Add the `assistant_inspect` contract and the topic list.
- Update the `vault_config_list` and `vault_config_read` contracts for picture
  rows and the read limit.
- Replace "Your tools cannot read them" for toolbar overrides with the
  `sessions` topic.
- Questions about stores start with `stores`. `characters`, `forums`, and
  `sessions` show where a store's folder is used. Keep uploaded stores, Files
  references, and chat attachments distinct.
- Distinguish saved source (`vault_config_read`) from Settings values with
  defaults (settings topics).
- Follow `next_after` when an answer needs the whole list. A partial list is
  not evidence that an item is absent.

Update `resources/application-guide.md` only where it says what Assistant can
inspect in Welcome.

## 8. Tests

Use synthetic fixtures only.

- `tests/app/unit_assistant_service.cpp`: argument validation, the
  Welcome-only check, `stale_context`, cancellation, and every topic.
- An uploaded but unattached store: a ready OpenAI record and an incomplete
  Google record appear with their saved fields.
- Each settings topic matches the existing reader JSON for the same snapshot,
  minus the removed Markdown and option fields.
- No secrets: seeded key values, the R2 secret key, and OAuth tokens never
  appear.
- No content: seeded sentinels in entry text, note documents, compressed text,
  session note copies, and attached files never appear.
- Overrides: a toolbar change shows in `sessions` after the session closes.
- Paging: a notes list larger than 64 KiB is read completely; a deleted
  `after` item does not stop the paging; an item larger than 256 KiB shows as
  `too_large`, and the next page continues after it.
- A pending audio attempt shows its saved service origin, key ID, clip count,
  and batch ID in `notes`.
- `vault_config_list` shows picture rows. A 100 KiB row is readable, and a
  write larger than 64 KiB is still rejected.
- Read-only: the configuration version and the Events do not change.
- Provider tool-schema tests for each supported API.

## 9. Implementation sequence

1. Change `vault_config_list` and `vault_config_read`, with tests.
2. Add the two `SessionRepository` metadata readers, with storage tests.
3. Add `assistant_inspect`, its topics, the paging helper, and the `Links`
   wiring, with service tests.
4. Add the tool schema and update the two resource files.
5. Run the native tests. In Welcome, with a synthetic uploaded store that has
   no references, ask which folders are uploaded.
