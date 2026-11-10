# Predefined prompts: storage, management, and use

Date: November 10, 2026

Status: Technical design. This document defines storage, the desktop Settings
interface, and use of vault prompt text by the corresponding model operations.
It does not implement the feature.

## 1. Scope and requirements

CHA has a fixed catalog of prompts for internal LLM operations. The application
defines each prompt's identity, name, and default text. A user can view and
replace its text in the active vault to adjust how models execute the
corresponding operations.

| Requirement | Behavior |
| --- | --- |
| Predefined catalog | Use the five resources listed below; split the existing combined summary resource into short and long summary resources. |
| New vault initialization | Copy all five resource texts into the new vault's `config` table before making the vault available. |
| Existing vault initialization | On opening a vault with no `system/prompts/` configuration entries, insert all five bundled prompt texts before publishing its workspace. |
| Content editing | Edit text directly, or upload a file and review its text before Save. |
| Fixed identity | No creation, deletion, or renaming of prompts. |
| Display | Show literal text, including Markdown syntax, rather than rendered Markdown. |
| Vault scope | Saved text belongs to the active vault. |
| Interface | Settings in desktop CHA only; no ChaWeb management interface. |
| Runtime use | Each operation uses its corresponding prompt from the active vault's workspace snapshot. Saved edits apply to operations accepted after the save. |
| Session summaries | Run separate short and long summary model operations against the same transcript, then combine their results into one summary note. |

There are no tags, folders, versions, history, model selectors, prompt execution
buttons, or configurable usage assignments. Each prompt has one fixed operation
mapping. There is no New prompt screen or delete action.
Editing does not require a configured provider or API key.

## 2. Initial catalog

Use these stable IDs, fixed display names, and bundled defaults:

| ID | Display name | Default source |
| --- | --- | --- |
| `note-compression` | Note compression | `resources/compression-prompt.md` |
| `session-short-summary` | Short session summary | `resources/session-short-summary-prompt.md` (new) |
| `session-long-summary` | Long session summary | `resources/session-long-summary-prompt.md` (new) |
| `page-formatting` | Page formatting | `resources/page-formatting.md` |
| `youtube-transcript` | YouTube transcript formatting | `resources/youtube-transcript.md` |

List prompts in this catalog order. IDs are internal and do not appear as extra
labels in the interface. Names are not editable or stored in vault metadata.

CMake already embeds the compression, combined summary, page-formatting, and
YouTube resources into `cha_lib`. Reuse the existing embedding mechanism and
the three unchanged prompt functions. Replace the combined summary resource
and `embedded_session_summary_prompt()` with the two new resources and
`embedded_session_short_summary_prompt()` /
`embedded_session_long_summary_prompt()`. Remove the old resource and generated
symbol from the build when the operation call sites have been converted. Do
not introduce a second packaging path or copy prompt bodies into C++ source.

### Split summary defaults

Derive both new resources from `resources/session-summary-prompt.md`. Preserve
its shared rules on factual accuracy, attribution, user contributions,
corrections, decisions, uncertainty, source links, and language in each
self-contained prompt. Copy these instructions into each resource rather than
adding template includes or a runtime prompt-assembly subsystem.

- The short prompt requests only the compact retrieval summary: one paragraph
  followed by `**Keywords:**`, retaining the existing 60–100 word target and
  3–8 keyword guidance. It returns the section body without a title or heading.
- The long prompt requests only the detailed, self-contained account of the
  full transcript. It retains the coverage and preservation rules, without
  reference to a generated short summary or a fixed word count. It returns
  only the section body, with any subheadings at level two or deeper.
- Both retain the existing Markdown restrictions and checks relevant to their
  own output. Neither asks the model to emit `# Short summary` or
  `# Long summary`; CHA supplies those headings when combining the results.

These are independently editable vault prompts. The long operation does not
receive the short result, and the short operation does not depend on the long
result. Both summarize the same original transcript.

Session naming, Assistant maintenance instructions, character instructions,
and forum instructions are outside this catalog.

## 3. Screen design

These wireframes describe the proposed interface. Use CHA's existing Settings
rows, icons, spacing, and text editor dialog. Screen names belong in the
existing application navigation title; do not add duplicate panel headings,
legends, or explanatory text.

### Settings and prompt list

Add Prompts as the last row in Settings, as in the supplied sketch.

```text
Settings                         Prompts
┌──────────────────────────┐     ┌─────────────────────────────────────┐
│ Existing settings rows   │     │ ‹ Settings                          │
│ ...                      │     │                                     │
│ Prompts                › │ ──► │ Note compression                  › │
└──────────────────────────┘     │ Short session summary             › │
                                 │ Long session summary              › │
                                 │ Page formatting                   › │
                                 │ YouTube transcript formatting     › │
                                 └─────────────────────────────────────┘
```

