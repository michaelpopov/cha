# Forum Library

Date: November 4, 2026

Status: Technical design for review. This task defines the design; it does not
implement application changes.

## 1. Purpose and scope

A Forum can have an optional Library: references to existing cloud stores and
an optional default provider for use with that Library in a later stage.

The user confirmed these decisions:

- One Library uses one saved API key. It cannot combine keys or services.
- The key must have Cloud store set to OpenAI or Gemini.
- A configured Library contains at least one store. OpenAI permits at most two
  selections in this application. Gemini has no application selection limit in
  this stage.
- The default provider is optional. It starts as None. A selected provider must
  be able to use the stores of the selected key (section 4). When Library use
  is implemented, this provider will override the provider for **all Forum
  members when the Library is used**. With None, members retain their normal
  providers; the Library's key and store selections remain configured.
- A Forum can have no Library. Users can remove an existing Library.

This stage adds the Library settings screen, store discovery, validation, and
persistence as a Forum property. Saving these settings does not change model
requests, member providers, session settings, prompts, tools, or search.

Store upload, creation, deletion, file inspection, and synchronization remain
outside this screen. Removing a Library or deleting a Forum never deletes its
remote stores. Several Forums may reference the same store.

## 2. Navigation and proposed screen

Extend the existing navigation sequence:

```text
Forum  →  Members  →  Library
Forum  ←  Members  ←  Library
```

On Members, put **Library ›** at the right of the same row as the existing
Forum back control. Library opens for `state.currentForumId`. On Library,
**‹ Members** returns to that Forum's Members screen. The existing application
navigation still provides access to other Settings forms.

Reuse the application's screen title mechanism: it shows **Members** and
**Library** in the normal title location. Do not add a title or legend at the
top of either panel. The proposed panel contents are:

```text
Members                              Library
┌───────────────────────────────┐    ┌───────────────────────────────┐
│ ‹ Brain Trust       Library › │    │ ‹ Members                     │
│                               │    │                               │
│ Persona                       │    │ API key                       │
│ [ Guest                    ▾] │    │ [ OpenAI work              ▾] │
│                               │    │                               │
│ ☑ Alice                     ⋯ │    │ Cloud stores                  │
│ ☑ Bob                       ⋯ │    │ ☑ Research                    │
│ ☐ Carol                     ⋯ │    │ ☐ Archive                     │
│                               │    │ ☑ Reference                   │
│                        [Save] │    │                               │
│                               │    │ Default provider              │
│                               │    │ [ Sol                      ▾] │
│                               │    │                               │
│                               │    │                        [Save] │
└───────────────────────────────┘    └───────────────────────────────┘
```

These drawings develop the user's sketches. The headings above the boxes are
document annotations, not additional panel labels. Use the existing form,
checkbox, navigation, error, and button styles.

### Selection sequence

1. **API key** lists saved keys with a value and `cloud_store` equal to
   `openai` or `google`. Display their saved names. Include **None** as the
   first option; it represents no Library.
2. Selecting a key requests its stores from the provider. Show a loading state,
   then **Cloud stores** with one checkbox per store. Show all pages through
   the existing store-list operation. Do not list individual files.
3. When the store list appears, show **Default provider**, a combo box. It
   lists the configured providers that can use the stores of the selected key
   (section 4), with provider names and IDs as option labels and values
   respectively. Its first option, **None**, means no default provider and is
   selected for a new Library. When editing, preselect the saved provider. The
   combo box stays visible when the user unchecks all stores.
4. **Save** persists the complete Library or removes it when the key is None.
   Stay on Library after success, retaining the saved selections. The user can
   then return to Members or use the existing application navigation.

Provider choices come from local configuration, not an external model-list
API. Load them together with the store list when a key is selected. A
provider-list failure must not prevent saving with None.

On first entry without a Library, select None. On entry with a Library,
restore its key and optional provider, fetch the current stores and providers,
and check its saved store IDs. An absent provider selects None. Changing the
key clears the draft stores, sets the provider to None, and loads the new
stores and providers. Do not keep separate drafts for each key.

