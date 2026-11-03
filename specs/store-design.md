# Store name and description during folder upload

Date: November 3, 2026

Status: **Ready for implementation; UI approved**. The user explicitly approved
the dialog preview and initial field rules in this chat on November 3, 2026,
then approved the review amendments: OpenAI's native description with a
512-character limit and Gemini store names with a 512-UTF-8-byte limit. This
document does not implement the feature. No design decisions remain pending.

## 1. Scope and decisions

Extend the existing desktop **Upload** dialog to collect a store name and a plain
text description, then save those values in the newly created provider store.
The existing folder selection, provider selection, saved API keys, note export,
operation worker, and cleanup remain the implementation path.

- **Name** is required and initially contains the source folder's name. The user
  can replace it. Save the submitted name without appending the folder Ref ID.
  OpenAI names permit at most 512 Unicode code points; Gemini names permit at
  most 512 UTF-8 bytes after the specified whitespace trimming.
- **Description** is optional, initially empty, and supports multiple paragraphs.
  Preserve its text, line breaks, punctuation, and Unicode characters. For
  OpenAI, the native field permits at most 512 Unicode code points; paragraph
  breaks count toward that limit. Gemini uses the chunked representation below.
- OpenAI receives its native vector-store `name` and `description` fields.
- Gemini receives the name as the store's native `displayName`, plus a generated
  `DESCRIPTION.txt` document containing the single ASCII character `x`. The
  document's custom metadata holds both the name and description.
- Creating the Gemini metadata document is part of a successful upload. A store
  cannot become ready with missing or failed metadata.
- Store these values remotely. Add no local database columns, metadata cache,
  saved form defaults, new settings, or background synchronization.

This stage does not add metadata reading, display of stored descriptions, editing
of existing store metadata, store discovery, or a model prompt/tool change.
Those uses will be designed later. Existing stores are not migrated or updated.
Do not edit the older documents in `docs/` as part of implementation.

## 2. Approved Upload dialog

Use `webapp/src/components/FolderStoreDialog.tsx` and its existing modal styling.
The same form is used for both providers. Preserve the existing provider labels
**OpenAI** and **Google**.

```text
  Provider
  [ OpenAI                                      v ]

  API key
  [ Personal OpenAI                             v ]

  Name
  [ Research                                      ]

  Description (optional)
  [ Research notes on language models, retrieval,  ]
  [ and application design.                       ]
  [                                              ]
  [ Includes experiments, technical decisions,    ]
  [ and reference material.                       ]
  [                                              ]

                                 Cancel   Upload
```

The visual preview shown in the approval chat is authoritative for this layout.
Its field labels, order, defaults, optionality, and buttons are approved.
There is no visible dialog title, panel legend, provider implementation detail,
description-format selector, character counter, or explanatory helper text.
The dialog retains the accessible name `Upload folder to store`.

### 2.1 Controls and validation

- Append **Name**, a single-line text input, after API key, followed by
  **Description (optional)**, a six-row plain-text textarea. Both use
  `cha-form-control`; give this textarea vertical padding and vertical resizing
  scoped to `cha-folder-store-dialog`. Keep the existing dialog width and form
  gaps. Preserve the native modal dialog's existing viewport constraints and
  scrolling; verify them on short screens without adding duplicate CSS rules.
  The `max-height` near `app.css:633` applies to the system-prompt/compressed-note
  dialogs, not `.cha-dialog` generally; it is not the source of this behavior.
  A review check using current app CSS in Chromium at 320 × 300 confirmed
  native `overflow: auto`, a viewport-derived max-height, and no offscreen edges.
- On opening a fresh dialog, set Name to `source.name` and Description to `''`.
  Do not read notes, call a model, or fetch remote metadata to populate either.
