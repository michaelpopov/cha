# Review: folder stores technical design

Date: October 27, 2026

Reviewed: [store-design.md](store-design.md) against [store-prd.md](store-prd.md).

Method: I read AGENTS.md, CLAUDE.md, the PRD, and the design. I examined the
storage, session, input, provider, application, bridge, and tree-menu code that
the design names. I checked the provider claims against the current OpenAI and
Google documentation. This review changed no file other than this report.

Result: 3 P1 findings, 7 P2 findings, 6 P3 findings. Each finding has a type:
**definite defect**, **uncertainty**, or **optional simplification**.

---

## P1: must fix before implementation

### P1-1. A copied vault owns the same remote stores as its source

Type: definite defect.

Design: [store-design.md:173](store-design.md) (lines 173–179).

Problem. The design covers backups and the configuration merge. It does not
cover **Create vault** with a source vault. That path copies the full database
with `sqlcipher_export`
([vault_operations.cpp:511](../src/app/vault_operations.cpp),
[workspace_session_database.cpp:773](../src/storage/workspace_session_database.cpp)).
The new tables are copied too. The copy then has `ready` records with the same
remote store IDs, file IDs, and key IDs. Both vaults now own one set of remote
resources. This copy path is the normal way to make a child vault
(`.parent = copied->name`, vault_operations.cpp:472).

Failure scenario. The user makes a child vault from the parent. In the child,
the user selects **Remove from store** on “Research”. CHA deletes the OpenAI
vector store and its files. The parent still has a `ready` record. Each parent
answer in a conversation with “Research” attached sends the deleted store ID and
fails with a provider error. In the parent, **Remove from store** gets “not
found” and reports success. A folder deletion in either vault also deletes the
other vault's stores.

Evidence. PRD line 22: “Stores belong to the vault that created them. No global
store browser or cross-vault reference system is added.”

Smallest correction. In the copy path of `create_vault`, delete all
`folder_stores` rows in the new database before it is used (for example
`DELETE FROM copied.folder_stores` before `DETACH`). `folder_store_files` rows
cascade. Keep `session_folders` rows: they resolve to the copy's folders, which
have no store, and give the normal “upload first” omission. Add one vault-copy
test.

### P1-2. The vault-drain rule does not work with the current lifecycle lock

Type: definite defect (concurrency and implementation readiness).

Design: [store-design.md:343](store-design.md) (lines 343–351).

Problem. The design says that drain must “release lifecycle locks while waiting
so workers can save IDs”, and must fail or defer the switch if workers do not
stop in time. But `switch_vault` takes `std::lock_guard lifecycle(app.lifecycle_mutex)`
([vault_operations.cpp:759](../src/app/vault_operations.cpp)) and holds it
through `drain_for_maintenance` (line 786), the database close, the retarget,
and the reopen. `merge` does the same (line 874). A `lock_guard` cannot be
released inside `drain_for_maintenance`. The store workers save IDs under
`lifecycle_mutex`, because the design follows the compression pattern
(design lines 276–277).

Failure scenario. The user switches vault while Google indexing is polled. The
switch sets the cancel flag and waits for the store worker. The worker cannot
get `lifecycle_mutex` to save its state. The wait reaches the grace limit, and
the switch fails. This occurs every time an upload is active. To prevent it, an
implementer must change the lock structure of switch, merge, vault deletion,
R2 upload, and R2 download. The design does not list that work.

Evidence. Note audio solves the same problem with less machinery.
`run_note_audio` checks the epoch under `lifecycle_mutex` before it saves a
batch ID ([note_audio_operations.cpp:191](../src/app/note_audio_operations.cpp),
lines 191–193 and 298–303). After an interruption, a missing batch ID becomes an
honest “status unknown” warning (lines 282–285).

Smallest correction. Use the note-audio pattern. Drain sets the cancel flag of
each store operation and does not wait for it. A worker saves an ID only under
`lifecycle_mutex`, after it checks the epoch and the store-record ID. If the
check fails, the worker stops and writes nothing. The precommitted
`remote_outcome_unknown` flag (design lines 119–126) already makes the next
**Remove from store** honest. That matches PRD line 222 and current note-audio
behavior. Remove the “release lifecycle locks while waiting” and “fail/defer the
switch” text, and the tests for them.

