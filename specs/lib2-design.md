# Forum Library use in chat

Date: November 4, 2026

Status: Technical design for review. This document defines the chat behavior
that follows [Forum Library settings](library-design.md). It does not implement
the feature or change that settings document.

## 1. Required behavior

Each session belongs to a Forum. Its Library is available to every character
in that session, subject to the character's effective provider being compatible
with the Library.

The agreed rules are:

- A Library references stores from one saved API key, for OpenAI or Gemini.
- If the Forum has a default provider, it overrides the provider for all
  characters throughout the Forum's sessions. The override applies to each
  conversational request, whether or not the model searches the Library.
- Without a Forum default provider, each character uses its normal provider
  selection, including any existing session provider choice.
- Compatible characters receive the Library's store references in their model
  requests. Incompatible characters continue chatting without Library access.
- The provider performs native file search. Exposing that tool does not force
  a search for every answer.
- Character configuration and saved session provider choices are not rewritten
  by the Forum override.

This stage adds runtime behavior to the settings already stored in
`WorkspaceForum::library`. It adds no store management, per-session Library
setting, cross-service search, or new chat controls. It disables the existing
chat Model selector while a Forum default applies.

## 2. Current code and intended change

The settings implementation already loads `ForumLibrary` with `api_key_id`,
`store_ids`, and optional `provider_id`. `forum.library.update` saves it through
the existing workspace edit path without shutting down live sessions.

The provider builders already emit both native search formats:

- `src/providers/responses_api.cpp`: OpenAI `file_search` with `vector_store_ids`.
- `src/providers/google_api.cpp`: Gemini `fileSearch` with `fileSearchStoreNames`.

They currently get their store IDs from `GenerationRequest::folder_search`.
`SessionController::resolve_folder_search()` still resolves attached folders
through an old store-reader callback. Production `open_session()` supplies a
callback that always returns no store because folders no longer own stores.
That path also contains cross-provider search and excess-store fallback code
which this design does not need.

Replace that source of chat store access with the Forum Library. Reuse the
native request builders, provider streaming, tool continuations, configuration
snapshots, and shared character settings resolver.

## 3. Effective provider

Extend `Workspace::resolve_character_settings()`, which is already used by
both character-definition construction and `to_snapshot()` for chat settings.
Do not implement a separate provider override in each request builder.

Provider selection has this precedence, highest first:

| Priority | Source |
| --- | --- |
| 1 | Valid default provider from the Forum Library |
| 2 | Existing valid provider choice for this character in this session |
| 3 | The character's configured provider |

When there is no Library, or its default provider is None, retain the current
selection behavior. A default provider selects that provider's full base
configuration: model, endpoint, API, authentication, limits, and other provider
options. Continue applying the current character/session reasoning-effort and
character web-search overrides to that base configuration under their existing
rules. This feature does not add a second set of reasoning or web-search rules.

Stored per-session choices remain present but are masked while the Forum
default applies. Existing session-setting operations do not bypass this
precedence. Removing the Forum default exposes the normal selection again.
The existing chat snapshot reports the effective provider and effort through
the same resolver.

### Chat Model selector

While a valid Forum default applies, the chat toolbar disables its Model
selector. The selector shows the Forum default, as reported by the snapshot.
The Character and Effort selectors stay enabled. Effort still applies under the
rules above. Do not add a label or helper text.

```text
[ Sol        ▾ ]  [ Gemini work  ▾ ]  [ Default ▾ ]
  Character         Model (disabled)    Effort
```

The default is valid or not for the whole Forum, so one property is enough.
Add the required boolean `model_locked` to `ChatSettings` in
`resources/dto.yaml` and `src/runtime/protocol.h`. `to_snapshot()` sets it to
true when the Forum has a valid default provider. The selector uses
`disabled || snapshot.chat_settings.model_locked`. The session-setting
operation needs no new rejection; the precedence already masks a stored choice.

Add one silent workspace function for the valid Forum default:

```cpp
// The Forum default provider when it is valid, or nullptr.
[[nodiscard]] const WorkspaceProvider* forum_default_provider(
    std::string_view forum_id) const;
```

`resolve_character_settings()` and `to_snapshot()` both use it, so the toolbar
state and the request provider cannot disagree.

### Default provider validity

