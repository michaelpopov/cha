# Block 2 — Chat, inference, and native API integration

Status: Implementation instructions; requires completed block 1.

This is the second of three implementation blocks, sized for one 512K-token
session including implementation, tests, fixes, and review. It connects folder
stores to chat and both inference protocols and supplies the complete native
API for the later UI. This file contains the block's requirements for a coding
agent and an independent reviewer without requiring previous chat history.
Source documents: [store-design.md](store-design.md),
[store-prd.md](store-prd.md), and [store-plan.md](store-plan.md).

## 1. Deliverable and prerequisites

A user submits a real folder's Ref ID as the whole chat input to attach it.
An ordinary answer resolves that folder's current ready store separately for
each target model, matching service and saved key. OpenAI Responses and native
Google requests receive hosted File Search tools, including during function-call
continuation. `/detach <Ref ID>` removes only that folder attachment.

Deliver this chat behavior, both inference integrations, native bridge methods,
DTOs and client guards, and saved-key usage. Do not add menus/dialogs or a separate
ChaWeb notes-management API. Block 3 owns UI and full live acceptance. This block
must pass deterministic serialization, runtime, bridge, and client tests; a fake
HTTP success does not prove a live model supports mixed tools.

Inspect the actual block 1 implementation and its handoff before editing. Reuse
its concrete methods/types; do not create duplicate clients or stores. The
required prerequisite behavior is:

| Existing block 1 capability | Contract to rely on and verify |
| --- | --- |
| Ownership | One real folder has at most one `folder_stores` row: local attempt ID, folder ID, service (`openai`/`google`), saved key ID, remote ID, `incomplete`/`ready`, note count, ready timestamp, uncertainty flag. Replacement has a new attempt ID. Only ready grants access. |
| Owned resources | OpenAI file IDs are retained for cleanup; no corpus or Google operation names are persisted. |
| Journal | `session_folders` stores session key plus folder ID, not remote IDs or text. Add/detach notices are transactional. Persistent new attach rechecks readiness on its own connection. Welcome validates against the active vault. |
| Lifetime | Clear removes folder refs; cover/uncover keeps them. Local folder deletion prunes persistent refs atomically and Welcome refs through the runtime. Vault path change clears only Welcome folder refs; same-path password change preserves them. |
| Matching | A shared helper classifies direct service/protocol and resolves the stable saved key ID. Choices deduplicate configured model connections by service/key. |
| Operations | Application methods provide choices, upload, Remove, operation status, upload cancellation, terminal/failed Close, and confirmed subtree deletion with Retry/Delete anyway. All enforce epochs and relevant attempt/operation identities. |
| Cleanup/results | Remove preserves notes and session refs. Failed known cleanup retains incomplete records/IDs. Unknown/not-found may finish local removal with warnings. Failed folder deletion retains its reservation until Retry/Cancel/Close/Delete anyway. |
| Queries | Metadata and current store lookup, folder summaries, subtree ownership, and current key usage can be read without network calls. |

If a prerequisite is missing or incorrect, identify and fix the narrow integration
defect with its regression test, or report the blocker. Do not silently replace
the specified behavior with a second implementation.

The completed design is approved by the user. Follow `AGENTS.md`, preserve
unrelated work, and keep this a small extension of existing paths. Add no session
attachment cache, provider account discovery, copied-text fallback, new provider
setting, protocol switch, synchronization, citation UI, or general HTTP framework.
Ignore unused/obsolete settings with a warning instead of blocking valid work.
Do not edit documentation unless the execution request authorizes a status update.

## 2. Code map

| Area | Files and integration point |
| --- | --- |
| Input | `src/runtime/text_input.{h,cpp}` has the command descriptor table and `parse_note_reference`. |
| Session wiring | `src/session/session_open.cpp` supplies callbacks through `WorkspaceConfigStore`; its current note callback prefers compressed text. Preserve that for ordinary note refs. |
| Controller | `src/session/session_controller.{h,cpp}` owns attach/submit behavior and `start_generation`, which already resolves multicast targets and detects the Welcome maintenance Assistant. |
| Journal/backend | `src/storage/session_database.{h,cpp}`, block 1 store storage, and `src/workspace/workspace_config_store.{h,cpp}` provide attachment/current-record queries. |
| Request values | `src/characters/model_context.h` defines `GenerationRequest`; `character_config.h` defines connection fields. |
| Adapters | `src/providers/responses_api.cpp`, `google_api.cpp`, and headers own request serialization and decoding. |
| Continuation/tools | `src/providers/provider_client.cpp` owns the existing function loop; `tool_calls.cpp` inserts declarations and writes tool-availability instructions. |
| Native bridge | `src/bridge/bridge_protocol.{h,cpp}`, `notes_dispatch.cpp`, and existing dispatcher/policy tests. |
| Key usage | `src/app/settings_operations.{h,cpp}` plus its application call sites supply API-key detail/list/create/edit responses. |
| Types/client | `resources/dto.yaml`, generated `webapp/src/api/schema.d.ts`, `webapp/src/api/{client,nativeClient}.ts`, and fixtures. |
| Ref formatter | `webapp/src/notes/reference.ts` already formats note item IDs. No formatter redesign is needed. |