### P1-3. A changed key value makes cleanup report false success and lose the remote IDs

Type: definite defect in the design's reasoning. The exact provider status codes
are an uncertainty (see below).

Design: [store-design.md:428](store-design.md) (lines 428–432), also lines
176–178 and 214–217.

Problem. The design says that changed or missing key values “can cause explicit
access errors, just as a manual key replacement can”. That is not true under its
own not-found rule. OpenAI vector stores and files belong to one project. A valid
key for another project normally gets “not found” for `vs_…` and `file-…`, not
401 or 403. The design counts an authenticated, structured not-found as a
confirmed deletion. It then clears the IDs, deletes the record, and writes a
success event.

Failure scenarios.

1. The user replaces the value of the saved key “Personal OpenAI” with a key
   from another project. PRD line 172 permits replacement.
2. A vault merge copies the source vault's `system/keys/<id>/` rows over the
   destination rows that have the same ID
   ([workspace_config_store.cpp:2884](../src/workspace/workspace_config_store.cpp),
   lines 2884–2886), and keeps the larger ID counter (lines 2889–2893). After the
   merge, the store's key ID can point to a different key. This is a silent key
   substitution, which design line 210 forbids.

In both cases, **Remove from store** or folder deletion gets not-found for every
resource. CHA reports success and deletes the only local copy of the IDs. The
remote store and files stay, and storage charges continue.

Evidence. PRD lines 172 and 200. The OpenAI Files delete reference does not
document the not-found response. Google does not document the response for a key
from another project; it can be 403 or 404.

Smallest correction. Keep the PRD rule that not-found completes cleanup. But when
any resource was not found, write the outcome event as a **warning**. List the
service, the key ID, and the not-found remote IDs. Say that a key for another
project gives the same result. Use a success event only when every deletion was
confirmed. This is one branch in the outcome event and needs no new state.

---

## P2: should fix

### P2-1. The Welcome association machinery is not needed, and it resolves IDs in the wrong vault

Type: optional simplification, with a latent defect.

Design: [store-design.md:130](store-design.md) (lines 130–133 and 148–154).

Problem. Welcome is in the Entrance forum. Its only member is the Assistant
([workspace.cpp:2030](../src/workspace/workspace.cpp)). Handle resolution uses
forum members only (workspace.cpp:2269–2291). So every Welcome answer is a
maintenance answer ([session_controller.cpp:744](../src/session/session_controller.cpp)),
and maintenance answers never receive stores (PRD line 188, design lines
532–536). A Welcome folder association can never give access. But the design
adds a post-commit runtime command that prunes Welcome, a prune on read, and an
attach/delete race test.

There is also a latent defect. CHA creates the Welcome journal once per process
([session_repository.cpp:211](../src/storage/session_repository.cpp)). A vault
switch does not recreate it: `retarget` changes only the vault path (lines
159–165). Design lines 131–132 say Welcome references “resolve against the active
vault”. After a switch, a folder ID from vault A resolves in vault B. The prune
on read removes only IDs that are missing. It does not remove IDs that exist in
both vaults.

Smallest correction. Save a Welcome association through the shared attach path
and never resolve it. Remove the post-commit prune command, the prune on read,
and the Welcome race test. Add one sentence: Welcome associations are never
resolved, because the Welcome journal survives vault switches.

### P2-2. The design gives two incompatible attach-validation rules

Type: inconsistency, with an optional simplification.

Design: [store-design.md:135](store-design.md) (lines 135–138), against lines
471–473 and 148–149.