- Trim leading/trailing ASCII whitespace from Name on submission, consistently
  with native `trim_view`. Validate the normalized value. A name consisting only
  of that whitespace is invalid. For OpenAI, Name permits at most 512 Unicode
  code points; count code points, not JavaScript UTF-16 code units. For Google,
  Name permits at most **512 UTF-8 bytes**; count the bytes of the normalized
  string before JSON serialization or metadata splitting. In the frontend use
  `new TextEncoder().encode(normalizedName).length`; in native code use the
  validated UTF-8 string's `.size()`. Do not
  truncate a name or append an identifier. An unusually long folder name remains
  visible for correction rather than being silently shortened.
- Preserve Description exactly as held by the textarea, including leading and
  trailing whitespace. Browser textarea line-ending normalization is the input
  boundary; preserve the resulting string through the bridge and provider JSON.
  An empty string means no description. Do not impose an invented 256-character
  limit on the complete description: that Gemini limit applies to each encoded
  metadata string, not the whole field. Existing bridge/transport size limits
  still apply. Provider rejection of larger metadata fails the upload normally;
  arbitrary-length Gemini descriptions are not promised. **OpenAI Description
  has a separate 512-code-point limit**, confirmed by the live check in section
  10. Reject a longer value locally with
  `Description must be 512 characters or fewer for OpenAI.`; never truncate it.
- Upload is enabled only when the existing key/operation checks pass and Name is
  valid and Description meets the selected provider's rules. Keep backend
  validation authoritative as well. For an empty normalized Name, show
  `Enter a store name.`. For an overlong OpenAI Name, show
  `Name must be 512 characters or fewer.`; for an overlong Google Name, show
  `Name is too long for Google. Use a shorter name.`. Use the existing error presentation.
  Associate field validation with the input and set `aria-invalid` when shown.
  Show the same validation message immediately if the prefilled folder name is
  invalid: an initially disabled Upload button must have an explanation. For
  other untouched valid fields, show no validation error.
- Provider/key changes preserve Name and Description and retain the existing key
  selection logic. Do not reset metadata on an asynchronous preflight failure or
  a recoverable start error. Provider changes immediately revalidate both fields
  against the selected provider's rules. Switching to Google can invalidate a
  Name longer than 512 UTF-8 bytes; switching to OpenAI can invalidate a
  Description longer than 512 code points. Preserve the text and show the relevant
  error until the user edits it or switches back. Do not use `maxLength` to
  silently prevent/truncate input.
- Disable all four fields while submission is pending. Once the operation is
  active, retain the current progress/cancel presentation, which hides the form.
  Preserve current initial focus, tab order, Escape, Cancel, error recovery, and
  restoration of focus to the invoking control. Enter in Description inserts a
  newline; it does not submit the form.
- Closing and opening a fresh dialog resets the fields. Recovery of an existing
  operation follows its status; it does not fetch or reconstruct a metadata form.

## 3. Request and application flow

Extend the existing request; do not add a bridge method:

```json
{
  "folder_id": 11,
  "service": "openai",
  "api_key_id": "api_key_1",
  "name": "Research",
  "description": "First paragraph.\n\nSecond paragraph."
}
```

Both new keys are required strings in `notes.storeUpload`; Description may be
empty. The frontend and native bridge ship together, so missing fields are an
invalid request rather than an implicit legacy-name fallback. Unknown fields
continue to be rejected.

Pass the two strings through these existing entry points:

1. `NotesClient.storeUpload(folderId, service, apiKeyId, name, description, epoch)`
   in `webapp/src/api/client.ts`.
2. The `storeUpload` implementation in `webapp/src/api/nativeClient.ts`; retain
   its context-epoch validation and existing start-result guard.
3. `notes_store_upload` in `src/bridge/notes_dispatch.cpp`; extend the exact
   allowed-key set and require both string fields.
4. `Application::start_folder_store_upload` in `src/app/application.{h,cpp}` /
   `src/app/folder_store_operations.cpp` and the corresponding declaration and
   definition in `src/notes/notes_service.h` /
   `src/notes/folder_store_operations.cpp`.
5. Two request-owned strings, `store_name` and `store_description`, on
   `NotesService::Impl::FolderStoreControl` in
   `src/notes/notes_service_internal.h`.

