# Referenced folders in chat: technical design

Date: October 30, 2026

Status: Technical design. This document does not implement the feature.
Live multi-store validation is recorded in section 9. OpenAI's observed
two-store boundary and the user-approved overflow rule are defined below.

## 1. Scope and confirmed behavior

Use uploaded folders referenced in a Forum's Files, a Character's Files, and
the chat as provider-hosted retrieval context for ordinary chat answers.

The user confirmed these rules:

1. Read current Forum and Character references on each request. Removing a
   reference from settings stops that source from supplying it on the next
   request, including in an existing chat.
2. Each OpenAI request receives only matching OpenAI stores. Each Gemini
   request receives only matching Google stores. Silently skip folders
   uploaded to the other service.
3. Keep the existing credential rule: the model connection must use the same
   saved API key ID as the store upload.
4. `/detach <Ref ID>` removes only an explicit chat attachment. A reference
   still present in Forum or Character settings remains eligible.
5. For OpenAI, send at most two distinct matching stores. If there are more,
   prefer chat sources, then Character sources, then Forum sources. Name
   omitted folders in the existing notice. This rule was confirmed after
   the live checks in section 9 demonstrated the two-store boundary.

Forum references apply to every replying Character in that Forum. Character
references apply only to the replying Character. Explicit chat attachments
apply to every replying Character. Combine these sources as a union; there
is no exclusion or override between layers before applying OpenAI's capacity
rule. Google receives the full distinct matching set.

A folder must have its own ready cloud store when a new reference or chat
attachment is added. An uploaded ancestor or descendant does not qualify it.
One folder currently has at most one store, on either OpenAI or Google. A
request can use several folders, but a single folder does not supply stores
for both services.

This feature extends the existing chat-folder path. Keep note snapshot
behavior from [notes-design.md](notes-design.md) separate: folder references
have no saved text copy or durable first-use scope. Keep upload and removal
behavior from [store-design.md](store-design.md). This document defines the
three-source selection and replaces the existing notice behavior for normal
cross-service filtering.

## 2. Existing code to extend

| Code | Current responsibility | Design action |
| --- | --- | --- |
| `src/workspace/file_reference.h` | `FileReference { kind, ref_id, name }`. | Reuse entries of kind `folder`. |
| `src/workspace/workspace.{h,cpp}` | Loads `WorkspaceForum::file_references` and `WorkspaceCharacter::file_references`. | Read these lists during request preparation; keep workspace loading free of store lookups. |
| `src/workspace/workspace_config_store.cpp` | `create_file_reference` validates source kind and exact-folder store readiness. | Keep existing admission validation. |
| `src/storage/folder_store_storage.{h,cpp}` | Stores service, upload key ID, remote store ID, readiness, and chat folder associations. | Reuse current records and queries. |
| `src/storage/session_database.{h,cpp}` | `SessionJournal::folder_ids`, `add_folder`, and `detach_folder` manage explicit chat attachments. | Keep their meaning and persistence unchanged. |
| `src/session/session_open.cpp` | Supplies `ItemReader` and `StoreReader` callbacks through `WorkspaceConfigStore`. | Reuse both callbacks. |
| `src/session/session_controller.cpp` | `start_generation` resolves only chat-added folders per target. | Extend that resolver to the three sources. |
| `src/providers/folder_store_client.{h,cpp}` | `folder_store_match` checks direct-service connection and saved-key eligibility. | Reuse the helper; do not infer service from model names. |
| `src/characters/model_context.h` | `GenerationRequest` carries optional `FolderSearchAccess` and local error context. | Reuse both fields. |
| `src/providers/responses_api.cpp`, `google_api.cpp` | Serialize hosted File Search tools. | Keep their existing request formats. |
| `src/providers/provider_client.cpp`, `tool_calls.cpp` | Preserve hosted retrieval during custom-function continuations. | Keep continuation behavior and accurate tool-availability instructions. |
| `src/providers/providers.cpp` | Appends `folder_search_context` to provider request errors. | Supply accurate source-specific recovery context. |