For OpenAI, when two stores are selected, disable unchecked boxes. Checked
boxes remain enabled so the user can change the selection. This is a product
rule from the request, not a claim about a universal OpenAI API limit.

Use `cloudStoreLabels()` from the existing Cloud stores screen: display the
store name, fall back to its ID, and append IDs when names collide. Store IDs,
not labels or list positions, identify selections. Preserve Gemini's full
`fileSearchStores/...` resource names.

### Save, clear, and navigation

- Enable Save only when the draft differs from the saved value and is valid.
  Compare store selections as sets; checkbox order is not significant.
- A non-None key requires at least one store. The provider is optional; None
  saves `provider_id: null`. A None key saves `library: null` and needs no
  remote request or provider selection.
- Selecting None clears the dependent draft fields. Saving it removes the
  Library property. Deselecting the last store with a key still selected is an
  incomplete draft; it does not silently remove the Library.
- Disable the form during Save. Show the existing saving state and prevent
  duplicate submissions. On failure, retain the draft and show a safe error.
- Follow current Members behavior: leaving a screen discards its unsaved
  draft. Members and Library save independently. Clicking Library does not
  save member edits; returning to Members does not save Library edits.

### Empty, failed, and unavailable choices

| Condition | Behavior |
| --- | --- |
| No eligible keys | Show `No cloud store keys`. None remains available. |
| Store request pending | Show `Loading cloud stores…`; do not enable a nonempty save. |
| Store list empty | Show `No cloud stores`; do not enable a nonempty save. |
| Store request failed | Use the existing error and **Try again** control. Keep the draft. |
| No provider can use the key's stores | Show `No providers for this API key`. None remains available. |
| Provider list failed | Show the existing error and **Try again** control. Saving with None is permitted; saving a selected provider requires a successful list. |
| Saved store missing from a successful list | Keep a checked row identified by its ID and marked `Unavailable`. Let the user uncheck it; require its removal or replacement before a nonempty save. |
| Saved key no longer eligible | Show the saved ID as unavailable and an actionable error. Allow replacement or clearing the Library; do not silently select another value. |
| Saved provider no longer eligible | Keep it selected, show its ID marked `Unavailable`, and an actionable error. Allow replacement, None, or clearing the Library. |

Do not treat a failed or incomplete list request as an empty list or proof that
a store was deleted. Loading and errors do not erase saved settings. Selecting
None must allow clearing even when store discovery fails.

Use accessible labels for controls and checkbox rows, native keyboard
interaction, and the existing status and alert patterns. Add no explanatory
helper text to normal successful states.

## 3. Forum data and persistence

Forums already persist their settings in
`workspace/forums/<forum_id>/config.toml`. Add an optional `[library]` table:

```toml
display_name = "Brain Trust"
default_character = "alice"
default_persona = "builtin-guest"

[library]
api_key = "api_key_1"
stores = ["vs_research", "vs_reference"]
provider = "sol"
```

A Gemini example uses the same shape:

```toml
[library]
api_key = "api_key_2"
stores = ["fileSearchStores/research-123"]
provider = "gemini"
```

When the default provider is None, omit `provider` from the table:

```toml
[library]
api_key = "api_key_1"
stores = ["vs_research", "vs_reference"]
```

The values are references: saved key ID, remote store IDs, and, when selected,
configured provider ID. An absent provider means None; do not persist a
separate flag. Never
persist key values, store names, URLs, file lists, or a copy of provider
configuration. The key's existing `cloud_store` property determines the
service. The application already stores Gemini as `google` and displays it as
**Gemini**; retain that convention.

Add a plain value type and optional Forum property:

```cpp
struct ForumLibrary {
    std::string api_key_id;
    std::vector<std::string> store_ids;
    std::optional<std::string> provider_id;
};

// WorkspaceForum
std::optional<ForumLibrary> library;
```

Use this value in `ForumDetail` too. Store IDs have set semantics; write them
in a stable sorted order. An absent table means no Library. Clearing removes
the table rather than writing empty strings or an empty Library.