Problem. Lines 135–138 require validation and insertion in one journal write
transaction. Lines 471–473 say that validation goes through the
`WorkspaceConfigStore` callbacks. The journal has its own SQLite connection
([session_database.cpp:597](../src/storage/session_database.cpp)). The callbacks
use the store's connection ([session_open.cpp:35](../src/session/session_open.cpp)).
One transaction cannot contain both. The implementer must either put folder and
store SQL into `SessionJournal`, which couples it to the new schema, or accept a
race. The race can only leave a dangling row. `note_items` uses `AUTOINCREMENT`
([note_storage.cpp:640](../src/storage/note_storage.cpp)), so a deleted folder ID
is never reused. A dangling row cannot point to another folder later.

Smallest correction. Validate through the existing callback, as `add_note` does.
When CHA reads attachments for a request or for `/detach`, ignore rows whose
folder does not exist. This also satisfies PRD line 164 (“Deleting the folder
removes its associations”) as observed behavior. The single `DELETE` in the
folder-delete transaction is optional. Remove the write-transaction requirement.

### P2-3. The Welcome database may not get the `session_folders` table

Type: definite defect if implemented as written.

Design: [store-design.md:68](store-design.md) (lines 68–71, and 143–144).

Problem. The design calls the schema helper “when creating a vault and when
opening an existing schema-v2 vault”. The Welcome database is neither.
`create_session_database` makes it ([session_repository.cpp:212](../src/storage/session_repository.cpp)
→ [session_database.cpp:523](../src/storage/session_database.cpp) →
`create_workspace_session_schema`). The design also adds
`DELETE FROM session_folders` to `SessionJournal::clear_history`, which Welcome
uses.

Failure scenario. If the helper is not called from
`create_workspace_session_schema`, **Clear** in Welcome fails with “no such
table: session_folders”. A folder attachment in Welcome fails too.

Smallest correction. Call the helper from `create_workspace_session_schema`
([workspace_session_database.cpp:461](../src/storage/workspace_session_database.cpp),
next to `create_note_items_schema`) and from the open path. Add a Welcome
**Clear** test.

### P2-4. The request-limit machinery has no verified limit to enforce

Type: uncertainty that leads to an optional simplification. It needs your
decision, because it affects acceptance criterion 13.

Design: [store-design.md:476](store-design.md) (lines 476–493 and 503–508), also
lines 261–267.

Problem. The only documented store-count limit is OpenAI's “current maximum of
two vector stores” for deep-research models. OpenAI shut down
`o3-deep-research` and `o4-mini-deep-research` on July 23, 2026. The Responses
`FileSearchTool` reference gives no maximum for `vector_store_ids`. The Google
File Search guide gives no maximum for stores per request or per project. (The
PRD says the Google `FileSearch` schema gives none; I could not read that schema
section.) So the limit helper, the attachment-time grouping by service and key,
the generation-time recheck, and the preflight error on `ProviderRequestInput`
enforce nothing for any model that a user can configure today.

The local byte check (lines 261–267) also cannot fail. A note document is
limited to 12 MiB ([note_storage.h:20](../src/storage/note_storage.h)). That is
far below Google's 100 MB and OpenAI's 512 MB. No per-store file-count limit is
documented for either provider.

Smallest correction. State that no verified per-request limit exists as of the
design date. Do not add the limit helper, the grouping, the recheck, or the
preflight-error field. Keep the provider-error path with folder names and
`/detach` commands (lines 509–513). That path already covers the “unexpected
provider limit” clause of acceptance criterion 13. Remove the local byte check.
Confirm with the PRD owner that the first clause of criterion 13 cannot be
tested until a verified limit exists.

### P2-5. Forced hosted web search uses a `tool_choice` form that the API reference does not list

Type: probable defect. I could not verify the API claim.

Design: [store-design.md:549](store-design.md) (lines 549–553).

Problem. For `web_search = required` with stores, the design forces “a choice
specifically targeting the hosted web-search tool”. CHA's hosted tool type is
`web_search` ([responses_api.cpp:192](../src/providers/responses_api.cpp), lines
192–204). The Responses create reference lists these hosted `tool_choice` types:
`file_search`, `web_search_preview`, `computer`, `computer_use_preview`,
`computer_use`, `code_interpreter`, `image_generation`. `web_search` is not in
the list. The `allowed_tools` form exists, but the reference does not say that
it accepts a hosted `web_search` entry.