Use the argument order `folder_id, service, api_key_id, name, description, epoch`
on both native facade and NotesService. Normalize and validate the name in
NotesService before registering an operation, reserving a folder, or making a
network request. Direct native callers therefore receive the same rules. Check
valid UTF-8 before code-point counting/splitting; invalid input is an
`invalid_argument`, not a serializer failure after allocating a remote store.
For Google, enforce the 512-UTF-8-byte Name limit on the normalized string at this
same admission point; for OpenAI, enforce the 512-code-point Name limit. Return
the same provider-specific validation message used by the frontend.

Also validate the selected provider's description limit at this admission point:
OpenAI accepts at most 512 Unicode code points. The frontend and native checks
use the same limit, count line breaks, and reject rather than truncate.

Set the strings once at admission; a later folder rename must not overwrite the
chosen remote name. Keep `control->folder_name` as the folder identity used by
existing local progress/events. It is distinct from the chosen remote name.

No returned DTO changes are needed: choices, start result, operation status, and
note-list store summaries retain their shapes. `resources/dto.yaml` currently
does not define the upload request; do not add or regenerate response types just
to carry the new input strings. Update the existing bridge/native-client tests
and test fixtures that call this method.

## 4. OpenAI encoding

Extend `FolderStoreClient::openai_create_store` in
`src/providers/folder_store_client.{h,cpp}` to accept Description in addition to
Name. Send the supplied name in `POST /vector_stores`; include native
`description` when it is nonempty. For an empty description, omit that field.
Do not use vector-store custom metadata for these values and do not create a
special description file for OpenAI.

Replace the current generated name (`folder name + Ref ID`) with the admitted
`store_name` in `run_folder_store_upload`. Preserve allocation uncertainty,
remote-ID persistence, file ownership, note attachment, and indexing behavior.
Do not silently drop Description if a request is rejected.

The pre-implementation live check in section 10 resolves the missing response
schema field: create and GET both returned the submitted native Description
exactly at 512 characters. A 2,004-character, three-paragraph value was rejected
with an explicit maximum of 512. Keep the native field as the user selected
during review; do not switch to custom metadata or save a truncated description.
Validate that limit before allocation and include native GET read-back in the
implementation smoke test rather than checking request acceptance alone.

The published update request documents `name`, `metadata`, and `expires_after`,
but not `description`. A separate live POST to update the temporary store's
Description nevertheless succeeded, and GET returned the changed value exactly.
Thus the review's claim that changing it necessarily requires a replacement
store is not supported by the observed API behavior. This stage adds no edit
operation and must not assume a documented description-update contract. Any
future editing design must distinguish observed behavior from
the documented API contract. The earlier chat's blanket assurance about all
description APIs was broader than the documentation establishes.

## 5. Gemini encoding

### 5.1 Remote representation

Create the store through the current `google_create_store` call, using the exact
normalized Name as `displayName`. Its generated resource name remains the ID.
Persist that resource name before uploading any documents, as today.

Upload one metadata document into that store using the existing resumable upload
protocol:

| Property | Value |
| --- | --- |
| Document display name | `DESCRIPTION.txt` |
| MIME type | `text/plain` |
| File body | `x` (one byte; no newline or BOM) |
| Custom metadata key `name` | `stringListValue.values`: ordered pieces of Name |
| Custom metadata key `description` | `stringListValue.values`: ordered pieces of Description; omit this entry when Description is empty |

Use `stringListValue` consistently, including names and short descriptions. This
avoids a reader having to guess between two encodings. Concatenating the ordered
pieces with **no separator** reconstructs each value exactly. A missing
`description` entry means an empty description. At most two custom metadata
entries are used; the list elements are not separate custom metadata entries.

This is a convention for CHA-created documents, not native store metadata.
`DESCRIPTION.txt` is the reserved display name for this generated document. Real
notes retain their current generated `.md` filenames containing their note IDs,
so a local note named DESCRIPTION does not collide with it. Do not create a
DESCRIPTION note in the vault or write a temporary physical file: the existing
transport accepts an in-memory body.