Use `WorkspaceConfigEditor` and `WorkspaceConfigStore` for the existing atomic
workspace edit and snapshot publication. Modify only submitted Forum fields.
Library-only saves must preserve member files, persona, default character,
description, and Markdown. All other Forum saves must preserve `[library]`.

A Library save has no effect on sessions in this stage. Do not call
`apply_edit()` for it. Other Forum edits call `apply_edit()`, which shuts down
the Forum's live sessions with reason `reloading`; a Library save must not
shut down or refresh them.

There is no new database table, migration job, folder-to-store link, catalog,
cache, background refresh, or remote resource ownership. Existing Forums need
no conversion. Workspace persistence, backup, and reload use the existing
configuration path.

## 4. Client and bridge contracts

Reuse existing operations, extend provider listing and Forum details, and add
one Forum Library update method:

| Operation | Request | Result or change |
| --- | --- | --- |
| `apiKey.list` | Existing request | Reuse key IDs, names, `has_value`, and `cloud_store`. |
| `cloudStore.list` | `{ api_key_id }` | Reuse `{ items: [{ id, name }] }` and existing pagination. |
| `provider.list` | Existing `{}` or optional `{ api_key_id }` | With a key filter, return only providers that can use the cloud stores of that key (see below). Keep the existing `ProviderSummary[]` result. |
| `forum.get` | Existing `{ forum_id }` | Add required nullable `library` to `ForumDetail`. |
| `forum.library.update` | New `{ forum_id, library }` | `library` is required and nullable: `null` clears the Library, and an object replaces it. Return updated `ForumDetail`. |

The Library wire shape is:

```json
{
  "api_key_id": "api_key_1",
  "store_ids": ["vs_reference", "vs_research"],
  "provider_id": "sol"
}
```

`provider_id` is a required nullable field in the wire object: a provider ID
selects the default provider, and `null` selects None. For example, saving a
Library with None uses:

```json
{
  "api_key_id": "api_key_1",
  "store_ids": ["vs_reference", "vs_research"],
  "provider_id": null
}
```

Null `provider_id` preserves the Library; null `library` removes it.

For example, `forum.library.update` clears the Library with:

```json
{"forum_id": "brain-trust", "library": null}
```

Extend `ChaClient.listProviders()` with an optional key argument; callers that
pass none retain current behavior. This avoids fetching every ProviderDetail
and avoids adding key metadata to every ProviderSummary in bootstrap and chat.

Resolve the filter in native code with the existing `folder_store_match()` in
`src/providers/folder_store_client.cpp`. Keep a provider when the returned
service equals the service of the key's `cloud_store` and the returned key ID
equals the filter key. The function already accepts only the OpenAI Responses
API at `api.openai.com` or the Gemini API at
`generativelanguage.googleapis.com/v1beta`, over HTTPS on port 443, without
subscription auth. It resolves `api_key_id` first and the legacy `api_key_env`
name only when `api_key_id` is absent, rejects ambiguous names, and requires a
key value. Sessions use the same check for folder search. Do not add a second
key-resolution rule. Compare key IDs, not secret values or `used_by` display
strings. Reject an unknown filter key; return an empty list when no provider
matches.

This check covers the endpoint, API, and key only. It does not prove that a
provider's model supports future Library execution; that check belongs to the
execution design before Library use is enabled.

Define the value, nullable `provider_id`, nullable Library property, and the
`forum.library.update` request in `resources/dto.yaml`, update native
serialization and request parsing, then regenerate `schema.d.ts`. The
request's `library` field is required and nullable, so the C++ request uses
`std::optional<ForumLibrary>`. Reject unknown fields and wrong types using the
existing parser conventions.

Keep the existing context epoch, writable-Forum checks, main-vault mutation
policy, workspace update notifications, and error conventions. Add
`forum.library.update` next to the existing `forum.members.update`; no other
bridge method or cancellation protocol is needed.

## 5. Store discovery and validation

### Existing remote path

`ChaClient.listCloudStores()` already calls `cloudStore.list`. The native
application delegates to `NotesService::list_cloud_stores()` and
`FolderStoreClient::list_stores()`. Reuse it even though it is currently housed
with note operations; moving it to a separate service is unnecessary here.