There is no new schema, persisted remote-ID list, scope table, controller
cache, background task, provider setting, SDK, or HTTP client. Do not copy
inherited references into `session_folders`: that table records deliberate
chat additions, which `/detach` and Clear must continue to control.

## 3. Identity and eligibility

Keep three identities distinct:

- A CHA Ref ID identifies the vault-local folder. Parse it with
  `parse_item_reference` and deduplicate by its numeric item ID.
- `folder_stores.id` identifies one local upload record. It is not a provider
  resource ID and is never sent as a File Search store name.
- `FolderStoreRecord::remote_store_id` is the provider's vector store ID or
  File Search store name. Only this value enters the provider tool payload.

Resolve against the active vault through the existing callbacks. The name
captured in settings is display metadata, not an identity or lookup key.
Use the current folder name when available, and the captured name or Ref ID
when reporting an unavailable source. A rename or move does not change identity.

When folder metadata is read, compare its current name with each contributing
Forum or Character reference's captured name. If they differ, log a warning
with the session, owner, Ref ID, and both names, as the note-reference path
already does. Continue normally; do not add a chat notice, require a name
match, or update the captured name automatically. A rename can cause this
warning, and equal names cannot prove cross-vault identity.

Reuse add-time validation. At dispatch, still check that the item is a folder,
that its store is `ready`, and that its remote ID exists and is nonempty.
Configuration can outlive an upload, and copied or restored configuration can
contain unavailable references. Syntactically valid references must still load
when their items or stores are unavailable. The existing workspace loader
rejects malformed Ref IDs; this design does not change that validation.
Do not require a source-name match, auto-upload a folder, use an ancestor's
store, or send local note text as a fallback.

Keep the existing direct-connection rules in `folder_store_match`:

| Service | Eligible effective model connection |
| --- | --- |
| OpenAI | Network mode, saved API key authentication, `api = responses`, HTTPS, host `api.openai.com`, port 443, empty `base_path`. |
| Google | Network mode, saved API key authentication, `api = google`, HTTPS, host `generativelanguage.googleapis.com`, port 443, `base_path = /v1beta`. |

The helper uses `ProviderAuth::none` for the normal saved-key path. Preserve
its host normalization, legacy saved-key-name resolution, and warning for
unused OpenRouter target settings. Unused settings must not block a valid
direct connection.

Extract a small `folder_store_endpoint_service` helper from the existing
matching logic. It identifies a direct OpenAI or Google network endpoint
using protocol, HTTPS, host, port, and base path, before authentication or
saved-key checks. Both `folder_store_match` and request resolution must use
it. Keep `folder_store_match`'s success contract unchanged: its `service` is
populated only when authentication and saved-key validation also succeed.
An empty match result must not hide a known endpoint service and prevent
silent filtering of the other service's records.

After matching the service, require equality of the connection's resolved
saved key ID and `record.api_key_id`. Do not compare key values, choose another
key, or send an upload credential through a different model connection. Two
saved key IDs with identical values still do not match. A replacement value
under the same saved key ID can change remote access; the provider decides
whether that credential can access the store.

OpenAI models reached through Chat Completions, subscription authentication,
OpenRouter, or a custom proxy remain outside this supported retrieval path.
Google's compatibility endpoint likewise does not use native File Search.
Do not silently change the selected endpoint, protocol, or model. For a
connection that cannot use stores, apply section 7's source-aware policy:
Forum-only references produce a log warning; deliberate Character and chat
references can produce one combined notice for that target. Continue the
ordinary request.

## 4. Per-target request preparation

Extend the existing folder-resolution block in
`SessionController::start_generation`, after recipients are known and before
constructing each `ProviderRequestInput`.

1. Use the current workspace snapshot for the Forum and Character references.
   Use the effective provider configuration of the target's request-owned
   Character definition for matching, including current session overrides.
2. Skip all folder resolution for maintenance answers before source or store
   lookups. Auxiliary requests remain excluded as described in section 6.
3. Collect numeric folder IDs from the current Forum's `folder` references,
   the target Character's `folder` references, and `journal_.folder_ids()`.
   Ignore `note` references here. The loader already rejects malformed Ref
   IDs. If parsing nevertheless fails, skip that entry defensively without
   a new notice, warning, or separate malformed-input test contract.
