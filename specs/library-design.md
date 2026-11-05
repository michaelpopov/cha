# Forum Library

Date: November 4, 2026

Status: Technical design for review. This task defines the design; it does not
implement application changes.

## 1. Purpose and scope

A Forum can have an optional Library: references to existing cloud stores and
the provider to use when that Library is used in a later stage.

The user confirmed these decisions:

- One Library uses one saved API key. It cannot combine keys or services.
- The key must have Cloud store set to OpenAI or Gemini.
- A configured Library contains at least one store. OpenAI permits at most two
  selections in this application. Gemini has no application selection limit in
  this stage.
- A provider configured with the selected key is required. When Library use is
  implemented, this provider will override the provider for **all Forum
  members when the Library is used**.
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
│                               │    │ Provider override             │
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
3. After at least one store is selected, show **Provider override**. It lists
   configured providers that resolve to that saved key, using provider names
   and IDs as option labels and values respectively. Start with **Select
   provider** for a new selection; require an explicit choice even if there is
   only one option. When editing, preselect the saved provider if available.
4. **Save** persists the complete Library or removes it when the key is None.
   Stay on Library after success, retaining the saved selections. The user can
   then return to Members or use the existing application navigation.

Provider choices come from local configuration, not an external model-list
API. They can be loaded when the key changes; keep the selection control
hidden until stores have been selected.

On first entry without a Library, select None. On entry with a Library,
restore its key and provider, fetch the current stores, and check its saved
store IDs. Changing the key clears the draft stores and provider before
loading the new choices. Do not keep separate drafts for each key.

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
- A non-None key requires at least one store and a selected provider. A None
  key saves `library: null` and needs no remote request or provider selection.
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
| No provider uses the key | Show `No providers use this API key`; require configuration elsewhere before a nonempty save. |
| Saved store missing from a successful list | Keep a checked row identified by its ID and marked `Unavailable`. Let the user uncheck it; require its removal or replacement before a nonempty save. |
| Saved key or provider no longer eligible | Show the saved ID as unavailable and an actionable error. Allow replacement or clearing; do not silently select another value. |

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

All three values are references: saved key ID, remote store IDs, and configured
provider ID. Never persist key values, store names, URLs, file lists, or a copy
of provider configuration. The key's existing `cloud_store` property determines
the service. The application already stores Gemini as `google` and displays
it as **Gemini**; retain that convention.

Add a plain value type and optional Forum property:

```cpp
struct ForumLibrary {
    std::string api_key_id;
    std::vector<std::string> store_ids;
    std::string provider_id;
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

There is no new database table, migration job, folder-to-store link, catalog,
cache, background refresh, or remote resource ownership. Existing Forums need
no conversion. Workspace persistence, backup, and reload use the existing
configuration path.

## 4. Client and bridge contracts

Reuse existing operations, extending provider listing and Forum details/updates:

| Operation | Request | Result or change |
| --- | --- | --- |
| `apiKey.list` | Existing request | Reuse key IDs, names, `has_value`, and `cloud_store`. |
| `cloudStore.list` | `{ api_key_id }` | Reuse `{ items: [{ id, name }] }` and existing pagination. |
| `provider.list` | Existing `{}` or optional `{ api_key_id }` | With a key filter, return only providers configured with that key. Keep the existing `ProviderSummary[]` result. |
| `forum.get` | Existing `{ forum_id }` | Add required nullable `library` to `ForumDetail`. |
| `forum.update` | `{ forum_id, library }` plus existing optional fields | Omitted Library preserves it, `null` clears it, and an object replaces it. Return updated `ForumDetail`. |

The Library wire shape is:

```json
{
  "api_key_id": "api_key_1",
  "store_ids": ["vs_reference", "vs_research"],
  "provider_id": "sol"
}
```

For example, clearing uses:

```json
{"forum_id": "brain-trust", "library": null}
```

Extend `ChaClient.listProviders()` with an optional key argument; callers that
pass none retain current behavior. This avoids fetching every ProviderDetail
and avoids adding key metadata to every ProviderSummary in bootstrap and chat.

Resolve the filter in native code. Prefer `config.api_key_id`; only when it is
absent, resolve the legacy `api_key_env` as a saved-key name, as provider
credential loading already does. Compare resolved key IDs, not secret values
or `used_by` display strings. Two saved keys with the same secret remain two
different settings choices. Reject an unknown filter key; return an empty list
when the key exists but no provider uses it.

List all providers configured with the key, as requested. This phase does not
add model-capability discovery or speculative protocol filtering. Key matching
does not prove that a provider/model supports future Library execution; that
check belongs to the execution design before Library use is enabled.

Define the value and nullable property in `resources/dto.yaml`, update native
serialization and request parsing, then regenerate `schema.d.ts`. The C++
update request must distinguish omitted, null, and object values. Reject
unknown fields and wrong types using the existing parser conventions.

Keep the existing context epoch, writable-Forum checks, main-vault mutation
policy, workspace update notifications, and error conventions. No new bridge
method or cancellation protocol is needed.

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

Start a fresh request on screen entry with a configured key, on key change, and
on explicit retry. Ignore replies after the selected key, Forum, or vault
context changes. Reuse `useLoad` and detail-refresh patterns; do not add a
persistent store cache or automatic polling.

### Save validation

For a non-null Library, native code validates against the current workspace:

1. The Forum exists and is writable under the current mutation policy.
2. The saved key exists, has a value, and has `cloud_store` set to `openai` or
   `google`.
3. Store IDs are nonempty, unique, and valid for that service's resource format.
4. There is at least one store; OpenAI has at most two.
5. The provider exists and resolves to the selected saved key.

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

- On load, parse a well-formed Library even if its key or provider is missing.
  Keep those IDs available for repair in the Library form; log warnings for
  invalid local references, limits, or service mismatches.
- If the table cannot be parsed structurally, warn and ignore it in the typed
  snapshot. Preserve its source text on unrelated edits. The existing load
  warning identifies the configuration problem; the Library screen can save a
  valid replacement.
- Do not query providers while loading a workspace or opening a session.
- Changing a key's value or Cloud store property does not rewrite Library
  settings. A now-incompatible store-ID format or provider association is an
  invalid draft when the Library is edited again.
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
| `src/workspace/workspace.h`, `workspace.cpp` | Add and load the optional Forum Library; report nonfatal warnings. |
| `src/workspace/workspace_config_editor.*`, `workspace_config_store.*` | Patch, clear, preserve, and publish the Library through existing edits. |
| `src/runtime/protocol.*`, `request_parser.*`, `resources/dto.yaml` | Library value, nullable detail property, and three-state update field. |
| `src/app/workspace_operations.*`, `application.*` | Validate and save Library through `forum.update`. |
| `src/app/settings_operations.*`, `src/bridge/settings_dispatch.cpp` | Optional saved-key filter on `provider.list`, passed through the application. |
| `src/bridge/workspace_dispatch.cpp` | Accept the extended Forum update. |
| `webapp/src/api/client.ts`, `nativeClient.ts`, `schema.d.ts` | Extend client types, guards, and calls; regenerate schema types. |
| `webapp/src/components/ForumScreens.tsx` | Add Members navigation and a Library form, using existing store labels. |
| `webapp/src/state/view.ts`, `components/App.tsx` | Add `forum-library`, its navigation action, rendering, and normal screen title. |

Update applicable wire fixtures and frontend test fixtures. Add CSS only if
existing navigation-row classes cannot place the two links. Do not change
session execution or introduce a general settings-wizard abstraction.

## 7. Validation plan

Implementation checks should cover behavior at the existing boundaries:

- **Persistence:** absent Library, OpenAI and Gemini round trips, clearing,
  restart, stable IDs after renames, and preservation during Forum/member/file
  edits. Verify Library-only saves preserve all other Forum properties.
- **Validation and parsing:** omitted/null/object updates; duplicates; empty
  stores; three OpenAI stores; multiple Gemini stores; malformed resource IDs;
  missing credentials; provider/key mismatch; read-only and stale contexts.
- **Local compatibility:** legacy provider key-name resolution follows the
  same precedence as credential loading. Unfiltered provider lists remain
  unchanged. Stale or malformed unused Library config produces warnings and
  does not break ordinary vault or session operations.
- **UI:** both navigation links, key filtering, saved selections, required
  provider choice, OpenAI's two-selection rule, Gemini selections, duplicate
  store names, loading/empty/retry states, unavailable references, independent
  Members/Library drafts, save failure, and clearing while discovery fails.
- **Request races:** a late response for key A must not replace key B's stores
  or providers. Navigation, Forum changes, and vault changes must not apply an
  old response to a new form. Reuse existing refresh tests for dirty drafts.
- **Scope:** saving, clearing, and deleting a Forum make zero remote mutation
  calls. Configuring a Library does not alter member providers or outgoing
  model requests. Reuse existing provider pagination tests; add provider-client
  tests only if that client changes.

Run the affected native test suites and `npm run check` in `webapp` when the
feature is implemented. No live provider write is needed to validate this
settings feature; a read-only list check can supplement the existing transport
tests.

## 8. Later Library use

The saved provider expresses the confirmed intent to override all members
when the Library is used. It is not a request to rewrite their character or
Forum-member configurations now.

The next execution design must define when a Library is used, provider/model
compatibility, precedence relative to session provider choices, request/tool
construction, and unavailable-store behavior. This design deliberately adds no
runtime override, session copy of the Library, fallback provider, or resource
cleanup policy.