Failure scenario. Every answer from a Responses configuration with
`web_search = required` and an attached store fails with HTTP 400.

Evidence. The present `required` choice does not force hosted search either.
CHA's own `web_search` and `web_read` functions can satisfy it
([tool_calls.cpp:30](../src/providers/tool_calls.cpp), lines 30–37). The PRD
does not ask for this change.

Smallest correction. Keep `tool_choice = "required"` as it is. Remove the
“required web search remains required” test (line 748). If strict hosted search
is necessary later, first test
`{"type":"allowed_tools","mode":"required","tools":[{"type":"web_search"}]}`
with a live request.

### P2-6. Folder deletion makes an unknown outcome a permanent cleanup failure

Type: definite defect (two inconsistent rules).

Design: [store-design.md:442](store-design.md) (lines 442–447), against lines
434–440.

Problem. For **Remove from store**, CHA reports the unknown outcome, saves a
warning, and clears the record. For folder deletion, the same uncertainty
“counts as cleanup failure”, and CHA keeps the subtree. CHA cannot discover
remote resources, so **Retry** can never resolve the uncertainty.

Failure scenario. A crash during an OpenAI file upload leaves
`remote_outcome_unknown = 1`. Later, the user deletes the parent folder. CHA
deletes all known resources and then shows the failure dialog. **Retry** does
the same work again and fails again, every time. The only exit is **Delete
anyway**, although CHA has already done all the cleanup that it can do.

Smallest correction. Use one rule in both flows. When all known resources are
gone and only uncertainty remains, save the uncertainty warning in the same
transaction as the local subtree deletion, and finish. If you prefer to keep a
confirmation, at minimum do not show **Retry** for an uncertainty-only result.

### P2-7. `pending_google_operation` is saved but nothing reads it, and the per-document waits are slow

Type: optional simplification.

Design: [store-design.md:114](store-design.md) (lines 84, 89, 114–117, 303–304,
409, and 665–666).

Problem. The design says that forced deletion of the Google store is sufficient
even if an operation name is lost (lines 124–126). Uploads never resume after a
restart (PRD line 221, design lines 339–341). So no code reads the saved
operation name, except the text of the **Delete anyway** warning.

The design also waits for each document's indexing before it uploads the next
document (lines 303–304 and 409). Google's examples poll every 5 seconds, so each
note costs at least one poll interval. A folder with 300 notes takes 25 minutes
or more on Google. OpenAI is similar, with one status poll for each file.

Smallest correction. Remove the column and its term in the `ready` CHECK. Keep
operation names in worker memory. Optionally, upload all documents first (save
IDs as the design specifies), then poll the in-memory list of pending files or
operations until each one is terminal. This keeps one worker, the per-note error
reasons, and no new state. Keep the final Google store-count check.

---

## P3: lower priority

### P3-1. User cancel during an allocation request causes avoidable uncertainty

Type: optional simplification.

Design: [store-design.md:318](store-design.md) (lines 318–324, and 119–123).

Problem. **Cancel** aborts the active transfer. If cancel arrives during
`POST /files` or `POST /vector_stores`, CHA does not learn whether the resource
exists. The flag stays set, and cleanup ends with “manual provider cleanup may
still be needed”. An unknown OpenAI file can remain. The design's own rule at
lines 323–324 (“Still parse and persist resource IDs in a response received after
cancellation”) works only if CHA lets the request finish.

Smallest correction. On user cancel, let an in-flight allocation request finish
(the normal timeout limits it). Save the returned ID, then clean up. Abort at once
only polling waits and requests that cannot allocate. Shutdown still aborts all
requests. PRD line 85 says “where possible”, so this agrees with the PRD.

### P3-2. Budget exhaustion keeps hosted web search on Responses

Type: optional simplification (unrequested behavior change).

Design: [store-design.md:622](store-design.md) (lines 622–623).

Problem. Today the final round removes all Responses tools, including hosted
`web_search` ([provider_client.cpp:854](../src/providers/provider_client.cpp),
lines 854–857). The design keeps “supported hosted tools” when stores are
present. This changes the existing budget behavior. Also, the availability block
then says that web search is available (tool_calls.cpp:270–271), while the
directive says that web tools are unavailable (provider_client.cpp:863–866).