The name is present in both the native store display name and document metadata
as requested. There is no reconciliation process if someone changes the remote
store outside CHA.

### 5.2 String splitting

Implement one small provider-local helper to encode a string as ordered list
elements. Reuse `utf8_prefix` from `src/util/text.h` with a **200-byte** budget:
take a prefix, append it, then advance by the prefix's byte length until no text
remains. The input is validated UTF-8; each iteration advances. No reordering,
trimming, ellipsis, added separators, escaping before splitting, or normalization
is permitted. JSON serialization performs its ordinary escaping afterwards.

The byte budget is deliberate. The live test rejected a 256-code-point list
element containing Unicode, while shorter elements succeeded. A maximum of 200
UTF-8 bytes per element conservatively satisfies byte, code-point, and UTF-16
interpretations of the provider's per-string limit. It does not set a maximum on
the overall description, and it does not split a UTF-8 code point.

Google's documentation does not establish an arbitrary maximum list length.
Preserve all the pieces and let an actual provider limit produce an ordinary
upload failure; never discard trailing pieces or report success with truncated
metadata. The existing live check verified six list entries, not an unbounded
list. No configuration option for chunk size is needed.

### 5.3 Provider client changes

Reuse `google_upload_document` instead of implementing a second uploader. Extend
it with trailing optional MIME-type and custom-metadata arguments, defaulting to
the existing `text/markdown` and no metadata. A small provider-local encoding
helper builds the two metadata entries; expose one narrow
`google_upload_store_metadata(store, name, description, key, cancel, cancel_forward)`
wrapper that calls the shared upload method with `DESCRIPTION.txt` and `x`.

Use the supplied MIME type consistently in resumable-start headers and the
finalize request. Attach `customMetadata` to the resumable-start JSON beside
`displayName`. Include `mimeType: text/plain` for the generated document, matching
the successful live test. The existing note path retains its request bodies and
headers apart from the shared method's parameter plumbing.

Keep the upload-host validation, authentication headers, sanitized errors,
operation-name validation, and the distinction between lifecycle cancellation
and user cancellation. Never abort an already-sent finalize solely because the
user clicked Cancel: retain its operation result and use existing cleanup.

## 6. Worker, progress, and readiness

Use the existing sequential upload worker and joint indexing loop:

1. Capture/validate the saved note snapshot as today. A folder without eligible
   notes still fails preflight; the generated document does not make an empty
   folder eligible for upload.
2. Register the local attempt with its real-note count, then create the remote
   store and persist its ID using the current guarded allocation path.
3. For Gemini, upload the generated metadata document first and add its returned
   operation to the same in-memory `WorkItem` vector used for note indexing.
   Represent it with a synthetic `FolderStoreDocument` whose `note_id` is zero,
   `note_name` is `Store metadata`. Leave its filename/body empty: the provider
   wrapper already supplied them; the queued item only needs indexing context.
   Real note IDs are positive; use that existing distinction when counting notes.
   Do not insert this synthetic document into the captured note collection.
4. Upload the actual captured notes through their existing provider paths.
5. Poll every pending item, including metadata, with the existing indexing loop.
   The Gemini store must have exactly `items.size()` active documents, none
   pending, and none failed before success. Here `items.size()` is real notes
   plus one for a new Gemini store. Preserve the existing mismatch checks.
6. Publish ready only after all items and the final counts check succeed and the
   current cancellation/epoch/ownership checks still pass.

`total`, the persisted `note_count`, progress `completed`, and the success event
continue to count only real notes. The metadata upload occurs before the real
note upload loop, so it never enters that loop's completion increment. Exclude
the synthetic zero-ID item from the indexing `count_if` and subsequent indexing
completion increments. Gemini's remote document count is intentionally one higher. While
metadata uploads, the existing status can show `Uploading 0 of N notes…`; no new
phase, status label, or progress DTO is needed.

Log the returned Gemini metadata operation name using the existing operation-ID
logging and lifetime checks, just as for a note. Keep its ID in worker memory;
the known containing store already supplies cleanup ownership. Add no persistent
document ID, note/file index, or additional allocation-uncertainty flag.