The single process runtime owns live controllers and journal writes. Controllers
perform no HTTP. Provider requests own immutable store access and must not reload
folder-store state halfway through an answer. The desktop bridge supplies
management methods; shared chat input/generation must also work for ChaWeb.

## 3. Chat references and journal integration

### Step 1: Dispatch a standalone item reference

Rename the internal parser to an item-oriented name and preserve its current
grammar exactly: trim surrounding whitespace, require `##` followed by exactly
six base62 digits, and use `0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz`.
The parsed ID remains case-sensitive. Embedded refs, longer refs, and malformed
strings retain ordinary-text behavior. The existing formatter/parser limitation
at very large IDs is outside scope. Zero or missing IDs cannot refer to the
synthetic root.

Dispatch by current item metadata:

| Item/association | Behavior |
| --- | --- |
| Ordinary note | Keep the existing saved text-copy attachment path, including compressed-text preference and size checks. |
| Annotation | Preserve current behavior; do not turn annotation text into a folder store. |
| New real-folder attachment with ready store | Insert the folder ID and transcript notice atomically, for example `Added folder “Research” (OpenAI).` |
| Already attached folder | Return an existing-style duplicate notice without changing the association, including when its store is now unavailable. |
| New folder with no ready store | Return `Upload this folder to a store before adding it to the conversation.` Do not insert corpus text. |
| Unknown item/root | Use the normal invalid/missing reference path. |

A ready folder can attach even if no current responding character can use its
store. Preserve submission-expiry, shutdown, busy-generation, input-consumption,
and transcript identity conventions. Attaching a folder makes no model request,
including no recipient detection or auxiliary generation triggered by the gesture.

Initial metadata comes through a narrow callback from `session_open.cpp`. For a
persistent session, block 1's journal must additionally recheck readiness on its
own connection inside the attachment transaction. Do not replace that recheck
with a separate application lookup or call another write connection under it.
Welcome uses active-vault validation and its existing runtime serialization.

### Step 2: Implement `/detach`

Add `CommandKind::detach` and its descriptor to the same input command path used
by desktop and ChaWeb. Require exactly one valid Ref ID; missing, malformed, or
extra arguments return `Usage: /detach <Ref ID>`.

Detach only a session-folder association. It works when the store, saved key,
folder metadata, or compatible model is unavailable. Use the Ref ID when the
folder name cannot be read. Record an existing-style notice atomically with
removal; for example `Removed folder “Research” from this conversation.` Repeated
detach reports not attached. Preserve earlier notices/history and ordinary note
or file attachments. Do not delete remote resources or generate an answer.

Use the journal as the source of attachment state, with no controller cache.
Exercise restart, Clear, cover/uncover, folder deletion, Welcome pruning, and vault
retarget against block 1's implementation when wiring these paths.

## 4. Request resolution

### Step 3: Resolve current access per ordinary answer target

Perform resolution in the ordinary answer dispatch path after recipient selection,
including each multicast child. Resolve the effective connection, not a provider
display name, model-name prefix, or original configuration ID.

| Service | Compatible effective connection |
| --- | --- |
| OpenAI | `mode = net`, API key, `api = responses`, HTTPS, exact host `api.openai.com`, port 443, empty base path. |
| Google | `mode = net`, API key, `api = google`, HTTPS, exact host `generativelanguage.googleapis.com`, port 443, base path `/v1beta`. |

Reuse block 1's eligibility helper. Current API-key auth is `ProviderAuth::none`
with a saved key; subscription auth is excluded. Resolve legacy key-name references
only through saved keys. Exclude test mode, proxies, alternate ports, unsupported
protocols, and Google's Chat Completions compatibility endpoint. Different saved
key IDs do not match even if their values or remote projects happen to match.