Smallest correction. In the final round, remove all tools except `file_search`.
Then only the PRD requirement (keep File Search) changes the request.

### P3-3. The bridge and folder-delete state can be smaller

Type: optional simplification.

Design: [store-design.md:670](store-design.md) (lines 670–674, 694–696, and
708–713).

Problem.

- `notes.folderDeleteInfo` returns data that the client already has. The design
  adds a store summary to each folder in the listing (lines 702–705), and the
  client has the full tree.
- The failed-attempt identity, the confirmed store identities, and the
  “refreshed confirmation” state are necessary only because the subtree
  reservation is released between attempts.
- Cancellation on connection loss needs a new link between store operations and
  bridge connections. Compression and note audio have no such link.

Smallest correction. Calculate the confirmation count in the client from the
listing. Keep the subtree reservation in the failed-delete control until
**Cancel**, **Delete anyway**, dialog close, or vault change. Then **Retry** is
the same `notes.folderDelete` call, without an identity check, and the UI needs
no “refreshed confirmation” state. Key the operation status by folder ID, so that
a reloaded UI can find a running operation. Do not cancel on connection loss.
This removes one or two bridge methods and one UI state.

### P3-4. The Ref ID parser change is outside the PRD scope

Type: optional simplification (unrequested behavior change).

Design: [store-design.md:460](store-design.md) (lines 459–462).

Problem. The design makes `parse_note_reference` accept more than six base62
digits ([text_input.cpp:44](../src/runtime/text_input.cpp)). Six digits cover
about 56.8 billion IDs. The change alters current behavior: today an input such
as `##ABCDEFG` is ordinary text, and after the change it becomes a reference
error. The PRD does not ask for this.

Smallest correction. Keep the six-digit parser and use it for folders too.

### P3-5. UI approval is not recorded

Type: process gap.

Design: [store-design.md:788](store-design.md) (lines 7–9 and 787–789).

Problem. AGENTS.md requires a visual proposal and your explicit approval before
any UI change. The PRD calls its sketches “proposals for review” (PRD line 125).
The design adds states that have no sketch: **Indexing notes…**, **Cleaning
up…**, removal pending and removal error, upload failure with **Close**, the
zero-key error, and the refreshed confirmation. But lines 787–789 say that no
additional product decision is required.

Smallest correction. Change lines 787–789: UI work starts only after you approve
the dialog states. If you accept P3-3, remove the refreshed-confirmation state.

### P3-6. Known failures on real data that the PRD accepts

Type: known failure (reported because the review policy requires it).

Design: [store-design.md:173](store-design.md) (lines 173–175). PRD line 204.

1. Events keep only the latest 1000 entries
   ([event_storage.h:14](../src/storage/event_storage.h)). The **Delete anyway**
   warning is the only record of the remote IDs. Normal work can delete it. For
   example, compression of 500 notes writes 1000 events.
2. **Download from R2** replaces the active vault database
   ([vault_operations.cpp:1059](../src/app/vault_operations.cpp)). An older copy
   can bring back `ready` records for stores that were removed. Requests then fail
   until the user removes them. An older copy can also drop the records of newer
   stores, and those remote stores then have no local record.

Smallest correction. Optional: write each remote ID to the diagnostic log at
info level when CHA saves it, and the **Delete anyway** list at warning level.
The log is outside the database. Otherwise, accept these limits and state them in
the design.

---

## Design claims that I verified

- The endpoint table (design lines 188–191) agrees with `provider_endpoint`
  ([character_config.cpp:235](../src/characters/character_config.cpp)).
- The two Google tool assumptions in `tool_calls.cpp` are correctly identified:
  `append_tool` writes into `tools[0]` (line 27), and `update_tool_instructions`
  calls `tool.at("functionDeclarations")` (line 256). A `fileSearch` entry breaks
  both.
