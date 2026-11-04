# Cloud stores in Settings

Status — Technical design for review.

## 1. Scope

Add **Cloud stores** to Settings. A user selects a saved API key, selects a
remote store, then views or deletes it. The provider is the source of the store
list. Show every store accessible to the selected key, including stores created
outside CHA. Two keys with access to the same project can show the same store.
Use the current vault's saved keys. Keep store data in the active view only.

## 2. API key setting

This section is implemented. The new-key and existing-key forms have a
**Cloud store** selector. It uses the strings defined in the backend DTO.

| Display label | `cloud_store` value |
| --- | --- |
| None | `none` |
| OpenAI | `openai` |
| Gemini | `google` |

When the property is absent from saved key configuration, use `none`. There are
no deployed keys with this property, so no boolean migration is needed. If a
saved value is an unsupported string, warn and use `none`. Keep the credential
available for its other uses. Validate the enum on create and update
requests. A user can change this setting without entering the secret again.
Renaming a key preserves its setting.

The generated TypeScript schema, client validation, and Settings forms use
these strings. The folder upload flow uses them too. Each key with a configured
provider and a saved value yields one upload choice for that provider. Reject
`none` or a provider mismatch before starting an upload. A missing key fails
during upload preflight. Upload preflight also refuses a name that a store of
the key already has. Failed or cancelled upload cleanup still uses the
provider recorded for that upload. [Key DTO](../resources/dto.yaml) ·
[Folder upload admission](../src/notes/folder_store_operations.cpp)

## 3. Settings navigation

Add a **Cloud stores** row below **API Keys**. Reuse the Settings rows, back
controls, and confirmation dialog. The text at the top of each example is the
back control. Store fields are read-only. Render a blank value for a missing
description and preserve line breaks in a description that exists. Show the
creation date and time in the user's local time.

```text
Key list                 Store list               Store details
┌────────────────────┐   ┌────────────────────┐   ┌────────────────────┐
│ ‹ Settings         │   │ ‹ Cloud stores     │   │ ‹ OpenAI work      │
│                    │   │                    │   │                    │
│ OpenAI work      › │   │ Research         › │   │ Name               │
│ Gemini personal  › │   │ Archive          › │   │ Research           │
│                    │   │                    │   │                    │
│                    │   │                    │   │ Description        │
│                    │   │                    │   │ Project notes      │
│                    │   │                    │   │                    │
│                    │   │                    │   │ Created            │
│                    │   │                    │   │ Nov 3, 2026 9:15 AM│
│                    │   │                    │   │                    │
│                    │   │                    │   │ [Delete]           │
└────────────────────┘   └────────────────────┘   └────────────────────┘
```

- Load the key list through the existing API key list method. Show keys set to
  OpenAI or Gemini by their saved display names.
- Load stores when the user selects a key. Use the provider's display name and
  fall back to the resource ID when the name is empty. Append the full store
  ID in parentheses to each named row whose name matches another row's label in
  the selected key's list. Other rows keep the plain name. Gemini labels keep
  the `fileSearchStores/` prefix. Select by saved key ID and remote store ID.
- Use the existing loading, empty, and retry states. A failed provider request
  shows an error. Show **No cloud store keys** for an empty key list and
  **No cloud stores** for an empty store list.
- **Delete** opens **Delete store?** with **Cancel** and **Delete store**. For
  Gemini, the message is "Permanently delete “<label>” and its files?". For
  OpenAI, the message adds "Files shared with other vector stores are removed
  from them too." Use the same label as the list, including the ID when names
  collide. Keep the confirmation open while deletion is pending. **Delete
  store** is disabled and reads **Deleting…**. **Cancel** and Escape stay
  available. They stop the deletion, close the dialog, and show "Deletion
  stopped. Some files may already be deleted." on the details screen. Add a
  `pending` property to the shared `ConfirmDialog` that disables its confirm
  button; Cancel and Escape still call `onCancel`. On success, return to the
  selected key's store list and fetch it again. On a `not_found` result, also
  return to the store list and fetch it again, and show the result's message
  there. On any other failure, stay on the details screen and show the error
  with a way to retry through the confirmation. If only the list refresh fails,
  retry the list load from the list screen.