At dispatch, read journal folder IDs and their current store records. Use only
ready records matching this target's service/key. Deduplicate remote IDs. Folder
rename retains identity; Remove makes it unavailable; a later upload on that
folder replaces access for the next answer, even if the service changed.

For missing/incomplete/incompatible attachments, continue normally with remaining
eligible stores. Combine omissions into one concise notice for the turn, grouped
by target where needed. Name folders and show applicable `/detach ##......`
commands. Do not silently drop them without a reason and do not insert their
source text into the prompt. Compatible models may still see previous answers
in ordinary shared history; matching controls direct store access only.

Provider count/access/model failures remain the affected target's normal request
error, augmented with supplied folder/ref and detach context. Other multicast
targets must continue. Do not retry without stores, select an arbitrary subset,
change the model, or switch protocols.

### Step 4: Carry immutable access, with explicit exclusions

Add a small optional value to `GenerationRequest`, reusing the service type from
block 1 where appropriate:

```cpp
struct FolderSearchAccess {
    StoreService service;
    std::vector<std::string> remote_store_ids;
};
```

Populate it only after successful resolution. Keep folder labels/refs with local
request error context, not in the provider tool payload. IDs remain unchanged
through that logical answer's continuations. Providers do not reload stores or
credentials from another vault through a new store lookup mid-answer. Removing a
store prevents future resolution; an already captured request may still finish.

Do not perform store lookup at all for recipient detection, session naming,
compression, page formatting, or other auxiliary calls. Also explicitly skip the
Welcome Assistant's maintenance answers before lookup, using the maintenance
condition already detected in `start_generation`. Those are chat answers with
configuration tools; `include_tool_instructions = true` is not sufficient to
identify an ordinary answer. Builders give maintenance tools precedence even if
incorrectly supplied store data reaches them. No eligible store means existing
request behavior remains unchanged.

### Step 5: Keep limit handling evidence-based

The design has no verified universal or current model-specific request-store
limit. Do not add a numeric limit, speculative attachment count scan, or
`ProviderRequestInput` preflight-error field. In particular, do not apply a
historical deep-research two-store example to other models.

If a current applicable API/model limit is verified during implementation or
acceptance, enforce that specific rule before a new attachment and again per
target at dispatch. Count distinct ready remote IDs matching service/key against
the compatible model configurations in the conversation. Reject only the new
attachment or affected target, preserving existing history and attachments, with
detach guidance. Otherwise known-limit cases are conditionally not applicable;
provider rejection and detach recovery still require tests. Add no monitoring
or account-discovery system.

## 5. Inference adapters and function continuation

### Step 6: Extend OpenAI Responses

Append one hosted tool to the existing list:

```json
{"type":"file_search","vector_store_ids":["vs_example"]}
```

Keep supported provider-hosted web search and CHA `web_search`/`web_read`
functions. Preserve the existing initial `tool_choice` (`auto` or `required`) and
continuation behavior; broad `required` is not a promise that File Search runs.
Do not add a strict hosted-tool choice, retrieval-result expansion, or citation UI.

Preserve all provider output items needed for the next continuation, including
hosted File Search output, alongside existing function-call results. Hosted
retrieval is not a CHA function call and must not be executed or counted locally.
Keep the existing `store: false` contract. Do not change provider persistence or
discard needed replay items to work around errors. Keep normal answer and usage
decoding and do not claim a store was searched merely because the tool was sent.

### Step 7: Extend native Google

Append File Search to the existing native tool list:

```json
{
  "tools": [
    {"fileSearch":{"fileSearchStoreNames":["fileSearchStores/example"]}}
  ]
}
```

Keep selected model, endpoint, saved-key authentication, context, supported
generation settings, streaming choice, cancellation, and usage. A configuration
using Chat Completions must stay on that protocol and receive no store tool.
Do not introduce an Interactions adapter or a second continuation loop.

For a documented mixed-tool model, retain requested CHA custom functions and add:

```json
{
  "toolConfig": {
    "includeServerSideToolInvocations": true,
    "functionCallingConfig": {"mode":"VALIDATED"}
  }
}
```