Selecting a row opens that prompt. There is no New prompt row. A newly created
vault shows all five prompts, reading the resource texts copied into its
`config` table during creation.

### Prompt detail

```text
Note compression                 ← existing application title
┌─────────────────────────────────────────────────────┐
│ ‹ Prompts                         [Edit] [Upload]   │
│                                                     │
│ The prompt's literal text appears here.              │
│                                                     │
│ # Markdown headings remain text.                    │
│ Indentation and line breaks remain visible.          │
│                                                     │
└─────────────────────────────────────────────────────┘
```

`[Edit]` and `[Upload]` represent compact icon buttons with accessible names
and tooltips `Edit text` and `Upload text`. There is no trash icon, editable
name, or additional name row. Long text scrolls within the normal content
area. Render text as a React text child with `white-space: pre-wrap` and
wrapping for long unbroken lines. Do not use Markdown or HTML rendering.

### Edit or upload review

```text
┌─ Edit text ─────────────────────────────────────────┐
│ ┌─────────────────────────────────────────────────┐ │
│ │ Editable plain text                             │ │
│ │                                                 │ │
│ │                                                 │ │
│ └─────────────────────────────────────────────────┘ │
│                                                     │
│ [existing transliteration control] [Clear]           │
│                                    [Cancel] [Save]  │
└─────────────────────────────────────────────────────┘
```

Reuse `TextEditorDialog`, including its keyboard, focus, Clear, and
transliteration behavior. Its prompt-specific textarea has an accessible
label but no Markdown helper placeholder. If necessary, add an optional
placeholder prop while preserving the existing default for other callers.

Edit initializes the dialog with the currently loaded text. Upload selects
one UTF-8 `.txt` or `.md` file and initializes the same dialog with the file's
full text. Upload replaces the draft; it does not append text and does not
save immediately. The user can edit the uploaded text before Save.

Cancel, including Escape when no save is running, closes the dialog and
discards the draft. Cancelling the file picker changes nothing. Save updates
the vault and closes the dialog only after a successful response. A failure
keeps the text and the dialog open and displays an actionable error.

Disable repeated Save and conflicting actions while a save is in progress.
The modal dialog prevents navigation through the underlying screen. Use the
existing application behavior for closing the app and for vault maintenance;
do not add prompt-specific draft persistence or a new navigation guard system.

## 4. Text rules

Prompt content is plain UTF-8 text. The application does not expand includes,
variables, templates, commands, or references. Strings such as `$$(file.md)`,
HTML tags, and Markdown syntax are literal content.

Use the existing 256 KiB text-file limit for both uploaded and manually edited
content. Check UTF-8 byte size, not JavaScript string length. Validate on the
backend as well as in the interface. Reject malformed UTF-8 and the binary
control bytes already rejected by `validate_text_file()` / `readTextFile()`.
The existing bridge envelope limit remains in effect.

Allow empty and whitespace-only text. An empty saved value remains in its
row and resolves to empty text in the workspace. The operation passes that
empty text to the provider adapter as described in section 10. Apply the
same text validation rules to all five prompts.

Normalize a leading UTF-8 BOM and line endings at the prompt input boundary:
remove one leading BOM and use LF for CRLF and lone CR. Preserve all other
text, including indentation, blank lines, and trailing spaces. Apply the same
normalization to imported prompt rows when resolving their displayed value.
Normal loading does not rewrite stored rows.

Reuse the existing text validation and upload reading code with a narrow
`allow_empty` / `allowEmpty` option that defaults to false. Existing chat file
uploads must retain their current rejection of empty files. Prompt callers
enable the option and supply a prompt-specific size error, for example:
`Prompt text is too large. The limit is 256 KiB.`

Check upload extensions case-insensitively and validate the bytes; a MIME type
alone is not evidence that a file is valid text. Reject unsupported files
before replacing a draft. Reset the file input after every attempt so the
same file can be selected again. Do not upload a local path or retain the
original filename in the vault.

## 5. Durable representation

Use the existing SQLite `config(name, content)` table. Store all five prompt
texts from the time a new vault is created, one raw text row per prompt:

```text
system/prompts/note-compression.md
system/prompts/session-short-summary.md
system/prompts/session-long-summary.md
system/prompts/page-formatting.md
system/prompts/youtube-transcript.md
```

The `.md` suffix uses the existing configuration import/export format; it
does not change the plain-text display or introduce Markdown interpretation.
There is no new table, schema version change, TOML metadata, next-ID counter, or
separate prompt database.

### New vault initialization

