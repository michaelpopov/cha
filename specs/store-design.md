# Cloud stores in Settings

Status — Technical design for review.

## 1. Scope

Add **Cloud stores** to Settings. A user selects a saved API key, selects a
remote store, then views or deletes it. The provider is the source of the store
list. Show every store accessible to the selected key, including stores created
outside CHA. Two keys with access to the same project can show the same store.
Use the current vault's saved keys. Keep store data in the active view only.

## 2. API key setting

Replace the **Cloud store** checkbox in the new-key and existing-key forms with
a single selector. Use the strings already defined in the backend DTO.

| Display label | `cloud_store` value |
| --- | --- |
| None | `none` |
| OpenAI | `openai` |
| Gemini | `google` |

When the property is absent from saved key configuration, use `none`. There are
no deployed keys with this property, so no boolean migration is needed. If a
saved value has the wrong type or is unsupported, warn and use `none`. Keep the
credential available for its other uses. Validate the enum on create and update
requests. A user can change this setting without entering the secret again.
Renaming a key preserves its setting.

Finish the existing string conversion in the generated TypeScript schema,
client validation, and Settings forms. Use the enum in the current folder
upload flow too. Each key with a configured provider and a saved value yields
one upload choice for that provider. Reject a missing key, `none`, or a provider
mismatch before starting an upload. Failed or cancelled upload cleanup still
uses the provider recorded for that upload. [Key DTO](../resources/dto.yaml) ·
[Folder upload admission](../src/notes/folder_store_operations.cpp)

## 3. Settings navigation

Place the new row next to **API Keys**. Reuse the Settings rows, back controls,
and confirmation dialog. The text at the top of each example is the back
control. Store fields are read-only. Render a blank value for a missing
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
  fall back to the resource ID when the name is empty. Select by saved key ID
  and remote store ID so duplicate display names identify separate rows.
- Use the existing loading, empty, and retry states. A failed provider request
  shows an error. Show **No cloud store keys** for an empty key list and
  **No cloud stores** for an empty store list.
- **Delete** opens **Delete store?** with **Cancel** and **Delete**. Name the
  store and say that the operation permanently deletes the store and its files.
  For OpenAI, add that deleting a shared file removes it from other vector
  stores that use it. Use the existing busy state to prevent duplicate
  confirmation. On success, return to the selected key's store list and fetch
  it again. On failure, stay on the details screen and show the error with a
  way to retry through the confirmation. If only the list refresh fails, retry
  the list load from the list screen.

## 4. Bridge and provider client

Add the Settings screens through `Settings.tsx`, `App.tsx`, and the existing
navigation reducer. Add these methods to `ChaClient` and the native bridge.
Define the shared result shapes in `resources/dto.yaml` and regenerate the
frontend types. The existing context epoch travels in the request envelope.

| Method | Parameters | Result |
| --- | --- | --- |
| `cloudStore.list` | `api_key_id` | `{ items }`, each with `id` and `name` |
| `cloudStore.get` | `api_key_id`, `store_id` | `id`, `name`, `description`, `created_at` |
| `cloudStore.delete` | `api_key_id`, `store_id` | Empty success object |

Use a string for `description`, with `""` for a missing description, and an
RFC 3339 UTC string for `created_at`. Use the provider's opaque ID for `id`.
Gemini IDs include the `fileSearchStores/` prefix.

Resolve the saved key and its provider in the application under the active
vault context. Reject an unknown key, a missing credential, or `none`. Validate
the store ID against that provider's resource format before constructing a URL.
Use the existing `FolderStoreClient` transport, fixed provider origins, and
authentication headers. Keep credentials in native code. Return safe errors
without provider response bodies or credential values.

Run remote requests through the existing asynchronous bridge reply path and
`BackgroundJobs`. Capture the selected credential for the request before
releasing the vault lock. Check cancellation and the context epoch before
further provider calls. Ignore replies for an inactive context or a selection
the user has left. Use the normal Settings mutation permission for deletion.
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

### Gemini

- List with `GET /v1beta/fileSearchStores` and follow `nextPageToken`. Read the
  selected store with `GET /v1beta/{store_id}`. Use `displayName` and
  `createTime`. [File Search stores](https://ai.google.dev/api/file-search/file-search-stores)
- Read the description only when the user opens the details screen. Page
  through the store's documents for the reserved display name `DESCRIPTION.txt`.
  Read the candidate metadata by resource ID when necessary. A CHA metadata
  document has a `name` entry with a string list. If exactly one candidate
  matches, concatenate the strings in its `description` entry's
  `stringListValue.values` in order. Return `""` if there is no unique match or
  the description is absent or malformed. A provider request failure is a load
  error. The store's native `displayName` supplies its name.
  [CHA metadata format](../specs/store-design.md#8-local-state-and-future-compatibility) ·
  [Gemini documents](https://ai.google.dev/api/file-search/documents)
- Reuse `google_delete_store`. It lists all documents before deleting them,
  explicitly deletes each document, including `DESCRIPTION.txt`, then deletes
  the store. Use the same routine for a retry.
  [Existing Gemini deletion](../src/providers/folder_store_client.cpp)

### Deletion failures and retry

Reject invalid pages, repeated cursors, and a missing cursor when the provider
indicates more results. Finish these checks before deleting any files. An absent
child during deletion counts as complete. If any child deletion fails, stop
before deleting the store. Some children may already be gone. A retry lists the
remaining children from the provider. If only the final store deletion fails,
retrying follows the same path with an empty or smaller child list. Treat the
store as already deleted if it is absent in the authenticated key's scope and
its child cleanup has completed or the children are absent. Authentication,
permission, transport, and invalid-response errors are failures. Keep only
request-local lists of resource IDs.

## 5. Validation

- Verify key config with an absent property, all three enum values, and an
  obsolete value. Verify create, provider-only edit, rename, and regenerated
  frontend types. Verify that a mismatched key cannot start a folder upload.
- Check Settings navigation with two providers, duplicate names, an empty
  description, a multiline description, an empty list, and a provider error.
  Check cancel, success with refreshed list, partial failure with retry, and
  stale replies after navigation or a vault switch.
- Use the existing provider transport tests to verify pagination, metadata
  reading, and delete order for both providers. Cover invalid cursors, a child
  deletion failure, an absent child, and an authorization failure. Verify that
  a failed enumeration causes zero deletes and a failed child deletion causes
  zero store deletes.
- Run the native test suite and `npm run check` in `webapp`. For provider
  validation, use dedicated temporary stores and files to check listing,
  details, and deletion for both providers.