```text
Archive               ›
Research (vs_abc123)  ›
Research (vs_def456)  ›

Delete store?
Permanently delete “Research (vs_def456)” and its files?
Files shared with other vector stores are removed from them too.
[Cancel]  [Delete store]
```

## 4. Bridge and provider client

Add the Settings screens through
[Settings.tsx](../webapp/src/components/Settings.tsx),
[App.tsx](../webapp/src/components/App.tsx), and the navigation reducer in
[view.ts](../webapp/src/state/view.ts). Add these methods to `ChaClient` and
the native bridge. Define the shared result shapes in `resources/dto.yaml` and
regenerate the frontend types. The existing context epoch travels in the
request envelope.

| Method | Parameters | Result |
| --- | --- | --- |
| `cloudStore.list` | `api_key_id` | `{ items }`, each with `id` and `name` |
| `cloudStore.get` | `api_key_id`, `store_id` | `id`, `name`, `description`, `created_at` |
| `cloudStore.delete` | `api_key_id`, `store_id` | Empty success object |
| `cloudStore.cancel` | `request_id` of the pending delete | Empty success object |

Use a string for `description`, with `""` for a missing description, and an
RFC 3339 UTC string for `created_at`. Use the provider's opaque ID for `id`.
Gemini IDs include the `fileSearchStores/` prefix.

Resolve the saved key and its provider in the application under the active
vault context. Reject an unknown key, a missing credential, or `none`. Validate
the store ID against that provider's resource format before constructing a URL.
Use the existing `FolderStoreClient` transport, fixed provider origins, and
authentication headers. Keep credentials in native code. Return safe errors
without provider response bodies or credential values.

For `cloudStore.list`, extend the provider client's `list_store_names`, which
the upload name check already uses, and rename it to `list_stores`. It returns
the ID and the name of every store, with an empty name for a store that has
none, and keeps its paging checks. The upload name check compares only the
names.

Run remote requests through the existing asynchronous bridge reply path and
`BackgroundJobs`. Capture the selected credential for the request before
releasing the vault lock. Check cancellation and the context epoch before
further provider calls. Ignore replies for an inactive context or a selection
the user has left. Call `require_main()` before deletion, as Settings edits and
folder upload do. Add `cloudStore.delete` to the bridge's existing
`deadline_exempt` switch so the normal command deadline cannot expire while the
worker is still deleting files. Keep the dialog busy until the worker's reply
for the active context or until the user cancels. The provider transport still
applies its per-request timeouts.

Cancel follows `speech.cancel`. The web client passes an `AbortSignal` with
`cancelMethod: 'cloudStore.cancel'`, and the bridge lists `cloudStore.cancel`
in `is_control_method`. The bridge finds the pending delete by `request_id` and
abandons its `OperationReply`, as it does when a connection closes. Add a
public `abandoned()` query to `OperationReply`. The delete worker checks it
with the context epoch before each provider call, so a request already sent
finishes and no further request starts. The dialog ignores the abandoned
delete's reply.
[Bridge background replies](../src/bridge/bridge_router.cpp) ·
[Provider client](../src/providers/folder_store_client.cpp)

### OpenAI