## 7. Failure, cancellation, and cleanup

Treat a rejected metadata upload, failed operation, or final document-count
mismatch as an upload failure. Use the existing failure path, including cleanup
of the newly allocated store. Do not mark notes ready while omitting metadata,
retry a rejected request with metadata removed, or automatically recreate a store.

Reuse the current `google_delete_store` implementation: it finishes a paginated
document listing, force-deletes each listed document, and only then force-deletes
the store. The listing automatically includes DESCRIPTION.txt. Existing code
does this because a force-deleted store has been observed to leave documents
readable; do not replace it with a single store DELETE or add a second cleanup
path for metadata. No new document-deletion API is required.
Cleanup failure keeps the existing incomplete record and manual-cleanup details.
Cancellation during metadata work follows the same guarded worker cleanup as
cancellation during note work. Vault changes, shutdown, and lost allocation
responses retain their existing handling.

Backend name validation fails before remote allocation. Descriptions are user
content: do not log their text, include it in Events, add it to operation-status
responses, or echo a provider's raw body. Reuse the existing safe error handling.
Report metadata failures with `Store metadata` as the resource context. Change
`require_result`'s optional second argument from a bare note name to a complete
context label: the helper appends `: ` to a nonempty label but adds no `Note` or
quotation marks itself. Existing real-note callers pass `Note “<note name>”`;
metadata upload and metadata polling pass `Store metadata`. Thus the error is
`Store metadata: …`, never `Note “Store metadata”: …`.

When store creation or the metadata upload returns
`FolderStoreRemoteKind::rejected`, use the fixed safe message
`The provider rejected the store name or description.` in the worker before
`require_result` reports it. Preserve any returned IDs and allocation-uncertainty
bookkeeping first. Do not replace specific authentication, quota, network, or
cancellation errors with this message.

Google reports a wrong or expired API key as HTTP 400 `INVALID_ARGUMENT` with an
`ErrorInfo` detail whose reason is `API_KEY_INVALID`. Provider classification
treats that response as `failed` with the existing message
`The saved API key was rejected.`, the same as a 401 response. It is therefore
not `rejected`, and the fixed store message does not replace it. This applies to
every folder store request, including note uploads, cleanup, and search. Other
400 responses keep their current classification. Actual note uploads retain
their current error reasons and note context.