4. Deduplicate this union by numeric folder ID. Retain its originating layers
   in request-local metadata for diagnostic and recovery messages. Do not
   mutate settings or the journal during collection.
5. Process the combined IDs in ascending numeric order. Reference-list order,
   discovery order, and the addition or removal of a duplicate source must
   not reorder the same eligible set.
6. Classify the endpoint and call `folder_store_match` once per target. If
   matching fails, use the connection-failure policy in section 7 and stop
   resolution for that target. Where an endpoint service is known, still
   discard known other-service records before reporting Character/chat
   references. Do not run connection checks when there are no candidates.
7. Use `read_item_` and `read_store_` to resolve each distinct folder. Check
   item kind, then apply the service and readiness checks below. Catch
   source-lookup failures per folder, log them, and continue with the other
   candidates. Normal journal or request persistence errors keep their
   existing error handling.
8. For a supported direct OpenAI or Google connection, silently skip records
   belonging to the other service. This is normal selection, including for
   explicit chat attachments: it produces no omission notice or warning.
   Apply this service filter before reporting a ready-state or key mismatch
   for a record whose service is already known to be different.
9. For the remaining folders, require a ready nonempty remote ID and the same
   saved key ID. Warn and report other failures as section 7 specifies.
   If no record exists, its service is
   unknown; report `no ready store` rather than guessing which service it used.
10. Append matching remote IDs, deduplicating them as well. Preserve the order
    established by the sorted folder IDs. Keep labels and recovery instructions
    in local error context, outside the provider tool payload.
11. For OpenAI, apply section 7's two-store selection to this complete eligible
    set. Keep the selected IDs in stable folder-ID order and report omitted
    folders. Google has no application count cap in this design.
12. If the result is nonempty, set
    `FolderSearchAccess { service, remote_store_ids }`; otherwise leave
    `generation.folder_search` empty. Do not serialize an empty File Search
    tool or announce folder access when none is available.

Implement this with local containers in the session layer. A small private
helper is acceptable if it makes `start_generation` easier to read; no shared
resolver service or new generic context framework is needed. Query once per
distinct folder per target. A cross-request or multicast lookup cache is not
required.

Service classification for silent filtering uses the effective protocol and
direct endpoint through the required shared helper. If credential validation
fails for an otherwise supported direct endpoint, still skip known records
of the other service. Missing records have no known service. Never guess a
service from a model name or tell a target that a different credential would
make an other-service store usable.

### Example

Assume all OpenAI stores use the OpenAI target's saved key, all Google stores
use the Gemini target's saved key, and A through E have increasing folder IDs:

| Source | Referenced folders |
| --- | --- |
| Forum | A: OpenAI; B: Google |
| OpenAI Character | C: OpenAI; A: OpenAI again |
| Gemini Character | D: Google |
| Chat | E: OpenAI; B: Google again |

| Reply target | Matching candidates | Store IDs supplied | Capacity omissions |
| --- | --- | --- | --- |
| OpenAI Character | A, C, E | A, E | C |
| Gemini Character | B, D | B, D | None |

Each row is ordered by the underlying folder IDs. Other-service references
produce no notice. C never reaches the Gemini Character, and D never reaches
the OpenAI Character. Removing A from the Forum still leaves it available to
the OpenAI Character through its own Files settings.

E has chat priority; A and C have Character priority, so A wins their folder-ID
tie-break. A's additional Forum source does not lower its priority or consume
another slot. The provider payload contains A and E in folder-ID order.

Source priority controls only OpenAI capacity selection. It does not tell the
model to rank documents by source layer or promise that every supplied store
will be searched for every answer. With at most two matching OpenAI stores,
all are supplied; source priority cannot reorder the payload.

## 5. Provider payloads

The existing adapters already support these payloads. The required change is
the source set supplied to them, plus source-aware diagnostics.

### OpenAI Responses

Append one hosted tool containing the eligible OpenAI remote IDs, subject to
the observed two-store boundary and the overflow policy in section 7:

```json
{
  "tools": [
    {
      "type": "file_search",
      "vector_store_ids": ["vs_character", "vs_chat"]
    }
  ]
}
```

This fragment shows only File Search. Preserve other supported tools, the
existing `tool_choice`, generation settings, and streaming behavior. Preserve
`include: ["file_search_call.results"]` from the current implementation so
hosted output can be replayed during continuation with `store: false`.
The model selects when to retrieve; supplying IDs does not force a search.
The official [OpenAI File Search guide](https://developers.openai.com/api/docs/guides/tools-file-search)
defines this tool shape and the optional results inclusion.

### Native Gemini Generate Content

Supply all matching Google store names through the existing native builder:

The live checks in section 9 accepted three stores on both configured native
Gemini models and retrieved a fact from every store.

```json
{
  "tools": [
    {
      "fileSearch": {
        "fileSearchStoreNames": [
          "fileSearchStores/forum",
          "fileSearchStores/character",
          "fileSearchStores/chat"
        ]
      }
    }
  ]
}
```

Keep full provider resource names. Do not substitute local IDs, raw file IDs,
document IDs, or OpenAI vector store IDs. The
[Generate Content FileSearch schema](https://ai.google.dev/api/generate-content#FileSearch)
defines `fileSearchStoreNames` as the required list of stores to retrieve from.
Keep Generate Content; this feature does not migrate to the Interactions API.

Keep the adapter's existing rules for combining File Search with CHA custom
functions. It currently omits optional custom functions for its known Gemini
2.5 case, and uses server-side invocation support and `VALIDATED` mode for
mixed requests on other models. Preserve returned content parts, function
IDs, and thought signatures. The current
[Google tool-combination guide](https://ai.google.dev/gemini-api/docs/generate-content/tool-combination)
describes circulation of hosted-tool context with custom function results.
This design does not expand or promise support for additional model families.

Retain the current Gemini 2.5 behavior without a new notice: when at least
one eligible Google folder enables File Search, the adapter removes optional
CHA web search, page reading, and summary functions for that answer and logs
its existing warning. This applies equally to Forum, Character, and chat
sources. Thus a Forum folder can disable those functions in every matching
Gemini 2.5 answer in all chats in that Forum, even when the user added no
folder directly to a chat. Keep File Search in this case. To retain the
optional functions, remove the applicable folder sources or choose a model
that supports the combination. This is an explicit design choice to retain
existing adapter behavior, not a new model-capability assumption.

No new retrieval filters, result-count settings, citation renderer, or
provider capability registry is required. Reuse the current tool-availability
instructions; they must describe folder search as available only when the
serialized request actually contains it.

## 6. Request lifetime and exclusions

Resolve once for each logical Character answer. Store the resulting value in
its immutable `GenerationRequest`. All HTTP calls that continue that answer
reuse the same IDs, even if settings or uploads change during the answer.
The next logical answer reads current settings and current store records.

Continue preserving hosted File Search when the existing CHA custom-function
budget is exhausted. Responses keeps its `file_search` tool. Gemini keeps
`fileSearch` while disabling further custom-function calls. Do not remove
retrieval because a different tool reached its budget.

Do not resolve or attach folders to session naming, Jev classification or
suggestions, summary-generation jobs, note compression, maintenance answers,
voice generation, or other auxiliary calls. These requests do not inherit the
chat's retrieval context merely because they use an OpenAI or Gemini model.
An ordinary answer that can call summary tools still receives folder access.

Multicast resolves independently for each target. Each receives only the
Forum, its own Character, and chat candidates filtered for its effective
connection and key. A remote access or model error for one target must not
abort other targets.

## 7. Omissions, errors, and recovery

Local reference failures must not block an answer that can otherwise run.
Keep the reference or attachment intact, omit the unavailable access, and
continue with remaining eligible stores. Recheck on the next request; no
retry queue or expiry state is needed.

| Condition | Result |
| --- | --- |
| Known record belongs to the other service on a supported direct connection | Silently skip it. |
| Missing item, or item is not a folder | Warn; omit; report `folder unavailable`. |
| Source/store lookup throws | Warn; omit; report `could not read folder store`. |
| No store, incomplete store, or missing/empty remote ID | Warn; omit; report `no ready store`. |
| Matching service with a different saved key ID | Warn; omit; report `different saved API key`. |
| Credentials unavailable for the matching service, or an unsupported connection | Apply the connection-failure policy below. |
| More than two distinct eligible OpenAI remote store IDs | Select two by the capacity rule below; warn and name omitted folders in the existing notice. Continue the answer with the selected stores. |
| Provider rejects a supplied store, model/tool combination, or store count | Fail only that target through the normal request error path, with source-specific recovery context. |

### OpenAI capacity selection

Use a fixed application maximum of two remote store IDs for OpenAI Responses,
based on the observed boundary for the tested models. This is the chosen
supported application behavior, not a claim that every OpenAI model has the
same independently verified maximum. Do not add a model-limit registry or a
new configuration setting.

Apply the cap only after service, readiness, and saved-key filtering and after
remote-ID deduplication. Assign each distinct remote ID the highest source
priority among all its eligible contributions: chat, Character, then Forum.
If several folder IDs resolve to the same remote ID, merge their sources for
selection and diagnostics; one remote resource consumes one slot.

Select the first two by source priority, breaking ties by the smallest
contributing numeric folder ID. Then serialize those selected IDs in ascending
folder-ID order. This keeps the payload stable when the selected set is the
same, even if a duplicate source changes its priority. No timestamps or saved
selection state are required. A new chat attachment or settings change can
change the selected set on the next answer, as intended.

For each omitted remote ID, name its contributing folders and sources once
in the existing `Folder access omitted:` notice with the reason
`two-store OpenAI limit`. Log the selection and omissions without credentials
or document contents. Keep every reference and attachment intact. Do not
combine stores, upload new resources, silently drop a layer, retry without
stores, or change the model. Recompute selection on the next ordinary answer.

Google keeps the full matching union. The live check validates the ordinary
three-store case on the two configured models; it does not establish an
unlimited capacity or justify inventing a Google cap. A later provider count
rejection still follows the normal target error path.

### Connection failures

When `folder_store_match` fails, no folder access can be supplied for that
target. Use this connection-failure policy before ordinary per-folder errors:

- Skip Forum-only references without a chat notice. Write one log warning for
  the target with the connection reason and affected Forum references. A
  Forum shared by supported and unsupported Characters must not cause an
  unfixable omission notice on every unsupported Character's answer.
- Report only references from the target Character's own Files or explicit
  chat attachments. If the same folder is also in Forum settings, include
  that source in recovery guidance, but emit only one entry for the folder.
- When the endpoint helper identifies a service, silently filter known
  other-service records from these deliberate references first. Credential
  failure does not turn an OpenAI endpoint into a potential Google consumer.
- Combine the remaining deliberate references into one line per target with
  their labels, sources, and one connection reason. If filtering leaves no
  such references, emit no notice. For unsupported authentication, protocols,
  or endpoints, say `this connection does not support folder search`; for a
  native saved-key connection whose key lookup fails, use its actual key
  reason. Do not suggest that adding a key enables an unsupported protocol.

No lookup is needed for a Forum-only folder once the connection failure is
known. Where a deliberate reference's service must be checked, use its store
record; an absent or unreadable record has unknown service and stays in the
combined connection notice. Keep lookup failures in the log. This branch
does not create additional per-folder missing-store notices.

Combine omissions into the existing per-turn `Folder access omitted:` notice,
grouped by replying Character. Deduplicate repeated folder failures across
layers within a target, and name the contributing layers. Preserve coexistence
with note omission notices. Warn through the existing diagnostic log with
session, target, source owners, Ref ID, and reason. Never log credentials or
folder document contents. Do not add an events table or write warning rows
into the chat history for every request.

Recovery must reflect the actual sources:

- For a chat-only folder, retain `/detach <Ref ID>` guidance.
- For a Forum reference, identify that Forum's Files settings.
- For a Character reference, identify that Character's Files settings.
- For a folder present in several sources, identify each source. Removing
  one source does not remove the others.
- For store readiness or credential failures, the user can repair the upload
  or connection instead of removing the reference.

Update `folder_search_context` for successfully supplied stores as well. A
later provider error must not tell the user that `/detach` alone removes an
inherited reference. Keep this context local; do not inject recovery labels
or source ownership into system prompts or File Search tool parameters.

The following is a proposed notice layout using the existing notice area and
styling. It is a design sketch; runtime copy is not changed by this document:

```text
…conversation…
Folder access omitted: Guide — Research (##00000B): no ready store;
sources: Forum Lobby Files, Guide Files, chat (/detach ##00000B).
[ Message…                                             Send ]
```

For an unsupported target with deliberate references, the existing notice
area uses one line per target, for example:

```text
Folder access omitted: Claude — this connection does not support folder search; Research (##00000B): Claude Files, chat (/detach ##00000B).
```

Provider errors already append `folder_search_context` as visible text. Use
the same source-aware formatting there. For a supplied folder shared by the
Forum and chat, the error area shows:

```text
<provider error>
Folder access: Research (##00000B); sources: Forum Lobby Files, chat (/detach ##00000B).
```

Both source names remain visible: detaching the chat source alone does not
remove Forum access. These sketches change text inside existing notice/error
areas; this document does not implement visible changes.

For capacity omissions, use the same notice area, for example:

```text
Folder access omitted: Guide — Research (##00000B): two-store OpenAI limit; sources: Forum Lobby Files.
```

No new panel, title label, helper text, dialog, or control is proposed.

Do not infer capacity from list-valued API fields or attribute a reported
Responses limit only to retired deep-research models. Use the actual model
checks in section 9 and the explicit capacity selection above.

## 8. Settings, storage, and vault lifecycle

| Event | Behavior on subsequent ordinary requests |
| --- | --- |
| Add a Forum or Character reference during an existing chat | Include it on the next relevant answer if its current store matches. |
| Remove a settings reference | Stop using that source; retain access supplied by any remaining source. |
| Change the reply target | Use the new target's Character references and effective connection. |
| Change a target's provider or key | Refilter all three sources. Do not retain previously matched remote IDs. |
| Rename or move a folder | Keep its identity and current uploaded snapshot. Use the current name in diagnostics; warn in the log when a settings reference still has another captured name. |
| Edit local notes within an uploaded folder | Keep using the uploaded snapshot. Do not upload or merge local edits during chat. |
| Remove a cloud store | The reference remains, but cannot supply access until a new matching store is ready. |
| Re-upload the same folder | Resolve its new ready store ID automatically; references remain folder-based. A change of service changes which targets can use it. |
| `/detach` a chat folder also referenced in settings | Remove only its chat source. Settings references remain active. |
| `/detach` a folder present only in settings | Preserve the existing not-attached result; do not modify settings or add an exclusion. |
| Clear the conversation | Clear explicit chat attachments with existing chat context. Current settings references remain eligible. |
| Close and reopen a chat | Restore explicit chat attachments and read current settings. There are no inherited folder snapshots to restore. |
| Delete a local folder | Keep existing store cleanup and chat-association pruning. Retained settings references become unavailable. |
| Switch Welcome to another vault path | Keep existing explicit-folder cleanup. Maintenance answers never resolve inherited folders. |

Configuration-only copies and merges retain vault-local Ref IDs without
establishing cross-vault identity. Resolve against the destination vault; a
missing folder or unavailable store is omitted. If the same numeric ID names
a different uploaded folder there, that destination folder is used. Do not
search by name, infer ownership from a matching label, or introduce an origin
registry. The user must check copied references and remove and add entries
again to select the intended destination folders. Apply the captured/current
name warning from section 3 when metadata is read. This warning helps expose
wrong-source resolution without blocking valid renames; it cannot detect an
incorrect destination folder with the same name.

Creating a new vault from an existing one keeps the existing rule that clears
copied remote-store ownership. Its references and copied chat attachments
cannot grant access until that vault has its own ready stores. Password
protection and backup/restore retain their existing ownership behavior. A
restore can contain a stale remote ID; the next provider rejection is a normal
request error. Do not add remote discovery or automatic repair.

Lookup and dispatch are not atomic with remote removal. A request already
prepared with a store ID can fail if the resource is removed before the
provider uses it. Reuse existing lifecycle guards and short database reads;
do not hold locks across HTTP calls, lease stores, or cancel unrelated upload
operations to prevent this race.

## 9. Implementation and validation

### 9.1 Mandatory live capacity check before implementation

Run a three-store inference check through OpenAI Responses and native Gemini
Generate Content on the actual configured models and account credentials.
Do this before implementing the three-source union. Mocked transports,
successful uploads, and an API field typed as an array are not substitutes.

Create three small temporary stores per service, with one synthetic document
in each. Give each document a distinct fact that is absent from the prompt.
Wait for indexing, then send one request containing all three IDs and ask for
all three facts. Record the date, exact model, API/endpoint, store count, HTTP
status, safe provider error details, retrieval evidence, and facts returned.
Never record credentials or use private vault documents. Delete the test
stores and independently owned uploaded files and record cleanup results.

A successful HTTP response alone does not pass the check: verify that the
answer retrieves the distinct fact from each store. If the provider rejects
three IDs, try two IDs to establish the observed boundary for that exact
model/account. If only some facts are retrieved, investigate with individual
stores or reordered IDs before concluding that a capacity limit exists.
Do not generalize one model's observation to all models or claim that this
small experiment guarantees retrieval of every store on every future turn.

If the required case fails, update this design with a concrete overflow or
compatibility rule before implementation. This gate is resolved for the
tested models: section 9.3 records the failure at three OpenAI stores and
success at two, and section 7 defines the user's chosen overflow behavior.
Google's three-store checks passed. Recheck an expanded supported count or a
different requested compatibility rule before changing this design; no
scheduled revalidation or automatic probing is required.

### 9.2 Implementation and local validation

1. Extend session-layer candidate collection to combine the current Forum,
   target Character, and explicit chat IDs. Sort and deduplicate while keeping
   enough source metadata for accurate recovery guidance.
2. Extend the existing resolver with silent cross-service filtering,
   per-folder lookup failure handling, and the existing same-key check.
   Extract the endpoint helper required by section 3 and use it from both
   matching and request resolution.
3. Populate the existing `folder_search` and `folder_search_context` fields.
   Apply the two-store OpenAI selection after eligibility and remote-ID
   deduplication. Keep provider serializers and continuation behavior unless
   a focused regression test exposes a necessary correction.
4. Update the existing notice/error formatting to identify the real sources,
   using the notice and error sketches in section 7. No new controls are needed.
5. Add focused tests to the existing suites and run the affected C++ targets.
   No frontend bridge, DTO, configuration-format, or schema migration is needed.

| Test area | Required coverage |
| --- | --- |
| `tests/session/unit_session_controller.cpp` and `unit_session_open.cpp` | Union across three layers; Character isolation; mixed-service multicast; silent other-service skipping; same service with different saved keys; duplicate folder/remote IDs; stable ordering when duplicate sources change. |
| Session integration | Add/remove current settings without reopening; explicit attachment survives settings removal; chat-only detach; detach leaves inherited access; Clear removes chat source and keeps settings eligibility; store removal/replacement and provider/key changes use current records. |
| Omission/error handling | Missing or wrong-kind folder; absent/incomplete/empty-ID store; lookup failure does not lose valid peers; unknown-service missing store reported; other-service record skipped before readiness/key errors; source-aware recovery for duplicate sources; existing note notices coexist; unsupported Forum-only references are log-only; deliberate references share one connection notice per target. |
| Reference names | Different captured/current names produce owner-specific log warnings without blocking or adding a chat notice; matching names do not claim source identity. |
| OpenAI capacity | At most two stores; chat over Character over Forum; numeric folder-ID ties; duplicate remote IDs use the highest contributing source and one slot; selection occurs after eligibility checks; omitted folders and sources are named; stable payload order for an unchanged selected set. Google still supplies all three eligible stores. |
| Exclusions | Maintenance and auxiliary paths perform no folder lookup or injection; ordinary answers with summary tools remain eligible. |
| `tests/providers/unit_folder_store_client.cpp` | Preserve direct endpoint, authentication, saved-key-ID/name, and unused-setting behavior; endpoint classification still succeeds when auth/key matching fails, so other-service filtering remains possible. |
| `tests/providers/unit_responses_api.cpp`, `unit_google_api.cpp` | Complete same-service ID list in one hosted tool; empty access produces no tool; no foreign-service IDs or source metadata in payloads; existing tools and settings preserved; Gemini 2.5 keeps File Search and logs removal of requested optional functions regardless of folder source. |
| `tests/providers/unit_provider_client.cpp` | IDs remain fixed through custom-function continuations and function-budget exhaustion; one multicast target's provider error does not cancel peers; inherited recovery guidance does not promise removal through `/detach`. |

Reuse injected model backends and provider transports. Keep existing reference
admission and upload/removal tests; do not duplicate their full coverage for a
request-source change. Local union, filtering, payload, and continuation tests
use no paid API calls. They complement the mandatory live capacity check;
they do not replace it. Ordinary chat generation must never create test
resources or modify stored references as part of validation.

### 9.3 Live validation results

Run on October 30, 2026, with API credentials from the user's login
configuration. The models below are assigned to Characters in the active
vault and were confirmed in the relevant account's model list. OpenAI
Characters currently use subscription connections; the experiment uses their
same model IDs through API-key Responses, which is this design's supported
retrieval path. No vault documents or settings were modified.

| API and endpoint | Exact configured model | Stores | Result and retrieval evidence |
| --- | --- | --- | --- |
| OpenAI Responses, `api.openai.com/v1/responses` | `gpt-6-astra` | 3 | HTTP 400, `invalid_request_error`, `param: tools`, `code: integer_above_max_value`; message: `Invalid input: maximum of 2 vector stores allowed.` |
| OpenAI Responses, same endpoint | `gpt-6-astra` | 2 | HTTP 200; both synthetic facts returned; hosted `file_search_call.results` included both `alpha.txt` and `beta.txt`. |
| OpenAI Responses, same endpoint | `gpt-5.6-sol` | 3 | HTTP 400 with the same two-store maximum error. |
| OpenAI Responses, same endpoint | `gpt-5.6-sol` | 2 | HTTP 200; both synthetic facts returned; hosted results included both documents. |
| Native Generate Content, `generativelanguage.googleapis.com/v1beta/models/{model}:generateContent` | `gemini-3.8-flash` | 3 | HTTP 200; all three synthetic facts returned; grounding metadata named `alpha.txt`, `beta.txt`, and `gamma.txt`. |
| Native Generate Content, same endpoint | `gemini-4-argon` | 3 | HTTP 200; all three synthetic facts returned; grounding metadata named all three documents. |

The OpenAI check used the current `file_search` shape, requested
`file_search_call.results`, and set `store: false`. The Gemini checks used
the native `fileSearch.fileSearchStoreNames` shape. Each store had one small
synthetic text document with a random project code that was absent from the
question. All documents finished indexing before inference. The two-store
OpenAI responses and both Gemini responses contained the exact expected codes.

All 15 independently owned test resources across both batches were confirmed
deleted: six OpenAI vector stores, six OpenAI Files, and three Google stores
with their documents. One OpenAI store deletion returned HTTP 500 on the first
attempt and succeeded on retry. Cleanup results and inference evidence are
recorded in the temporary machine-readable files:
`/private/tmp/cha-folder-count-current-models.json` and
`/private/tmp/cha-folder-count-results.json`. These contain synthetic test
data and resource IDs, not credentials.

These results confirm a two-store boundary for both tested OpenAI models and
three-store operation for the two tested Gemini models. They do not establish
a general maximum for Gemini or prove identical behavior for every OpenAI
model. The user approved sending at most two OpenAI stores, selected by
chat, Character, then Forum priority, with a notice naming omissions. This
resolves the capacity decision before implementation.

The implementation is complete when each ordinary target request contains
the distinct current ready stores from its three applicable sources that
match its direct service and saved key, after the approved OpenAI capacity
selection, with the confirmed omission, detach, and lifecycle behavior above.