When creating a new vault, copy the five bundled resource texts into these
five `config` rows. Use the catalog's embedded resource functions as the source
and validate the texts before committing them. Include all five rows in one
configuration transaction, and make the vault available only after the full
initial configuration is committed. A failure uses the existing cleanup for
an unpublished new vault; do not leave a usable vault with partial prompt
initialization.

Apply this to first-run vault creation and the normal New vault action. The
normal New vault action currently copies configuration from the active vault;
replace the five prompt entries in that initial configuration with the
application resource texts so the new vault starts with the bundled prompts.
An explicit copy of an existing vault retains its stored prompt texts, as
other copied configuration does; fill any missing prompt rows from resources
in that newly created copy. Restoring an existing database preserves its
configuration; the existing-vault initialization rule below applies when the
restored vault is opened.

These paths are logical filenames in SQLite. Creating the `system/prompts`
directory means inserting its file rows in `config`; no physical directory
or separate directory row is required. Export materializes the stored rows
as files through the existing configuration export path.

Once initialized, the vault's stored text is authoritative. Saving edits
updates that row. Application upgrades must not replace stored text, including
text that still equals the resource version copied at vault creation. A
repeated save of identical stored text is an ordinary no-op through the
existing config diff machinery.

### Opening an existing vault

When the main process opens or reopens an existing vault, check whether any
`config.name` begins with the exact prefix `system/prompts/`. If none does,
the logical directory does not exist. Insert all five catalog paths with
their current bundled resource texts in one transaction before publishing
the workspace or admitting operations for that vault.

Apply this check during startup, vault switching, and reopening after
maintenance, including import or restore. An existing vault first opened by
the new application version therefore receives durable prompt defaults,
which are subsequently used by Settings and the model operations and included
in exports and backups.

Check absence and insert the rows within the existing serialized configuration
write path and transaction. Never overwrite existing rows. Once any entry is
present under the prefix, this initialization is a no-op. Repeated opens do
not reset defaults or change user edits. No migration marker, schema version
bump, or background task is needed; the stored paths are the condition.

If initialization fails, roll back all inserted rows, preserve the original
database, and report the vault-open failure through the existing path. Do not
publish a partially initialized workspace. A later open can retry normally.
Only the process that owns settings writes performs initialization; a
secondary process follows the existing read-only settings role and does not
write default rows.

### Missing rows in an existing prompt directory

If the logical directory already contains any entry, including an unknown or
obsolete prompt file, opening the vault does not backfill individual missing
rows. Preserve the existing per-prompt fallback for such incomplete
configuration and for read-only loading. The value shown for a catalog entry
and used by its operation is:

```text
valid stored row exists → its text, including an empty string
otherwise               → the bundled default text
```

Saving a prompt whose row is missing creates its row, even when the submitted
text equals the bundled default. This compatibility path does not replace
the requirement to persist all five rows during new-vault creation or when
opening an existing vault whose prompt directory is absent.

There is no Reset to default control or deletion endpoint in this phase.
Removing a prompt row through existing configuration maintenance causes the
default to appear again; it never removes the catalog item.
Removing every entry under `system/prompts/` also makes the directory absent,
so the next main-process open/reopen persists all five defaults again.

Only catalog IDs are addressable. Build storage paths from a successful
catalog lookup, never from an arbitrary client path. Additional files under
`system/prompts/` cannot define new prompts or rename existing ones.

### Invalid or obsolete stored data

Unknown prompt paths are unused configuration. Preserve their rows,
ignore them for the catalog, and log a warning. This permits older application
versions to retain stored texts for prompts introduced by a newer version.

If a vault contains the former `system/prompts/session-summary.md` path,
preserve it as unused configuration and warn. Do not attempt to split an
arbitrary user-authored combined prompt or use it for either new operation.
Missing short/long rows follow the normal bundled-default fallback; newly
created vaults receive both new rows. There is no combined-summary catalog
entry or management control.

For a recognized path whose stored text fails validation, preserve the row,
log a warning naming the path and reason, and display the bundled default.
Do not log the prompt body. An unused invalid prompt row must not prevent
startup, unrelated settings edits, or vault maintenance from succeeding.
Saving valid text for that prompt replaces the invalid row.

Dedicated prompt saves validate strictly before commit. The loader's fallback
behavior must not turn an invalid save into apparent success.

## 6. Workspace implementation

Add a small `src/workspace/prompt_catalog.h/.cpp` pair. It defines the five
catalog entries, resolves an ID, derives its fixed storage path, and provides
shared prompt text normalization/validation. Register the source in CMake.
Use a fixed collection and linear lookup; five entries do not need an index
or plugin registry.

Add `WorkspacePrompt` with `id`, `display_name`, and `text` to the immutable
workspace model. During `Workspace::load`, build the prompt collection from
validated `TextSource` rows, using bundled defaults only for the missing or
invalid-row cases described above. Expose
`prompts()` and `find_prompt(id)`. Follow the existing snapshot ownership
model: callers hold the `shared_ptr<const Workspace>` while using its values.