**Known form-recovery limit:** keep the approved dialog's existing terminal-error
flow (the review's option 2). A remote failure, even after successful cleanup,
leaves the terminal error and Close button rather than returning to the form.
The in-memory draft remains while that dialog is mounted, but closing and
reopening initializes Name from the folder and clears Description. A preflight
or recoverable start error still preserves the mounted form as section 2 says.
This includes store-name/description rejection before any notes are uploaded.
No persistent draft, new retry control, automatic acknowledgement/retry, or
changed recovery UI is part of this stage.

For an unconfirmed store allocation during the current upload, include the
chosen remote `store_name` in the existing warning event/log as well as folder
name/ID, service, and saved key ID. Pass the already-admitted value from the
control to `unknown_warning` as optional context; do not change the persisted
record or duplicate remote-ID ownership. Quote/escape the name as one diagnostic
value and never include Description. Existing remove/delete paths lacking that
value keep their current warnings. This helps manual provider-console lookup
when the create response is lost, but does not promise uniqueness or recovery
of a name lost to an abrupt process exit before it could be logged.

Removing an old Gemini store still works: removal does not need to find a
DESCRIPTION document. Existing OpenAI file cleanup remains necessary and
unchanged. No automatic reads or writes occur merely on opening a vault.

## 8. Local state and future compatibility

The frontend form and admitted operation strings are the only new local state.
Remote stores remain associated with their existing local folder/store IDs and
saved key IDs. A local folder rename does not rename an existing remote store.
Removing and uploading again starts a fresh form and creates new remote metadata.

For a future reader of the Gemini convention, a document can be located by
listing documents and matching `displayName == DESCRIPTION.txt`, then retrieving
that document's metadata. Listing is paginated, and display names are not unique
identifiers; do not infer a document resource ID from its filename. The two keys
and ordered concatenation defined here are sufficient to decode a CHA-created
metadata document. Handling missing, duplicate, or externally modified documents
belongs to the later usage design, not a new subsystem in this stage.

The generated document lives in the searchable store. This stage adds no search
filter or prompt instruction to hide it. Its body is deliberately one character;
provider-returned metadata may still appear in retrieval/citation data. Do not
claim that it is invisible to search or that its description is automatically
supplied to the model as a store-level instruction.

This deliberately retains the scope decision made before review. In the current
Gemini `search_stores` path, `DESCRIPTION.txt: x` can occupy one of the five local
match slots, reducing space for real note results. Filtering by title would be
small, but it changes the existing search behavior; metadata usage and search
treatment remain deferred to the following stage. The tradeoff is accepted for
this stage, not an overlooked assumption that the chunk is empty.

## 9. Implementation files and validation

| Area | Files and checks |
| --- | --- |
| Dialog | `webapp/src/components/FolderStoreDialog.tsx`, scoped styles in `webapp/src/styles/app.css`, `FolderStoreDialog.test.tsx`: defaults, field order/labels, multiline input, name validation, provider changes, exact submission, disabled/pending state, preflight recovery, cancel, and existing removal/deletion modes. |
| Client/bridge | `webapp/src/api/client.ts`, `nativeClient.ts`, `nativeClient.test.ts`, frontend fixture implementations/callers, `src/bridge/notes_dispatch.cpp`, `tests/bridge/unit_bridge_router.cpp`: both string fields are required, empty description is accepted, invalid types/missing keys/unknown keys fail, and stale epochs still fail. |
| Admission and worker | `src/app/application.h`, `src/app/folder_store_operations.cpp`, `src/notes/notes_service{,_internal}.h`, `src/notes/folder_store_operations.cpp`, `tests/app/unit_folder_store_operations.cpp`: chosen name survives folder rename; no remote allocation for invalid names; empty-note preflight remains; all-note-plus-metadata readiness and cleanup behavior. |
| Provider encoding | `src/providers/folder_store_client.{h,cpp}`, `tests/providers/unit_folder_store_client.cpp`: OpenAI name/description, empty description omission, Gemini native displayName, exact one-byte body, MIME headers, both metadata keys, upload URL/operation validation, and existing note upload compatibility. |

Add focused cases to the existing suites rather than a separate metadata test
framework. In particular verify:

- Chunking at 199/200/201-byte boundaries, code points crossing a boundary,
  accented/CJK text, emoji, blank lines, and exact reconstruction by concatenation.
- A multiline description substantially longer than 256 characters is preserved
  in outgoing Gemini metadata. Both short and long descriptions use list values.
- An OpenAI name of 512 code points is accepted locally; 513 fails before
  allocation. A Google name of 512 UTF-8 bytes is accepted locally; 513 bytes
  fails before allocation. Cover ASCII, 256 Cyrillic letters, 128 four-byte emoji,
  and mixtures with a one-byte character to exercise the exact boundary. Include
  a name whose raw bytes exceed 512 only because of surrounding ASCII whitespace:
  validate its trimmed value. Empty normalized names fail; text is not truncated.
- A prefilled invalid Name displays the approved validation error immediately.
  OpenAI descriptions at 512 code points, including Unicode and paragraph breaks,
  are accepted locally; 513 fails before allocation. Switching provider preserves
  text and revalidates the provider-specific constraints. In particular, 300
  Cyrillic letters are within the OpenAI local name limit but exceed Google's
  byte limit: switching to Google disables Upload and shows its approved error;
  switching back clears that validation error without changing Name.
- With two real Gemini notes, the provider expects three documents; local counts
  and the success message still say two notes. Metadata pending/failed, one remote
  document missing, or an unexpected extra active document prevents ready state.
- Metadata-start/finalize/poll failure and cancellation clean the known store;
  a cleanup failure remains actionable through the existing incomplete-store path.
- OpenAI creates no metadata document. Old-store removal needs no special file.
- Rejected create/metadata requests use the fixed store-specific error; other
  errors retain their safe reason. Metadata polling reports `Store metadata`,
  while real notes retain `Note “…”`. Unknown-allocation warnings during upload
  include the chosen remote name and never Description.
- A remote error after successful cleanup retains the terminal error/Close
  flow. Reopening defaults Name to the folder name and clears Description;
  preflight recovery still preserves both fields in the mounted form.
- No database migration or response-schema changes appear in the final diff.

During implementation, run the affected frontend suites and typecheck, plus the
provider, app-operation, and bridge C++ suites using the existing `cha_tests`,
`cha_app_tests`, and `cha_bridge_tests` targets. Check the dialog visually in both
themes and on a short/narrow viewport, including focus and textarea behavior.
Build the affected native/frontend targets; do not run live API checks in CI.

Finish implementation with a manual live smoke test for each provider using
throwaway stores and the user's authorized credentials: create an OpenAI store
with a multiline native description within 512 code points, GET it, and compare
both fields exactly. A value above the limit must fail local admission before
any create request. Use a direct Gemini document GET to compare reconstructed
metadata with the submitted name/description. Include long Cyrillic/emoji store
names: 256 Cyrillic letters or 128 four-byte emoji must pass Google's local
512-byte check and round-trip exactly; 300 Cyrillic letters must fail before a
Google create request. Check the provider-specific validation against the
observations below.
These regression checks validate the implementation; they do not defer the
storage-format decision until after implementation. Delete
only the test-created resources, including separately uploaded OpenAI files, and
confirm cleanup. Do not turn these checks into a shipped metadata-read feature.

## 10. Evidence and approval record

Official references consulted for this design:

- [OpenAI vector-store creation](https://developers.openai.com/api/reference/resources/vector_stores/methods/create), [retrieval](https://developers.openai.com/api/reference/resources/vector_stores/methods/retrieve), and [update](https://developers.openai.com/api/reference/resources/vector_stores/methods/update): the create input documents Description, but the shown response schemas and update input omit it. Live results below resolve the behavior tested here, rather than inferring its absence from those schemas.
- [Gemini File Search store API](https://ai.google.dev/api/file-search/file-search-stores?hl=en): displayName and direct upload with customMetadata.
- [Gemini File Search document API](https://ai.google.dev/api/file-search/documents?hl=en): document customMetadata, string-list values, get/list/delete operations, and 20 metadata entries per document.

Live Gemini checks completed in this chat on November 3, 2026:

| Check | Observed result |
| --- | --- |
| Zero-byte text file with a valid 256-character metadata string | HTTP 400 `INVALID_ARGUMENT`. |
| One-character DESCRIPTION.txt with that string | Active document; exact metadata returned by document GET. |
| Three-paragraph description as one metadata string | Rejected with a 256-character string-value limit. |
| Description split at 256 code points, containing Unicode | A list element was rejected for exceeding the string-value limit. |
| One-character DESCRIPTION.txt with three paragraphs in six shorter string-list entries | Active document; all 1,175 characters returned exactly, including Unicode and paragraph breaks. |
| Test-resource cleanup | All temporary stores were deleted and confirmed absent. |

The evidence verifies the representation; it is not a published guarantee of an
unbounded metadata value or of every possible Unicode boundary. The byte-safe
splitter and its unit cases address the latter in the implementation.

Additional live checks performed during document review, before implementation:

| Check | Observed result and decision |
| --- | --- |
| OpenAI create with a 2,004-character, three-paragraph native Description | HTTP 400, `string_above_max_length`; the error explicitly states a maximum of 512 characters. |
| OpenAI create and GET with a 512-character ASCII Description | HTTP 200 for both; Name and Description returned exactly. |
| OpenAI create and GET with a 512-code-point Description containing Cyrillic, CJK, emoji, and paragraph breaks | HTTP 200 for both; exact Description returned. This distinguishes the limit from a UTF-8-byte limit. |
| OpenAI POST update with a new Description, then GET | Both succeeded, and GET returned the new Description exactly, despite its omission from the published update input schema. No shipped edit operation is added. |
| OpenAI metadata alternative with the 2,004-character Description split over five values | Create/GET succeeded with exact reconstruction. Not adopted: the user selected the native Description field with its 512-character limit. |
| Gemini displayName containing 300 or 512 Cyrillic letters, or 512 emoji | HTTP 400 despite each being within 512 code points. The respective UTF-8 sizes were 600, 1,024, and 2,048 bytes. |
| Gemini displayName containing 256 Cyrillic letters or 128 emoji | Create/GET succeeded with exact names; both names contain 512 UTF-8 bytes. These boundary checks support a byte-based limit for the tested names, rather than the spec's original code-point-only rule. |
| Cleanup of all resources created during review | Each temporary store was deleted, then GET confirmed HTTP 404. No files or notes were uploaded for these additional checks. |

UI approval: **approved on November 3, 2026**. The user stated:
"UI is Approved. Update the document."

The approval covers the preview and layout in section 2, including:

- Provider and API key followed by Name and Description, with Cancel and Upload
  buttons and no visible dialog title.
- Name required, prefilled from the folder name, and editable. The final limits
  are 512 Unicode code points for OpenAI and 512 UTF-8 bytes for Google.
- Description optional, initially empty, and supporting multiple paragraphs.

The layout remains approved. During review the user additionally selected
**Keep native description; enforce 512 characters** for OpenAI. This is a
provider-specific validation amendment, not a switch to the tested metadata
alternative. The user then explicitly instructed:
"Use the 512 UTF-8 byte limit for Gemini store names. Update specs/store-design.md
accordingly and mark the design ready for implementation."
This approves Google's byte-based Name validation and its specified error text;
all provider-specific validation decisions are settled.
Implementation of the completed, approved design needs no further UI/design
approval. Any genuinely new scope must be assessed separately.

## 11. Review dispositions

| Finding | Assessment and disposition |
| --- | --- |
| OpenAI read-back was deferred | Valid. Tested before implementation; sections 4, 9, and 10 now record exact GET read-back, the 512-character cap, and actual update behavior. The claim that Description cannot be updated was contradicted by the temporary-store test. |
| Remote rejection loses typed metadata; messages blame notes | Valid. Specify safe store-specific rejection wording. Keep existing terminal-error recovery and explicitly accept the review's option 2 as a known limit in section 7; do not expand the approved recovery flow. Name resets to the folder default, rather than necessarily becoming empty. |
| `require_result` adds an unwanted Note prefix | Valid. Pass complete context labels; update note callers and distinguish metadata during polling. |
| Synthetic upload increment / duplicated filename and body | Valid simplifications. Metadata is uploaded before the note loop; only indexing counters need exclusion. The queued synthetic item need not duplicate the wrapper's filename/body. |
| Chosen name is absent from unconfirmed-allocation warnings | Valid. Add the in-memory chosen name to the current upload's existing diagnostic; retain the approved exact remote name without a Ref ID suffix and add no persistent state. |
| DESCRIPTION.txt can occupy a search result slot | Factually valid and already accepted as part of deferring metadata usage/search changes. Section 8 now states the concrete five-slot consequence; no title filter is added in this stage. |
| Non-ASCII Gemini name boundary; unexplained invalid default | Valid. Live probes verified the code-point-only rule is insufficient for Gemini. The user approved a 512-UTF-8-byte Gemini Name limit; enforce it in the frontend and native admission, with immediate validation of invalid defaults and provider changes. |
| Force-delete cleanup description differs from current code | Valid. Section 7 now preserves the existing paginated list / document deletes / store delete sequence. |
| Existing `.cha-dialog` CSS already sets scrolling | The cited selector claim is incorrect: those app.css rules apply to other dialogs. Preserve existing native modal behavior and verify short viewports; add no duplicate scrolling rule without a demonstrated need. |