- List with `GET /v1/vector_stores`. Follow `has_more` with `after=last_id`.
  Read the selected store with `GET /v1/vector_stores/{id}`. Use its native
  `name`, `description`, and `created_at`. Convert Unix seconds to UTC.
  [Vector store list](https://developers.openai.com/api/reference/resources/vector_stores/methods/list) ·
  [Vector store retrieval](https://developers.openai.com/api/reference/resources/vector_stores/methods/retrieve)
- For deletion, finish paging through `GET /v1/vector_stores/{id}/files` before
  changing any resource. Use the returned file IDs with the existing
  `openai_delete_file` method. After all listed Files resources are deleted or
  confirmed absent, call `openai_delete_store`. Deleting a File resource can
  remove it from other vector stores because OpenAI permits shared files. CHA
  uploads a separate File resource for each store. Treat every file listed on
  the selected store as part of the confirmed deletion.
  [List vector store files](https://developers.openai.com/api/reference/resources/vector_stores/subresources/files/methods/list) ·
  [Delete a File resource](https://developers.openai.com/api/reference/resources/files/methods/delete)
- If the file listing returns a confirmed not-found response on its first page,
  return `not_found` with "Store unavailable. File cleanup could not be
  verified." This also applies when retrying after a lost store-delete reply.
  A not-found response on a later page is an enumeration failure. A confirmed
  not-found response to the final store delete counts as success after this
  request has completed enumeration and deleted or confirmed the absence of
  every listed file.

### Gemini

- List with `GET /v1beta/fileSearchStores` and follow `nextPageToken`. Read the
  selected store with `GET /v1beta/{store_id}`. Use `displayName` and
  `createTime`. [File Search stores](https://ai.google.dev/api/file-search/file-search-stores)
- Read the description only when the user opens the details screen. Page
  through the store's documents for the reserved display name `DESCRIPTION.txt`.
  The document list returns each document's `customMetadata`, so no further
  request is needed. A CHA metadata document has a `name` entry with a string
  list. If exactly one candidate matches, concatenate the strings in its
  `description` entry's `stringListValue.values` in order. Return `""` if there
  is no unique match or the description is absent or malformed. A provider
  request failure is a load error. The store's native `displayName` supplies
  its name.
  [CHA metadata format: `google_upload_store_metadata`](../src/providers/folder_store_client.cpp) ·
  [Gemini documents](https://ai.google.dev/api/file-search/documents)
- Reuse the `google_delete_store` sequence. List all documents before deleting
  them, then delete each document with `force=true`, including
  `DESCRIPTION.txt`. Use `force=false` for the final store delete in Settings
  and `force=true` for failed or cancelled upload cleanup. If the final
  Settings delete returns `FAILED_PRECONDITION` because documents remain,
  show "The store still contains documents. Retry deletion."
- List Gemini documents even if the store may already be gone. Accept a
  confirmed not-found response on the first document page as an absent
  collection and proceed to the store delete. A confirmed not-found response
  to the final store delete counts as success once all listed documents are
  deleted or confirmed absent.
  [Existing Gemini deletion](../src/providers/folder_store_client.cpp) ·
  [Gemini store deletion](https://ai.google.dev/api/file-search/file-search-stores#method:-filesearchstores.delete)

### Deletion failures and retry

Reject invalid pages, repeated cursors, and a missing cursor when the provider
indicates more results. Finish these checks before deleting any files. An absent
child during deletion counts as complete. If any child deletion fails, stop
before deleting the store. Some children may already be gone. Every retry starts
with a fresh child listing, including after a failed final store delete. Use the
provider-specific rules above for an absent store. Authentication, permission,
transport, and invalid-response errors are failures. Keep only request-local
lists of resource IDs.

## 5. Validation

- Verify key config with an absent property, all three enum values, and an
  unsupported string. Verify create, provider-only edit, rename, and regenerated
  frontend types. Verify that a mismatched key cannot start a folder upload.
- Check Settings navigation with two providers, an empty description, a
  multiline description, an empty list, and a provider error. Verify that only
  rows with duplicate names show their IDs in the list and that the
  confirmation uses the selected row's label and sends its ID. Check cancel,
  success with a refreshed list, partial failure with retry, a `not_found`
  result that returns to the refreshed list with its message, and stale replies
  after navigation or a vault switch.
- Delay a delete beyond the normal bridge command deadline. Verify that the
  dialog stays busy, sends one delete, and handles the worker's final result.
  Press Cancel and Escape during a delete with several files. Verify that the
  dialog closes, the details screen shows the stopped message, the worker
  starts no provider call after the current one, and a later Delete finishes
  the remaining files.
  Switch vaults during deletion and verify that the worker issues no further
  provider calls after it detects the context change.
- Use the existing provider transport tests to verify pagination, metadata
  reading, and delete order for both providers. Cover invalid cursors, a child
  deletion failure, an absent child, and an authorization failure. Verify that
  a failed enumeration causes zero deletes and a failed child deletion causes
  zero store deletes. For OpenAI, test first-page `404` with zero deletes and
  final store-delete `404` after confirmed file cleanup. For Gemini, verify
  `force=true` for documents and `force=false` for the Settings store delete.
  Simulate a document added after enumeration and verify that retry enumerates
  and deletes it before deleting the store.
- Run the native test suite and `npm run check` in `webapp`. For provider
  validation, use dedicated temporary stores and files to check listing,
  details, and deletion for both providers. Verify that Gemini rejects a
  non-forced delete while a document remains and accepts it after the last
  document is deleted.