Before applying the Forum default, check that it exists and can use the
Library's saved key under the same local eligibility rule as Library settings.
If the provider or key has been removed or changed incompatibly, log a warning
and ignore the stale default for this generation. Use the normal selection
and evaluate its Library compatibility. Do not make unused stale configuration
prevent an otherwise valid chat. A missing Library key, or a key without a
value, makes the default stale.

`resolve_character_settings()` and `forum_default_provider()` write no log.
`to_snapshot()` calls them for each member on each chat snapshot. Write the
stale-default warning in `character_definition()`, next to the existing
"Ignoring unavailable session provider" warning, so it appears once for each
generation target.

A valid default still applies if store discovery would fail, a remote store
has disappeared, or the locally saved store list is invalid. Provider selection
and store access are separate decisions: an unavailable store must not silently
switch the model answering the user.

An API error after a request has been sent is a generation failure, not a reason
to switch to a different provider automatically.

## 4. Local Library compatibility

Reuse `folder_store_match()` and the existing `provider_uses_key_stores()`
check. The latter currently lives in `src/app/workspace_operations.cpp`; move
it to the workspace layer with a declaration in `workspace.h` so settings and
runtime can call the same function. Do not make workspace code depend on the
application layer or copy the eligibility rule.

Also move the key and store checks of `update_forum_library()` into one
workspace function:

```cpp
// The reason the Library key or stores cannot be used, or nullopt.
[[nodiscard]] std::optional<std::string> forum_library_problem(
    const Workspace& workspace, const ForumLibrary& library);
```

It returns the existing save messages. The save path fails with the returned
message. The runtime writes it to the log and omits Library search. The save
path keeps its separate default-provider check through
`provider_uses_key_stores()`, because a valid default does not depend on the
store list.

A provider can receive the Library references only when:

1. `forum_library_problem()` returns no problem. The Library key exists, has a
   value, and has `cloud_store` equal to `openai` or `google`. The store list
   is nonempty, has no duplicates, and uses that service's valid resource-ID
   format. OpenAI has at most two stores, as in Library settings.
2. `provider_uses_key_stores()` accepts the effective provider: it passes
   `folder_store_match()` (a network connection, supported native endpoint and
   API, and saved-key authentication), the returned service matches the key's
   Cloud store service, and the returned key ID is the Library's saved key ID.

Do not truncate an invalid OpenAI list to two stores or select a different
subset. For a locally invalid list, omit Library search for the request and log
the reason. Preserve the saved configuration for repair.

The existing matching function accepts OpenAI Responses at `api.openai.com`
and Gemini at `generativelanguage.googleapis.com/v1beta`, over HTTPS on port
443. It excludes subscription authentication, other endpoints, and other APIs,
including Chat Completions. It resolves explicit key IDs before legacy saved
key names and rejects unavailable or ambiguous keys. Keep those rules.

Two saved key entries are distinct even when their secrets happen to match.
Do not compare secrets or assume that another key can access the same project.
Never substitute the Library credential into an incompatible character's
provider configuration.

| Forum Library | Effective character provider | Result |
| --- | --- | --- |
| None | Any ordinary provider | Normal chat, no Library tool |
| OpenAI key A | OpenAI Responses using saved key A | Include OpenAI store IDs |
| OpenAI key A | Gemini, Chat Completions, subscription auth, or another saved key | Normal chat, no Library tool |
| Gemini key B | Native Gemini using saved key B | Include Gemini store names |
| Gemini key B | OpenAI or another saved key | Normal chat, no Library tool |

These are local endpoint and credential checks, not proof of remote model
support or store permissions. Do not add a model catalog, capability cache,
probe request, or store listing to each turn. Use a model that supports the
provider's native File Search API. An unsupported model or inaccessible store
reported by the provider follows normal generation error handling. Unknown
remote capability is not silently retried without the Library.

## 5. Request preparation and configuration changes

Use the current immutable workspace snapshot when generation begins:

```text
Session forum ID
       ↓
Current Forum settings
       ↓
Effective provider for each target character
       ↓
Library compatibility check
       ↓
Native search references, or no Library tool
       ↓
Existing provider request and tool-continuation path
```

In `SessionController::start_generation()`:

1. Read one workspace snapshot and find the session's Forum.
2. Build each target's character definition from that same snapshot and the
   existing session settings, using the extended provider resolver.