The current client lists OpenAI vector stores at
`https://api.openai.com/v1/vector_stores`, following `has_more` and the next
`after` cursor. This matches the documented
[OpenAI vector store list](https://developers.openai.com/api/reference/resources/vector_stores/methods/list).

For Gemini, it lists `https://generativelanguage.googleapis.com/v1beta/fileSearchStores`
and follows `nextPageToken`. These are Gemini File Search stores, not the
general Files API or Google Drive. See the official
[Gemini File Search store reference](https://ai.google.dev/api/file-search/file-search-stores).

Both services are queried using the selected saved credential. The results are
the stores visible to that credential's project/account and permissions; a key
need not have created each store. Reuse fixed origins, native authentication,
pagination checks, timeouts, and safe error mapping. Return no credentials or
raw provider error bodies to the browser.

Reuse `useLoad` and the detail-refresh patterns. `useLoad` requests the list
on screen entry with a configured key, on key change, and on explicit retry. It
also requests the list again in the background at each detail-refresh epoch,
for example after a Library save, as the Cloud stores screen does now. Accept
this behavior. Ignore replies after the selected key, Forum, or vault context
changes. Do not add a persistent store cache or automatic polling.

### Save validation

For a non-null Library, native code validates against the current workspace:

1. The Forum exists and is writable under the current mutation policy.
2. The saved key exists, has a value, and has `cloud_store` set to `openai` or
   `google`.
3. Store IDs are nonempty, unique, and valid for that service's resource format.
   Use the existing `safe_component()` for OpenAI and `google_store_valid()`
   for Gemini; declare them in `folder_store_client.h`. Do not write a second
   format rule.
4. There is at least one store; OpenAI has at most two.
5. If `provider_id` is non-null, the provider exists and `folder_store_match()`
   returns the key's service and the selected key ID. If it is null, no
   provider is required or validated.

The screen additionally requires a successful store list for the selected key
and permits saving only currently listed selected IDs. Native validation
enforces the structural and local-reference rules even for direct bridge
calls. Save does not issue another remote list or one detail request per store.
This keeps saving local and avoids a second discovery mechanism. It does not
promise that a store still exists or remains accessible after discovery;
eventual use must handle remote failures.

For null, validate Forum write access and remove the Library. A missing key,
provider, or store cannot prevent clearing it.

### Configuration changes and reload

Library configuration is unused by chat in this stage. Do not make opening a
vault, starting a session, or unrelated settings changes fail because a
Library has stale references.

- On load, check only the structure of the table: field names and value
  types, in the same way as `warn_unknown_fields()` in `load_forum_config()`.
  Do not check key, provider, or store references, limits, or the service at
  load; the Library form and save validation do this. Keep the IDs available
  for repair in the Library form.
- If the table cannot be parsed structurally, warn and ignore it in the typed
  snapshot. Preserve its source text on unrelated edits. The existing load
  warning identifies the configuration problem; the Library screen can save a
  valid replacement. Accepted limitation: the screen then shows None, and Save
  stays disabled for an unchanged None, so the screen cannot remove the
  malformed table. The user can save a valid Library or edit the file. Do not
  add state for this case.
- Do not query providers while loading a workspace or opening a session.
- Changing a key's value or Cloud store property does not rewrite Library
  settings. A now-incompatible store-ID format, or a selected provider that no
  longer matches, is an invalid draft when the Library is edited again.
- Library references alone do not add deletion blocks for keys or providers.
  Existing restrictions caused by their other uses still apply. Deletion
  leaves the reference for repair; it never deletes remote stores or silently
  assigns another provider.

Reuse the current detail-refresh handling: refresh a clean form when local
configuration changes. Keep a dirty draft and show the existing stale notice
with Reload; prevent saving it until refreshed. A Library-only save dispatches
the usual `forum-updated` action, without adding Library data to ForumSummary
or bootstrap lists.

## 6. Implementation areas

| Area | Work |
| --- | --- |
| `src/workspace/workspace.h`, `workspace.cpp` | Add and load the optional Forum Library; warn only for structural problems. |
| `src/workspace/workspace_config_editor.*`, `workspace_config_store.*` | Patch, clear, preserve, and publish the Library through existing edits. |
| `src/runtime/protocol.*`, `request_parser.*`, `resources/dto.yaml` | Library value, nullable detail property, and `forum.library.update` request. |
| `src/app/workspace_operations.*`, `application.*` | Validate and save Library through `forum.library.update`, without `apply_edit()`. |
| `src/app/settings_operations.*`, `src/bridge/settings_dispatch.cpp` | Optional saved-key filter on `provider.list` with `folder_store_match()`, passed through the application. |
| `src/providers/folder_store_client.*` | Declare `safe_component()` and `google_store_valid()` for save validation. |
| `src/bridge/bridge_protocol.*`, `workspace_dispatch.cpp` | Add and dispatch `forum.library.update`. |
| `webapp/src/api/client.ts`, `nativeClient.ts`, `schema.d.ts` | Extend client types, guards, and calls; regenerate schema types. |
| `webapp/src/components/ForumScreens.tsx` | Add **Library ›** to Members with the existing `cha-detail-toolbar` and `cha-detail-link` classes, and a Library form using existing store labels. |
| `webapp/src/state/view.ts`, `components/App.tsx` | Add `forum-library`, its navigation action, rendering, and normal screen title. |

Update applicable wire fixtures and frontend test fixtures. No new CSS is
needed: the Forum detail screen already places its back control and
**Members ›** in one row with these classes. Do not change session execution
or introduce a general settings-wizard abstraction.

## 7. Validation plan

Implementation checks should cover behavior at the existing boundaries:

- **Persistence:** absent Library, OpenAI and Gemini round trips with and
  without a default provider, clearing, restart, stable IDs after renames,
  and preservation during Forum/member/file edits. Verify Library-only saves
  preserve all other Forum properties and selecting None preserves the
  Library's key and stores while removing its provider property.
- **Validation and parsing:** null and object Library values and a missing
  `library` field; duplicates; empty stores; three OpenAI stores; multiple
  Gemini stores; malformed resource IDs; missing credentials; a provider that
  fails `folder_store_match()`; null provider; read-only and stale contexts.
- **Local compatibility:** the provider filter excludes providers that use the
  key through another API or endpoint, such as Chat Completions, and follows
  `folder_store_match()` for legacy and ambiguous key names. Unfiltered
  provider lists remain unchanged. Malformed Library structure produces a
  warning; stale references produce none. Neither breaks ordinary vault or
  session operations.
- **UI:** both navigation links, key filtering, saved selections, None as the
  default provider, restored provider, key changes resetting the provider to
  None, saving with None when no provider matches or the provider list fails,
  and the combo box remaining visible after all stores are unchecked. Cover
  OpenAI's two-selection rule, Gemini selections, duplicate store names,
  loading/empty/retry states, unavailable references, independent
  Members/Library drafts, save failure, and clearing while discovery fails.
- **Request races:** a late response for key A must not replace key B's stores
  or providers. Navigation, Forum changes, and vault changes must not apply an
  old response to a new form. Reuse existing refresh tests for dirty drafts.
- **Scope:** saving, clearing, and deleting a Forum make zero remote mutation
  calls. Configuring a Library does not alter member providers or outgoing
  model requests. A Library save while a session of the Forum is live leaves
  that session running. Reuse existing provider pagination tests; add
  provider-client tests only if that client changes.

Run the affected native test suites and `npm run check` in `webapp` when the
feature is implemented. No live provider write is needed to validate this
settings feature; a read-only list check can supplement the existing transport
tests.

## 8. Later Library use

When present, the saved provider expresses the confirmed intent to override
all members when the Library is used. When absent, members retain their normal
providers; future Library use must handle their capabilities without silently
enabling an override. Neither choice rewrites character or Forum-member
configurations in this stage.

The next execution design must define when a Library is used, provider/model
compatibility, precedence relative to session provider choices, request/tool
construction, and unavailable-store behavior. This design deliberately adds no
runtime override, session copy of the Library, fallback provider, or resource
cleanup policy.