This collection is the resolved view inside the existing workspace snapshot,
not a separate mutable store or cache. No prompt-specific worker, lifecycle,
polling, or refresh timer is required.

Use the catalog to add the five initial rows in the existing vault creation
flows, including first-run setup. In `src/app/vault_operations.cpp`, account
for both normal creation through
`create_workspace_session_database_from_configuration()` and explicit vault
copying. Prepare the initial prompt rows before the new vault is published,
using the transaction and failure rules in section 5. Keep resource selection
in the catalog/application layer; the generic SQLite copy and configuration
transfer helpers must not acquire an unconditional prompt-reset policy.
Add the absent-directory check to `WorkspaceConfigStore`'s main-process
open/reopen preparation, after acquiring the existing write ownership and
before publishing the initial snapshot. Reuse the catalog values and the
existing config transaction helpers. Build and validate the candidate from
the rows that will be committed, then publish it only after commit succeeds.
Where D1 recording is active, record these inserts through the existing
`config.edit` mechanism in the same transaction; add no prompt-specific sync
protocol. Pass `command.machine` from `Application` into
`WorkspaceConfigStore::open()` and set the store's machine identity before
loading and initializing the first workspace. The current call to
`set_sync_machine()` after open is too late for startup initialization. The
store retains the machine identity when it switches or reopens a vault.

Keep `Workspace::load` itself read-only. Offline import, export, backup, and
restore helpers retain their transfer semantics and do not seed prompts.
Opening or reopening their resulting database runs the check above. Do not
turn every workspace reload, individual Settings read, or config edit into
an initialization operation.

Add `WorkspaceConfigEditor::write_prompt_text(id, text)` and
`WorkspaceConfigStore::apply_prompt_text(id, text)`. The store method uses
the existing `Impl::edit` transaction path:

1. Require the main process and acquire the existing configuration mutex.
2. Resolve the catalog ID; reject an unknown ID.
3. Normalize and validate the submitted text.
4. Replace only the corresponding row in the disposable `TextFiles` candidate.
5. Build the candidate workspace and commit changed config rows atomically.
6. Publish the candidate snapshot only after commit succeeds.

Return no affected forum IDs. Saving a prompt publishes its new text for
subsequent operations without restarting or refreshing live sessions or
cancelling model requests. Operations already accepted retain their captured
text as described in section 10. Validation, SQLite write, or commit failure
leaves the rows and published snapshot unchanged.

Do not broaden the Assistant configuration-tool writable-path allowlist as
part of this feature. Dedicated Settings operations use the editor directly.
The existing general configuration read tools can still see stored text under
their current rules and limits. Runtime selection follows the fixed operation
mapping in section 10, not arbitrary paths or tool arguments.

## 7. Application and native bridge contract

Add settings operations and `Application` wrappers following the provider and
style settings patterns. Keep the lifecycle mutex and `require_admitted(epoch)`
checks used by those operations. The store enforces main-process writes.

Use three native methods:

| Method | Parameters | Result |
| --- | --- | --- |
| `prompt.list` | `{}` | `PromptSummary[]` in catalog order |
| `prompt.get` | `{ "prompt_id": "note-compression" }` | `PromptDetail` |
| `prompt.update` | `{ "prompt_id": "note-compression", "text": "..." }` | Saved `PromptDetail` |

```typescript
type PromptSummary = {
  id: string;
  display_name: string;
};

type PromptDetail = PromptSummary & {
  text: string;
  writable: boolean;
};

type PromptUpdate = { text: string };
```

The list omits bodies; the detail request loads one body. `writable` reflects
whether the current process may save settings. An unlocked protected vault
uses the same rules as other settings; encryption does not itself mean
read-only.

Validate exact parameter keys and string types. Reject `display_name`, file
paths, null text, and missing text. Resolve URL-safe IDs against the catalog.
An unknown well-formed ID returns `not_found` with `That prompt was not found.`
Invalid text returns `invalid_argument` with a specific validation message.
Use the existing bridge handling for database failures, secondary-process
writes, stale vault epochs, and application unavailability.

There are no create, delete, rename, or reset methods. All three methods
require the current context epoch. None is a control method or changes the
context epoch. Use the existing method tables, policies, and reply handling.

Add DTO schemas to `resources/dto.yaml`, C++ values and JSON serializers in
`src/runtime/protocol.h/.cpp`, and dispatch in
`src/bridge/settings_dispatch.cpp`. The simple parameter shapes can use the
existing `require_only_keys`, `require_identifier`, and `require_string`
helpers directly. Regenerate `webapp/src/api/schema.d.ts`; do not edit it by
hand.