3. Resolve Library access against that character's effective provider.
4. Put compatible store IDs into `library_store_ids` of the immutable
   generation input.
5. Start generation through the current `Providers` path.

Today `definition_for()` reads a workspace snapshot itself. Pass the already
captured snapshot to it, or use that snapshot directly, so provider selection
and Library access cannot come from different revisions. All children of an
All-characters request use the same Forum revision, while compatibility is
still evaluated for each child's effective provider.

The Library remains a Forum property. Do not copy it into the session database,
the session's saved character settings, or a controller-owned mutable cache.
Use a plain request-owned list in place of the obsolete folder access value:

```cpp
// GenerationRequest
// Native File Search store IDs for this request's provider. Empty means no Library.
std::vector<std::string> library_store_ids;
```

CHA computes the list for the effective provider, so the provider API already
identifies the service. Do not add a service field or a separate struct. The
list contains no credential, folder link, or callback.

A Library save must not shut down live sessions. It must refresh the
presentation of open sessions in that Forum, so that the chat toolbar shows
the new effective model and `model_locked` value. Use the existing no-restart
path:

```cpp
apply_edit(live_sessions, store.apply_forum_library(id, library), false);
```

This calls `refresh_presentation()` for the affected Forum only.
`update_forum_library()` and `Application::update_forum_library()` therefore
receive `live_sessions`. A save affects the next generation in any session of
the Forum. Already prepared requests and their continuation rounds retain their
starting provider and store IDs. Do not mutate a request midway through a
reply. Removing the Library or default provider follows the same rule.

Other workspace edits retain their existing session-reload behavior. Vault
switches, session closure, cancellation, and secondary-process restrictions
also retain their existing behavior. Add no Library-specific timer or refresh
worker.

## 6. Provider requests

### OpenAI

When `library_store_ids` has items, the Responses builder appends one native
search tool containing all selected Library store IDs:

```json
{
  "tools": [
    {
      "type": "file_search",
      "vector_store_ids": ["vs_research", "vs_reference"]
    }
  ],
  "include": ["file_search_call.results"]
}
```

This is a request fragment. Merge with the existing tools and include entries;
do not replace web or summary tools. Do not force File Search with a new
`tool_choice` setting. Keep the existing default of automatic tool choice and
preserve any independently configured web-search behavior.

Keep `file_search_call.results`, existing hosted-tool continuation handling,
and the native File Search definition in later custom-tool rounds. The current
tool-limit path retains native File Search after ordinary custom functions
reach their limit; preserve that behavior. Store references must remain on
each applicable outgoing conversational request, not only the first request
after opening the session.