- The Google decoder keeps all response parts, and the Responses decoder keeps
  all output items, for continuation (google_api.cpp:119, responses_api.cpp:325).
- The note-reference callback prefers compressed text
  ([session_open.cpp:35](../src/session/session_open.cpp)), so the export must
  not use it.
- `note_delete` owns a `BEGIN IMMEDIATE` transaction, so the refactor at design
  line 165 is necessary.
- Google: `includeServerSideToolInvocations` is required for the combination,
  and “Default to `VALIDATED` mode (`AUTO` mode is not supported)” when that flag
  is set. File Search is a supported combined tool for Gemini 3 and 4 (preview).
  File Search cannot be used with Google Search or URL Context.
- Google: `DELETE …?force=true` deletes the documents. The upload operation path
  is `fileSearchStores/*/upload/operations/*`. The store has `activeDocumentsCount`,
  `pendingDocumentsCount`, and `failedDocumentsCount`. `text/markdown` is
  supported. The size limit is 100 MB for each document.
- OpenAI: “Delete a file and remove it from all vector stores.” `.md` files are
  supported. The size limit is 512 MB and 5,000,000 tokens for each file.

## Implementation readiness

The design is not ready. Fix P1-1, P1-2, and P1-3 first: they cause shared remote
ownership, a vault switch that cannot complete, and a false cleanup success. The
P2 corrections mostly remove machinery and make the design smaller. After these
changes, the code boundaries, the schema, and the test plan agree with the
repository.

Integration checks that are still open:

1. Google: is `functionCallingConfig.mode = NONE` accepted together with
   `includeServerSideToolInvocations`, `fileSearch`, and replayed
   `toolCall`/`toolResponse` parts? The documentation does not say. The design
   must decide what to do if Google rejects it, before implementation starts,
   because the design forbids dropping the store.
2. Google: does a `done` upload operation mean the document is indexed, and are
   the store counts immediately consistent? The documentation does not say. The
   final count check covers the first question.
3. OpenAI: is a replayed `file_search_call` input item accepted with
   `store: false`? The documentation does not say.
4. Both providers: the response to a delete with a valid key from another project
   (see P1-3).
5. Google labels its `generateContent` File Search page “Legacy”. The main guide
   now uses the Interactions API. Confirm that `generateContent` File Search
   stays supported for the models that you use.
6. OpenAI: does the deep-research two-store limit apply to the deep-research
   replacement model? This affects P2-4.
7. Gemini 2.5 Flash is not in Google's File Search model list. A visible error is
   the expected behavior.

## Recommended fix order

1. P1-1 (vault copy), P1-3 (not-found outcome event), P1-2 (vault drain).
2. P2-3 (Welcome schema), then P2-1 and P2-2 together (association lifecycle).
3. P2-4 (limit machinery; needs your decision on criterion 13) and P2-5
   (`tool_choice`).
4. P2-6 (uncertainty rule) and P2-7 (Google operation column, polling).
5. P3 items, then UI approval (P3-5) before UI work starts.

## Sources

- [Google File Search (Generate Content)](https://ai.google.dev/gemini-api/docs/generate-content/file-search)
- [Google File Search guide](https://ai.google.dev/gemini-api/docs/file-search)
- [Google File Search stores API](https://ai.google.dev/api/file-search/file-search-stores)
- [Google File Search documents API](https://ai.google.dev/api/file-search/documents)
- [Google tool combinations](https://ai.google.dev/gemini-api/docs/generate-content/tool-combination)
- [OpenAI File Search](https://developers.openai.com/api/docs/guides/tools-file-search)
- [OpenAI retrieval](https://developers.openai.com/api/docs/guides/retrieval)
- [OpenAI deep research](https://developers.openai.com/api/docs/guides/deep-research)
- [OpenAI web search](https://developers.openai.com/api/docs/guides/tools-web-search)
- [OpenAI Responses create reference](https://developers.openai.com/api/reference/resources/responses/methods/create)
- [OpenAI Files delete reference](https://developers.openai.com/api/reference/resources/files/methods/delete)
- [OpenAI deprecations](https://developers.openai.com/api/docs/deprecations)