## 8. Frontend implementation

Add `listPrompts`, `getPrompt`, and `updatePrompt` to `ChaClient`, its native
implementation, and response guards. Use the established error presentation.
Test client fixtures must implement the added methods where required.

Add `PromptSettings.tsx` with a list screen and a detail screen. Add
`settings-prompts` and `settings-prompt` views, list/detail navigation actions,
and the inspected prompt ID/name to `state/view.ts`. Wire the screens into
`components/App.tsx`, the Settings row into `components/Settings.tsx`, and
the names into `navigationTitle`. No new browser URL scheme is necessary.

Keep prompt bodies and the open editor draft local to the detail screen.
Do not add them to bootstrap, global navigation state, `promptDrafts.ts`
(which concerns chat composition), browser storage, or the Notes tree.

Reuse `SettingsRow`, `BackToSettings`, `LoadFailure`, `useLoad`, icons, and
`TextEditorDialog`. A small prompt-specific action component can own the
upload input and dialog. Do not reuse `DetailActions` unchanged: its current
upload action saves immediately, which would violate the review-before-save
requirement. Do not change upload semantics for other settings screens.

A list load failure offers Try again. A detail load failure retains a route
back to Prompts and offers retry. Opening a different prompt starts with its
own text and clears the prior draft. A failed save keeps the draft. Successful
save replaces the displayed text with the server-returned value.

Use the existing detail refresh context and stale-draft conventions. When a
refresh observes changed text, a clean editor can take the new value; a dirty
editor retains its draft and blocks Save until the user cancels and reloads.
Do not claim server-side conflict detection: updates follow the existing
settings behavior, with the last successful write winning if no intervening
change was observed. No new revision token or conflict resolution system is
part of this design.

On vault change, use the existing application reset and context-epoch behavior
to discard old screen data. Ignore late responses for another prompt or vault.
Never retry an old save against the newly active vault.

## 9. Import, export, backup, and synchronization

The chosen paths are ordinary stored `.md` configuration files, so existing
whole-vault operations transport the stored prompt texts:

- Export materializes the stored prompt rows, including all five initial
  texts from new vaults and older vaults initialized on open. The export
  helper itself does not manufacture files for individually missing rows.
- Import follows existing configuration replacement behavior. Missing prompt
  rows in a partially populated prompt directory resolve to bundled defaults.
  If the imported configuration has no entries under `system/prompts/`, the
  subsequent main-process reopen initializes all five rows before admission.
- Database backup, restore, R2 transfer, and vault protection include prompt rows
  through the existing database paths.
- D1 synchronization replicates prompt edits through the existing `config.edit`
  path described below. It does not need a prompt event type, table, or queue.
- Vault merge follows the existing source-over-destination path policy. A
  source prompt row replaces the destination row at that path; no source
  row means no replacement of that destination row during merge.

All five prompt identities remain present after any of these operations.
Verify these paths with focused tests; do not add a second transfer mechanism.

All five initial prompt texts belong to the vault once created or initialized
on open and travel with its backup, even if the user never edits them.
Restoring a vault with stored prompt rows preserves those texts. Restoring an
older vault without the prompt directory causes all five defaults to be
persisted when it is opened. An incomplete existing prompt directory retains
the per-prompt fallback for its missing entries.

### D1 replication of prompt edits

When D1 recording is active, saving changed prompt text updates the local
`config` row and queues a `config.edit` record with the row's path and
normalized text in the same SQLite transaction. The sending machine's existing
sync worker uploads the queued record to D1.

Import and merge can introduce raw prompt rows with malformed UTF-8. Preserve
those bytes in the local vault. In `record_config_edit()`, omit changed `set`
entries under `system/prompts/` whose content is malformed UTF-8 before
checking whether there are changes to record and serializing the record.
Log the path and reason without the prompt body. Queue the remaining changes
using the existing rules. An omitted row leaves the receiving vault's row as
it was. A later valid edit to that path replicates normally.

The receiving machine's sync worker reads the record for the same vault and
passes it to `WorkspaceConfigStore::apply_sync_config_edit()`. This path
replaces or inserts the row by path in the receiving vault's configuration,
loads a candidate workspace, and publishes it only after the SQLite commit
succeeds. Applying the incoming record does not queue it again.

Fresh Settings reads and newly accepted model operations on the receiving
machine use the resolved text from the new snapshot. Already accepted
operations retain their captured prompt text as described in section 10.

## 10. Use in model operations

Replace the direct embedded-prompt selections in
`src/notes/notes_operations.cpp` with the corresponding resolved prompt text
from the operation's `Workspace` snapshot:

| Operation | Catalog ID | Integration point |
| --- | --- | --- |
| Compress note | `note-compression` | `NotesService::compress_note()` passes the text to `prepare_note_provider()`. |
| Generate short session summary | `session-short-summary` | `NotesService::generate_session_summary()` prepares the short operation's definition. |
| Generate long session summary | `session-long-summary` | `NotesService::generate_session_summary()` prepares the long operation's definition. |
| Format imported article | `page-formatting` | `format_note_page()` uses the text when `youtube` is false. |
| Format imported YouTube transcript | `youtube-transcript` | `format_note_page()` uses the text when `youtube` is true. |

Use the resolved text as `CharacterDefinition::system_prompt`, replacing the
current bundled prompt in full. Do not prepend or append the bundled text.
Keep the note, session transcript, or imported page as the existing user
message. Do not expand templates or includes in prompt text. Embedded resource
functions remain the source for new-vault and absent-directory initialization
and the compatibility fallback in section 5, not a separate runtime selection
path.

The Settings detail view and model operation must read the same resolved
workspace value. A valid stored value, including an empty string, takes
precedence over the resource. Use the existing missing/invalid-row fallback
consistently in both places. Do not silently retry with a bundled prompt if a
model fails or returns invalid output after using user-edited text.

An empty resolved prompt reaches the provider adapter as an empty system
prompt. The Responses adapter for ChatGPT subscription requests keeps its
existing `instructions` fallback, `You are a helpful assistant.`. Other
request builders receive no system prompt text from the operation. A nonempty
resolved prompt is supplied in full, including whitespace-only text.

### When edits take effect

Capture one immutable workspace snapshot when the operation is accepted under
the existing lifecycle/admission checks. Select its provider settings and
prompt text from that same snapshot:

- Compression obtains a named snapshot before `prepare_note_provider()` and
  copies the selected text into the existing owned character definition before
  launching the worker. Waiting for the compression mutex does not change it.
- Session summary uses the snapshot it already captures to prepare both
  character definitions before launching its worker. Both definitions use
  the same selected Notes provider/model and reasoning settings. Editing either
  prompt while the short operation runs cannot change the pending long one.
- Page import uses the snapshot already captured by `read_note_page()` and
  retained by its worker. Select the formatting prompt from that snapshot
  after retrieval, even if Settings changed while the page was downloading.

The existing `SharedCharacterDefinition` owns the selected text. A retry of
the same operation uses that definition and the same prompt. Keep the existing
snapshot alive wherever a reference into it is used; do not retain a dangling
reference from a temporary snapshot or add a separate prompt cache.

A successful save makes the new text available to the next accepted
corresponding operation, without an application restart or manual reload.
Already accepted, queued, or running operations keep their captured text.
An independently submitted later operation captures the current value.
Existing vault epoch checks and cancellation prevent old-vault work from
starting or committing against a new vault; do not add a new lifecycle policy.

This selection belongs in the shared backend, so it also applies when an
existing ChaWeb or Assistant entry point invokes one of these operations.
Prompt management remains limited to desktop Settings.

### Two operations, one summary note

Keep Save summary as one user action and one existing `SummaryRun`, with one
connection owner, cancellation token, epoch, target note, and commit flow.
Run the two model operations sequentially in its existing worker: short first,
then long. This needs no additional worker scheduler or parallel-run state.

1. Capture the source transcript once using the existing saved-message filter
   and `session_markdown()`. Prepare both definitions from the captured
   workspace before registering/launching the summary run.
2. Call `collect_note_prompt()` with the short definition and the full source
   transcript. Validate its result with the existing summary-answer checks.
3. Recheck cancellation, run ownership, and vault admission before starting
   the long operation. Call `collect_note_prompt()` with the long definition
   and that same source transcript. Validate its result separately. Do not
   include the short answer as input or chat history.
4. After both operations succeed, assemble one Markdown string in application
   code using the exact structure below, with one blank line around each body:

   ```markdown
   # Short summary

   <short operation's Markdown body>

   # Long summary

   <long operation's Markdown body>
   ```

5. Check the combined UTF-8 byte size against the existing 12 MiB note-document
   limit, including the application-supplied headings and separators. The
   individual answer limits are insufficient for the combined result.
6. Recheck the existing run/epoch state, mark the run ready, and return the
   combined Markdown in the existing summary reply. The client converts it
   with `summaryDocument()` and commits one note through the existing
   `sessionSummary.commit` operation. No new response shape is needed.

Use the existing retry policy independently for each model operation. A long
operation retry reuses its captured definition and transcript and does not
rerun a successful short operation. A small local helper may share the retry
loop; do not introduce a general workflow engine. Keep the short result only
in the worker's local memory while waiting for the long result.