OpenAI executes search on its side. CHA does not download stores or execute
a separate search callback. See the official
[OpenAI File Search guide](https://developers.openai.com/api/docs/guides/tools-file-search).

### Gemini

When `library_store_ids` has items, the Gemini builder appends one native File
Search tool:

```json
{
  "tools": [
    {
      "fileSearch": {
        "fileSearchStoreNames": [
          "fileSearchStores/research-123",
          "fileSearchStores/reference-456"
        ]
      }
    }
  ]
}
```

Use full resource names without stripping their prefix. Keep the existing
GenerateContent/streamGenerateContent transport; adopting another Gemini API
is not required. The REST schema defines these fields in the
[Gemini Tool reference](https://ai.google.dev/api/generate-content#Tool).

Preserve the current `google_file_search_only_model()` behavior for the known
Gemini 2.5 family: when File Search is present, omit optional CHA custom
functions that cannot be combined with it and write the existing warning.
For models that can combine tools, retain those functions and the existing
`toolConfig` handling. Do not remove the Library to keep an optional function.
Gemini documents the supported combinations in its
[File Search guide](https://ai.google.dev/gemini-api/docs/file-search#tool-combination).

Accepted consequence: when a Forum has a valid Library and a Gemini 2.5
default provider, every character in that Forum loses web search, page
reading, and summary tools on each request. Only the existing warning log
records this. Select a newer Gemini model as the default to keep those tools.

Keep File Search and its store names in continuation requests. Disabling
custom function calls after the existing call limit must not remove native
File Search. Provider-hosted retrieval does not become a CHA function call.

## 7. Prompts, errors, and scope

Update the existing `update_tool_instructions()` detection and wording to refer
to the Forum Library rather than attached folder stores. State that Library
search is available only when the final request contains its native tool.
Do not tell incompatible characters that they can search the Library.

Continue using provider results as retrieved source material, not instructions.
Do not add store descriptions or file contents to every system prompt. Store
IDs belong in the tool payload; key values never belong in model text or logs.
Retain existing response decoding and transcript behavior. A new citations
viewer, durable retrieval archive, and additional chat status labels are out
of scope.

Use warning logs for stale or invalid configuration, with Forum, session,
character, provider/key IDs, and the reason. Do not add a warning line to every
chat response or an extra status property. Having no Library is a normal state
and needs no log. An incompatible character in a Forum with a Library is also
a normal state under the agreed rules; log it at info level, not as a warning.

| Situation | Behavior |
| --- | --- |
| No default, incompatible provider | Send normal chat without Library references; log the omission at info level. |
| Stale default provider, or missing Library key or key value | Ignore the default, log a warning, and evaluate the normal provider. Omit Library search. |
| Invalid local store list | Omit search and log a warning; retain a valid default provider selection. |
| Remote store absent, denied, or model rejects File Search | Use the existing generation error path; do not retry by dropping stores or switching providers. |
| Transient provider error | Keep existing retry/cancellation rules and the same request's Library references. |

Do not list stores or fetch their details before each generation. The saved IDs
are the request source; the remote service determines whether it can use them.
The model is free to answer without invoking File Search. No results must not
be described as proof that every file was searched.

The override and Library apply to ordinary character replies, including the
Assistant when it is a member of a normal Forum. Do not apply them to Welcome
maintenance, recipient classification, session naming, note compression, voice
processing, or other auxiliary model requests with their own configuration.
Self-notes send no model request and need no Library processing.

## 8. Remove obsolete folder search machinery

The Library is independent of folder attachment. Existing folder/note
attachment, detachment, and prompt behavior remain available, but attachment
does not select, add, remove, or restrict Library stores. Do not require an
attached folder for Library access or restore folder-to-store persistence.

Replace the old source of access instead of running both paths:

- Remove `resolve_folder_search()`, `FolderSearchStore`, `StoreReader`, and
  `read_store_`, including the empty callback in `open_session()` and test
  constructors. Keep item/note readers used by attachments elsewhere.
- Replace `FolderSearchAccess` and the generation `folder_search` field with
  `library_store_ids`. Remove `search_targets` and
  `folder_search_context`, the old omission notices, and folder/detach hints
  appended to generation errors.
- Remove the `search_attached_files` custom tool, its definition, dispatch,
  request callback, and tool-instruction branches. This stage does not support
  searching an OpenAI Library from Gemini or vice versa through a CHA proxy.
- Remove `FolderStoreClient::search_stores()` and `FolderStoreSearchTarget`
  after removing their production caller. Keep cloud-store listing, upload,
  deletion, eligibility, and shared service types used by Library settings and
  other existing operations.
- Remove tests and contracts that exist only for the replaced folder-store
  path, its cross-provider fallback, or its extra-store fallback. Keep and
  adapt native File Search, continuation, and tool-limit coverage.
- In `resources/assistant-maintenance.md`, replace the "Folder search uses
  these routes and limits" list under "Explaining attachments". CHA embeds
  this file in the Assistant prompt, and the list describes removed behavior.
  Replace it with a short Library rule: a Forum Library gives native File
  Search to characters whose effective provider uses the Library key; a Forum
  default provider replaces the model for all characters; attached folders do
  not give search access. Keep the existing Gemini 2.5 summary-tool sentence.

Limit this cleanup to the replaced search path. Do not rename all store files
or refactor upload/storage services as part of this feature.

## 9. Implementation areas

| Area | Change |
| --- | --- |
| `src/workspace/workspace.cpp`, `workspace.h` | Extend the shared provider resolver; add `forum_default_provider()`; move `provider_uses_key_stores()` and add `forum_library_problem()` for settings and runtime. Write the stale-default warning in `character_definition()`. |
| `src/app/workspace_operations.cpp`, `workspace_operations.h`, `application.cpp` | Use the shared Library checks; pass `live_sessions` and refresh presentation after a Library save, with no shutdown. |
| `src/session/session_controller.*`, `session_open.cpp` | Resolve Library access from the current Forum and one workspace snapshot; remove obsolete store-reader plumbing. |
| `src/characters/model_context.h` | `library_store_ids` in generation input. |
| `src/providers/responses_api.cpp`, `google_api.cpp` | Use Library references in the existing native tools. |
| `src/providers/provider_client.cpp`, `model_backend.h`, `tool_calls.*`, `providers.cpp` | Preserve native search across rounds, update prompt wording, and remove custom folder-search execution and error context. |
| `src/providers/chat_completions_api.cpp`, `anthropic_api.cpp` | Remove the old custom folder-search tool; incompatible chat keeps its ordinary tools. |
| `src/providers/folder_store_client.*`, `folder_store_types.h` | Remove unused proxy-search support; retain the store management code. |
| `src/runtime/session_projection.cpp`, `protocol.h`, `protocol.cpp` | Reuse effective provider resolution; set `ChatSettings::model_locked`. |
| `resources/dto.yaml`, `webapp/src/api/schema.d.ts`, `webapp/src/api/client.ts` | Add the required `model_locked` boolean to `ChatSettings` and its type guard. |
| `webapp/src/components/ChatToolbar.tsx` | Disable the Model selector while `model_locked` is true. |
| `resources/assistant-maintenance.md` | Replace the obsolete folder-search routes with the Library rule. |

No new database schema, persisted enable flag, Library session state, new UI
control, bridge method, or API endpoint is needed.

## 10. Validation

Use the existing native tests and fake transports to verify behavior:

1. **Provider precedence:** with two differently configured characters and
   session overrides, the Forum default wins for both request definitions and
   chat snapshots. Without a default, normal selection wins. Stored character
   and session settings remain unchanged. Verify existing reasoning/web-search
   layering and unrelated Forums too. `model_locked` is true only with a valid
   default, and the toolbar disables only the Model selector.
2. **Eligibility:** cover OpenAI Responses and Gemini with the matching saved
   key, another saved key, legacy/ambiguous key names, subscription auth, Chat
   Completions, other endpoints, missing credentials, and malformed store lists.
   Incompatible characters still generate a normal reply without a Library
   tool or cross-provider callback.
3. **Request payloads:** OpenAI gets all one/two selected IDs in one tool and
   retains include entries. Gemini gets all selected full resource names in
   one tool. No attached folder is required. A second Forum cannot inherit the
   first Forum's references. Credentials do not enter the serialized body.
4. **Several characters:** test ordinary targets, All characters, and the
   Assistant as a normal Forum member. Without a default, a mixed-provider
   Forum can have children with and without Library access in the same run.
5. **Live edits and restart:** save, replace, and clear the Library/default
   while a reply runs. The reply and its tool rounds retain their original
   input; the next generation uses the published settings. A save refreshes
   the snapshot of an open session in that Forum, with the new effective model
   and `model_locked`, and the session stays live. Reopened sessions use the
   current Forum, with no Library record added to session storage.
6. **Native tool rounds:** preserve OpenAI hosted results and store references
   across custom-tool continuations and the call limit. Preserve Gemini's
   existing 2.5 custom-function omission and newer-model combination handling.
   The prompt advertises only tools actually present.
7. **Failures:** stale local configuration logs and omits unused access without
   blocking a valid chat. A chat snapshot writes no stale-default warning; a
   generation writes it once for each target. An incompatible character logs at
   info level. The save path and the runtime return the same
   `forum_library_problem()` message. A provider rejection uses normal failure handling,
   with no hidden retry without stores. Retrying/cancelling follows existing
   behavior. Store-list/detail operations are never called during preparation.
8. **Boundaries:** no Library/default override reaches maintenance, naming,
   classification, compression, or voice requests. Attachment behavior and
   store upload/delete operations retain their current responsibilities.

Extend the existing workspace, session-open/controller, session-projection,
provider-request, and tool-round tests. Run the affected native suites and
`npm run check` in `webapp` after implementation. Verify the old folder-search
contracts are removed rather than retained solely to keep tests passing.

If live validation is needed, use existing dedicated test stores and one
read-only search request per service with a matching configured provider. Do
not create or delete stores as part of chat verification. No live provider
calls are needed to write or review this design.