Implement a small model-capability decision separate from service/key matching.
The approved design uses known non-mixed Gemini 2.5 models with File Search only,
omitting optional CHA functions with a diagnostic warning. Documented Gemini 3/4
mixed models use the mixed form. Verify current official model support rather
than extending an unverified name heuristic. For an unrecognized model, use the
mixed form if functions were requested and surface rejection; do not silently
disable capabilities. Saved settings remain unchanged. Known unsupported optional
functions need no notice on every answer; availability instructions must be true.

CHA's web functions are custom functions, not Google's built-in Google Search or
URL Context tools. Those built-in incompatibilities do not by themselves settle
the custom-function combination. Live tests in block 3 must check the exact
request used here.

Preserve returned model content parts, including hosted `toolCall`/`toolResponse`
parts, IDs, and thought signatures. Return each custom `functionResponse.id`
unchanged. Preserve part order and signed data in streaming and non-streaming
continuations. Existing decoder/loop retention should be extended, not replaced.
Hosted-only parts must not be misclassified as custom calls.

### Step 8: Correct tool inspection and the final budget round

`tool_calls.cpp` currently assumes Google's first tool entry owns declarations
and every tool has `functionDeclarations`. Find an existing declaration group or
append one as needed, and inspect optional members safely. Test File Search both
before and after declarations. Derive web search, page read, and folder search
availability independently from the serialized request and effective mode.

When the existing local function-call/result budget is exhausted:

| Request | Required final-round behavior |
| --- | --- |
| Responses with stores | Remove all tools except `file_search`; set choice to auto. Hosted web search stops as it does today. Retain continuation output. |
| Google with stores | Retain `fileSearch`, model parts/signatures, and server-side invocation support. Set custom-function mode to `NONE`. |
| No stores | Keep existing budget behavior for every protocol, including Anthropic and Chat Completions. |

Hosted File Search does not consume CHA's function budget. Recompute the
availability block after changing tools/mode. Final instructions must say CHA web
functions are unavailable and explain using collected results, without forbidding
all further tools while hosted retrieval remains available. Never describe
available retrieval as already performed.

Google `NONE` compatibility with the mixed continuation requires live validation.
A rejection is an ordinary visible request failure with the attachment retained.
Do not strip signatures, drop stores, retry silently, or switch protocols/models.
Tests must cover exact serialization; live model support remains block 3's check.

Official references for current API/model checks:

- [OpenAI File Search](https://developers.openai.com/api/docs/guides/tools-file-search)
- [OpenAI deprecations](https://developers.openai.com/api/docs/deprecations)
- [Google native File Search](https://ai.google.dev/gemini-api/docs/generate-content/file-search)
- [Google mixed tools](https://ai.google.dev/gemini-api/docs/generate-content/tool-combination)
- [Google FileSearch schema](https://ai.google.dev/api/generate-content#FileSearch)

## 6. Native bridge and frontend contract

### Step 9: Expose bounded application operations

Implement exactly these native method names using block 1 operations. Every call
requires the current context epoch in the normal request envelope. Use existing
ID validation, unknown-field rejection, admission, and error conventions.

| Method | Inputs and result semantics |
| --- | --- |
| `notes.storeChoices` | Returns eligible service plus saved-key ID/name choices; no key value. |
| `notes.storeUpload` | Folder ID, service, key ID; promptly returns an accepted operation ID. Later status distinguishes editable preflight failure from remote failure. |
| `notes.storeRemove` | Folder ID and expected local store-record ID; promptly returns operation ID. |
| `notes.storeOperation` | Folder ID; returns no current control or its operation ID, kind, phase, completed/total counts, safe error, terminal outcome, and whether known cleanup remains. Supports reopening/reconnection. |
| `notes.storeCancel` | Exact operation ID; requests upload cancellation and its cleanup. It is not a general destructive cancel or failed-delete close. |
| `notes.storeClose` | Exact operation ID; acknowledges terminal state or releases a failed-delete reservation. Cannot discard active cleanup. |
| `notes.folderDelete` | Initial call: folder ID plus confirmed subtree store count. Retry: existing failed operation ID. `delete_anyway = true` requires that failed operation ID and performs only the explicit local fallback. |

Choose concrete DTO field names consistent with existing native conventions and
block 1 values, then make YAML, serialization, strict guards, and tests agree.
Reject invalid mixtures of initial/retry/fallback inputs. Do not let a stale
operation ID act on a replacement. Include enough terminal semantics for the UI
to distinguish local folder deletion from mere acceptance, warning completion,
or failed cleanup; no extra persistent state is needed.
Status and Close must still find the retained terminal control after successful
local deletion removed the folder row. Do not make recovery depend on that row
still existing.

Keep `notes.delete` for local-only deletion. If the selected subtree contains
store rows, it must reject and direct the caller to the confirmation flow. Count
incomplete/no-remote-ID rows and stores on empty folders. Admission also guards
active operations. No older or direct client may bypass backend cleanup.

Do not hold a bridge request until networking/indexing completes. A bridge
disconnect or request timeout does not cancel an application-owned operation or
release a failed-delete reservation. Status by folder ID recovers it. Explicit
Close, completion, or vault interruption controls retirement. No connection
ownership, operation expiry, separate delete-info endpoint, or extra retry/anyway
endpoint is required.

### Step 10: Add summary DTOs and strict TypeScript clients

Add an optional store summary to real-folder listing items: service, local
store-record ID, and ready/incomplete state. Derive it from current vault rows;
do not expose remote IDs in normal listings/menus. Existing note/annotation items
and items without stores remain valid. Do not invent a persistent progress field.

The full notes listing already supplies hierarchy and lets block 3 count all
store summaries in the selected subtree. Initial folder-delete admission must
recompute the count. On mismatch, return ordinary validation without deletion;
the UI refreshes and reconfirms. Failed retries use the retained reservation and
same operation ID, not a second confirmation mechanism.

Update these together:

1. `resources/dto.yaml` and native serialization/method policies.
2. `webapp/src/api/client.ts` interfaces and native client methods.
3. Strict response validation in `nativeClient.ts`; its current `isNoteItem`
   uses exact keys, so explicitly account for the optional summary without
   accepting arbitrary fields or malformed enum/ID/count values.
4. Generated `schema.d.ts` using the package command, not hand editing.
5. Existing wire fixtures, client fixture builders/mocks, protocol/router and
   wire compatibility tests.

Do not weaken validation to make the added field pass. Read/status methods have
the correct method policy; mutations retain epoch and main-process restrictions.
No remote ID/credential should leak through routine status except explicit safe
manual-cleanup diagnostic context. The frontend must not perform provider calls.

### Step 11: Extend saved-key usage

Add folder-store usage to the existing API-key `used_by` values, for example
`Folder store: Research (OpenAI)`. Read current folder names and both ready and
incomplete records. Do not make a second index/cache for this information.

Cover every response that returns API-key detail: list and key-edit/create
responses must remain consistent after rename or value replacement. A store still
counts when its model configuration was removed. Preserve existing key rename,
replacement, and deletion behavior; add no key foreign key or deletion veto.
Never rebind a store to another saved ID automatically.

## 7. Required validation

Use current test harnesses. Extend input/controller/session and multicast tests;
`tests/providers/unit_responses_api.cpp`, `unit_google_api.cpp`, and
`unit_provider_client.cpp`; bridge/protocol/wire tests; settings tests;
`webapp/src/api/nativeClient.test.ts`, wire fixtures, and related client fixtures.
Register any new source in the appropriate existing CMake target. Do not create
a second Google provider-client harness.

| Area | Required cases |
| --- | --- |
| Grammar/attach | Case-sensitive six-digit refs; surrounding whitespace; malformed/embedded/long strings stay ordinary text; zero/root/missing IDs; existing notes/annotations retain behavior; new ready folder; duplicate unavailable folder; upload-first notice; no model request. |
| Detach/lifetime | Missing/extra/invalid argument usage; unavailable/missing-name folder; repeated detach; ordinary note/file attachments unchanged; no remote calls; restart, Clear, cover/uncover, deletion, Welcome pruning and path retarget. |
| Journal race | Readiness/existence changes after callback but before insertion; persistent transaction rejects invalid new attach; notice failure rolls back association. |
| Matching | Exact endpoint/API/auth/key matrix; shared helper behavior; deleted/changed configuration; replacement upload with same folder ID or changed service; equal key values under different IDs do not match. |
| Dispatch/errors | Deduplicated IDs; combined omissions; correct target labels/detach commands; target continues without incompatible stores; one multicast target's count/access/model rejection does not cancel others; no silent retry/subset/text fallback. |
| Exclusions | No store lookup for Welcome maintenance, recipient detection, naming, compression, or other auxiliaries; builders prioritize maintenance defensively; existing no-store behavior retained. |
| Responses | One File Search tool alongside supported existing tools; auto/required preserved; hosted output replay with `store: false`; hosted retrieval is not a local call; budget keeps only File Search/auto; normal text/usage handling. |
| Google | Streaming/non-streaming File Search; declaration order variations; known unsupported mixed case warning; unknown-model rejection; all returned parts/IDs/signatures preserved; unchanged `functionResponse.id`; `VALIDATED` and final `NONE`; truthful availability. |
| Regression | Google Chat Completions stays on that protocol; subscription/proxy/no-store requests and Anthropic budget behavior unchanged; raw note text is absent from store payloads. |
| Bridge | Method names/policies, invalid fields/IDs/enums, stale epochs/operation/store IDs, bounded start, preflight/remote/warning distinction, reconnect status, no disconnect cancellation, active Close rejection, failed-delete retry/fallback authorization, changed count validation. |
| DTO/key usage | Optional folder summary with strict validation; old items valid; generated schema/fixtures agree; no routine remote IDs/secrets; incomplete usage and post-edit details; key deletion remains allowed. |
| Limits | Fake provider count failure plus successful detach recovery. Verified-limit attachment/dispatch tests only when an applicable current rule is established. |

During implementation, use focused filters and frontend file tests. At block
completion, run from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_tests cha_app_tests cha_bridge_tests
build/ninja/cha_tests
build/ninja/cha_app_tests
build/ninja/cha_bridge_tests
npm --prefix webapp run api-types
npm --prefix webapp run api-types:check
npm --prefix webapp run typecheck
npm --prefix webapp test
git diff --check
```

Run API generation when editing YAML, and verify the generated diff before the
final check. Exercise shared command behavior through the existing ChaWeb adapter
tests if its integration changes; do not add management endpoints there. Record
exact commands and actual executed tests. Unavailable checks remain unverified.
Live provider/model acceptance is not claimed by passing these tests.

## 8. Contract and handoff delivered to block 3

Block 3 must be able to use the typed client for all seven methods, list real
folders with optional store summaries, count stores in a subtree, recover an
active/failed operation by folder ID, and distinguish each of these states:

- Editable preflight validation failure with no remote allocation.
- Working upload/indexing with Cancel; cancellation cleanup in progress.
- Upload ready and committed, or confirmed cancelled cleanup.
- Normal upload failure with or without remaining cleanup.
- Standalone removal pending, failed, confirmed complete, or warning complete.
- Folder deletion pending; failed with retained reservation; local deletion
  committed with success or warning; failure that left the subtree intact.
- Stale context/operation rejected, and no current operation after retirement.

Use the implemented status/result types rather than reconstructing progress
from durable ready/incomplete rows. Do not require the UI to guess whether an
accepted delete already deleted local data. Identify actual DTO/client/type names
and any deliberate no-operation representation in the handoff. Include focused
test evidence and remaining live risks: Google mixed final-round support, current
native model support, OpenAI hosted-output replay, and provider deletion responses.

## 9. Independent review and completion

Review this block's diff against its actual starting point and inspect the
surrounding controller, adapter, continuation, and bridge callers. The reviewer
should independently verify representative serialized payloads and tests, not
rely on the implementation summary. Review for:

1. Correct journal-backed folder lifetime and no accidental changes to ordinary
   note refs, inline text, draft consumption, or detach semantics.
2. Per-target resolution of current ready stores, stable key identity, no direct
   store access across incompatible endpoints, and no corpus prompt fallback.
3. Maintenance/auxiliary exclusion before lookup and unchanged maintenance tools.
4. Immutable IDs and intact hosted output/Google signatures through all
   continuations, including budget exhaustion and non-streaming paths.
5. Tool-availability instructions that match the actual request and never claim
   retrieval happened merely because access was provided.
6. No silent retry, model/protocol switch, guessed count limit, or multicast-wide
   failure introduced to handle a provider rejection.
7. Native methods that preserve backend admission/cleanup semantics, bounded
   bridge calls, strict DTO validation, and connection-independent operation life.
8. Complete block 3 client contract, current key usage, and no key-deletion veto.
9. Reuse of existing loops and helpers with no second store client, attachment
   cache, general framework, or unnecessary compatibility machinery.

Report actionable findings with severity, file/line, reproduction conditions, and
the expected behavior. Separate code defects from unverified live API support.
Do not require block 3's UI as evidence for this block's completion.

This block is complete when its chat/inference/native integration works through
the existing harnesses and the required checks pass. Hand off the implemented
contracts, commands/results, material decisions, and remaining live checks.