If either operation fails, is refused, returns empty/incomplete output, or is
cancelled, retire the whole run through the existing failure path. Do not
start the long operation after a short failure. A long failure discards the
short result. Conversion or commit failures use the existing handling. No
partial summary note is saved, and an existing summary remains unchanged
unless the complete replacement commits successfully. A later user retry
starts both operations again with the then-current transcript and prompts.

Keep the existing start/completion events and busy state for the whole Save
summary action. Two successful model generations are required instead of one;
each receives the full transcript, so input-token use and total latency can
increase. Do not add a third model call to merge the results.

### Other operation behavior and output requirements

Preserve provider/model selection, reasoning settings, source selection,
and the existing cancellation, conversion, and persistence paths. Compression
and page/transcript formatting retain their existing model call count. The
summary split changes one generation into two as specified above; editing
prompt text does not itself add further calls.

Keep the existing page-formatting condition: YouTube transcripts use their
formatter, and ordinary articles use the page formatter when the existing
long-paragraph heuristic requires it. Articles that already bypass formatting
continue to do so. A prompt edit does not force formatting on every import.

Keep the existing output checks. In particular,
`webapp/src/notes/summaryImport.ts` requires two level-one headings, Short
summary followed by Long summary, with readable content in both sections.
CHA supplies these two headings; the model responses supply the bodies.
Edited summary prompts must therefore request section bodies, with any
subheadings at level two or deeper. Additional level-one headings or other
output that breaks the combined document follow the existing conversion
error path; do not strip or reinterpret arbitrary model content to repair it.
Page formatting retains its title handling, Markdown-to-note
conversion, and size checks. Invalid output follows the existing error path;
do not loosen parsers, add hidden corrective instructions, or validate natural
language prompt intent during Save.

Saving text alone makes no provider call. Do not add prompt selectors to
provider, Notes, character, forum, or session settings, preview execution,
or new Assistant tools. Operation-to-prompt mapping is fixed by the catalog
IDs above.

## 11. Implementation sequence

1. Add the fixed catalog, new-vault and existing-vault initialization, and
   resolved workspace values; test initial rows, open/reopen behavior, stored
   text, and compatibility fallback loading.
2. Add atomic text updates through the existing config editor/store; test
   persistence, vault isolation, and rollback.
3. Add the application operations, bridge methods, DTOs, and client calls.
4. Connect the five prompt selections to the captured workspace values,
   implement the two-operation summary worker and application-side assembly,
   and test request construction, capture timing, and failure handling.
5. Implement the proposed Settings list, detail view, and edit/upload dialog.
6. Run focused backend, protocol, operation, and frontend tests, then the normal web
   checks and native UI smoke test for the changed screens.

The wireframes in section 3 are the visual design for review. This document
itself changes no UI. Implementation must follow the repository's UI approval
rules; an explicit request to implement an approved completed design does not
require another design approval.

## 12. Validation and acceptance

### Workspace and persistence

- First-run setup and normal New vault creation persist exactly the five
  catalog prompt rows, including both new summary rows, with the current
  embedded resource texts before the
  vault becomes available. Normal creation uses resource text even if the
  active vault has edited prompts.
- Explicit vault copying preserves existing prompt text and fills missing
  prompt rows in the new copy. Creation failure does not publish a partially
  initialized vault.
- Opening an older vault with no `system/prompts/` entries inserts all five
  current resource texts before workspace publication. Verify startup, vault
  switching, and reopening after import/restore. Subsequent reads and model
  requests use the committed rows, and exports include all five files.
- Reopening an initialized vault performs no prompt writes and preserves
  edited, empty, and unedited stored texts. Unknown or obsolete entries count
  as an existing directory; similarly named prefixes such as
  `system/prompts-other/` do not.
- A partially populated prompt directory is not backfilled on open. Missing
  entries use defaults; saving one creates only that row. Removing all entries
  causes all five defaults to be persisted on the next main-process reopen.
- Inject initialization failures and verify atomic rollback, no partial
  publication, preservation of the original vault, and success on retry.
  Secondary-process reads must not initialize configuration.
- Start the application with a configured machine identity and active D1
  recording against an older vault without prompt rows. Verify startup
  commits all five rows and their `config.edit` record in the same transaction.
  An injected commit failure leaves both the prompt rows and the sync queue
  unchanged.
- Saving, reopening, and switching between two vaults preserves each vault's
  own text. Other prompts remain unchanged.
- Saving an empty string persists an empty value and does not select the
  default. Reopening or upgrading never replaces an existing value with a
  newer bundled default, whether or not the user has edited it.
- UTF-8 text, quotes, Markdown, template-like syntax, indentation, and trailing
  whitespace survive round trips, subject only to the specified BOM/newline
  normalization. Check multibyte text at the byte-size boundary.
- Unknown IDs cannot write arbitrary config paths or change catalog names.
  Extra stored paths cannot create visible prompt entries.
- Unknown or invalid imported prompt rows warn and preserve raw rows without
  blocking unrelated work. Invalid dedicated saves fail without mutation.
- With D1 recording active, import and merge a malformed UTF-8 prompt row
  alongside a valid change. Verify the operation commits, preserves the raw
  bytes locally, and resolves the bundled default for a recognized malformed
  row. Verify the queued record carries the valid change and omits the
  malformed row. Check that a warning names the omitted path.
- A legacy combined-summary row is preserved but supplies neither new summary
  prompt. It cannot add a sixth catalog entry.
- Inject validation, SQLite write, and commit failures using the existing
  store test seam; verify both durable rows and the published snapshot.
- Export and back up a newly created, unedited vault and verify that all five
  initial prompt texts are included and survive import/restore. Verify merge
  precedence using the existing transfer test support.
- Save changed prompt text on one machine and use the existing D1 test
  transport to replicate it to another machine's local copy of the same vault.
  After the receiver applies the record, verify that its SQLite `config` row
  at the catalog path contains the saved text and that `find_prompt(id)` on
  its published workspace snapshot exposes the same text.

### Contract and interface

- Protocol tests cover the three method names, epoch policy, exact arguments,
  unknown IDs, error mapping, and response shapes. Add wire fixtures.
- The Settings list contains all five prompts and has no create action.
  Detail has edit and upload actions, with no rename or delete action.
- Literal HTML and Markdown display as text and cannot execute or render.
- Edit → Cancel leaves stored text unchanged; Edit → Save updates it once.
- Upload → Cancel leaves stored text unchanged; upload opens the editor and
  sends no update request until Save. The filename never replaces the name.
- Invalid uploads keep the prior text. A cancelled picker changes nothing.
  Selecting the same file again works.
- Save failures retain drafts. Duplicate submissions are blocked. Refresh and
  vault-change tests cover stale drafts and late responses.
- Verify keyboard focus, Escape/Cancel, accessible icon names, long text
  scrolling, and the narrow desktop layout with the native host.
- Confirm that saving prompt text causes no provider call or live-session
  restart.

### Operation integration

- Give each of the five stored prompts distinct nonempty text. Use the
  existing fake provider/test transport to inspect the actual model request.
  Verify that each operation uses its corresponding text as the system prompt
  and the existing source content as the user message.
- Save a changed prompt, then start its operation without restarting CHA.
  Verify that the request uses the saved value. Repeat with another vault to
  verify that text does not leak across vaults.
- Hold an accepted compression job in its queue, or hold page retrieval before
  formatting. Save new prompt text, then release the job. It must use the
  captured old value; the next accepted operation must use the new value.
- Force a retry while editing the prompt and verify that the accepted
  operation retains the same text across attempts. For summaries, save new
  long-prompt text while the short call is blocked and verify that the long
  call still uses the value captured for that run.
- Verify that an empty stored value stays empty in Settings and in the
  operation's character definition. Inspect the actual request payload with
  an empty value. A ChatGPT subscription request uses the Responses adapter's
  existing fallback and an API key Responses request omits `instructions`.
  Verify missing and invalid rows use the same bundled default shown by
  Settings.
- Verify ordinary pages that bypass formatting still make no formatting model
  call, while eligible articles and YouTube transcripts select their own IDs.
- Preserve summary-format validation, page title handling, and existing result
  error paths. Invalid output must not cause a retry with bundled instructions.

### Split-summary integration

- Verify two model requests, short then long, with their distinct configured
  prompts, the same original transcript, and the same captured provider and
  reasoning settings. Neither request contains the other operation's result.
- Verify CHA supplies the exact two top-level headings, combines the two
  returned bodies in order, and sends one complete result through the existing
  conversion and commit flow, producing one summary note.
- A short failure prevents the long call. A long failure saves no partial note
  and leaves an existing summary intact. Cover cancellation between calls,
  vault changes, and failure of final conversion/commit.
- A retryable long-call failure retries only the long call. A new Save summary
  action after failure starts both calls with current prompts.
- Cover empty/refused/incomplete responses from either operation, combined
  size overflow even when each result is individually within its limit, and
  unexpected top-level headings that fail the existing document validator.
- Verify model bodies containing valid level-two subheadings and Markdown
  remain intact after assembly and conversion.

Use existing suites in `tests/workspace`, `tests/app`, `tests/bridge`,
`tests/runtime`, and `tests/notes/unit_notes_service.cpp`, plus focused tests
beside `PromptSettings.tsx` and the client.
Update shared text-file tests for the new allow-empty option without changing
their existing defaults. Run `npm run api-types`, `npm run check`, and the
relevant CTest cases after implementation. Live provider credentials are not
required for deterministic validation of storage, management, and operation
prompt selection.
