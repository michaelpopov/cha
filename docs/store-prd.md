# Folder stores

Status: Proposed PRD

Date: October 27, 2026

## Purpose

Upload a folder's notes to an OpenAI or Google search store, then attach the folder to a conversation with its existing CHA Ref ID. Compatible models can search the uploaded notes while answering. CHA keeps the local notes as the source documents and remembers which remote store belongs to the folder.

The first version should be a small extension of folders, reference attachments, and provider requests. It does not need a local vector database or a general document synchronization service.

## Proposed scope

Confirmed scope: upload creates **a fixed snapshot**, refreshed by removing and uploading again, and includes **all ordinary notes in the folder and its subfolders**.

- One folder has at most one store, on either OpenAI or Google. To change providers, remove the existing store first.
- Upload is an explicit action. Editing, adding, moving, renaming, or deleting notes does not update an uploaded snapshot.
- Use saved note text, including editable notes. Do not require notes to be read-only.
- Support real folders in the open vault. The synthetic vault root is not a folder that can be uploaded or referenced.
- Use the providers' direct, API-key-authenticated services. OpenAI subscription login, OpenRouter, Azure, Vertex AI, and other proxies are outside this version.
- Stores belong to the vault that created them. No global store browser or cross-vault reference system is added.

## Provider facts and integration boundary

OpenAI calls the resource a **vector store**. Its hosted `file_search` tool accepts vector store IDs in Responses API requests. Files must finish processing before use. This makes CHA's existing Responses implementation the natural integration point. [OpenAI file search](https://developers.openai.com/api/docs/guides/tools-file-search)

Google calls the resource a **File Search store**. It supports direct uploads and a native Gemini file-search tool that receives store resource names. Upload completion includes asynchronous indexing. [Google File Search](https://ai.google.dev/gemini-api/docs/file-search)

CHA already supports native Google inference on `main` through `api = "google"`, shown as **Google (Gemini)** in provider settings (commit `94484310`). The existing adapter handles `generateContent` and `streamGenerateContent`, saved-key authentication, conversation/system context, streaming, cancellation, usage, and function-call continuation with preserved response parts and thought signatures. Reuse [the Google adapter](../src/providers/google_api.cpp) and [provider client](../src/providers/provider_client.cpp); no new inference protocol, transport, or function-call loop is required.

The remaining Google work is store creation/upload/indexing/deletion, vault and conversation associations, and adding `fileSearch.fileSearchStoreNames` to the existing native request's `tools`. Keep the configured model, API format, endpoint, and supported generation settings. Google store access requires a direct **Google (Gemini)** configuration using `https://generativelanguage.googleapis.com/v1beta` and the store's saved key. A Google configuration using OpenAI-compatible Chat Completions remains on that protocol and cannot use the store; do not switch protocols when an attachment is added. No new provider setting is needed. [Gemini File Search request](https://ai.google.dev/gemini-api/docs/generate-content/file-search)

Keep CHA's existing `web_search` and `web_read` function tools alongside File Search when the selected Gemini model supports the combination. These are distinct from Google's built-in Google Search and URL Context tools. Google documents mixed built-in/function tools for Gemini 3 and 4 models as a preview feature. Mixed requests need `includeServerSideToolInvocations`, the documented `VALIDATED` function-calling mode, and replay of returned content parts, IDs, and signatures. Extend the existing request builder and continuation path only as needed for those requirements; provider-hosted File Search executes remotely. [Google tool combinations](https://ai.google.dev/gemini-api/docs/generate-content/tool-combination)

For a model known not to support mixed tools, send File Search without the optional CHA web tools, log a warning, and keep the model's tool-availability instructions accurate. Leave saved settings unchanged and do not add a notice to every answer. Unexpected provider rejections use the existing request error path; do not silently retry without the store or change models. Do not pass stores to the Assistant's maintenance-tool answers, which must retain their configuration tools. Requests without eligible stores keep their existing behavior. Ignore unused or obsolete settings with a log warning, as elsewhere in CHA.

## User flow and proposed UI

### Folder menu

Add **Copy Ref ID** to every real folder's menu, whether uploaded or not. Add **Upload to store** when it has no store. Replace that action with **Remove from store** when it has a ready store or an incomplete upload that needs cleanup.

Existing menu items retain their order and behavior. The sketches show only the additions:

```text
No store                     Store registered
┌───────────────────────┐    ┌───────────────────────┐
│ Copy Ref ID           │    │ Copy Ref ID           │
│ Upload to store       │    │ Remove from store     │
└───────────────────────┘    └───────────────────────┘
```

### Upload

1. **Upload to store** opens a dialog with **Provider**, **API key**, **Cancel**, and **Upload**. Provider choices are **OpenAI** and **Google**. Reuse saved API keys; never ask for a raw key in this dialog.
2. Offer only keys referenced by at least one compatible direct model configuration: OpenAI Responses with API-key authentication, or **Google (Gemini)** (`api = "google"`) on the direct Gemini endpoint above. A key used only by OpenAI or Google Chat Completions configurations is not eligible. Deduplicate by saved key ID. Select the key automatically when there is exactly one; otherwise require a selection. With no eligible key, show an actionable error directing the user to the existing provider/API-key settings.
3. **Upload** validates the folder, credential, and saved note export before remote creation. An empty folder, no nonempty notes, or invalid required configuration leaves the dialog open with an error.
4. Create a new remote store and upload the captured notes. Keep the dialog open with concise status such as **Uploading 3 of 12 notes…** or **Indexing notes…**. Disable duplicate submission and provider/key changes while working, but keep **Cancel** available. Network work must not block the UI thread.
5. On success, close the dialog after every included note is indexed and the ready association is saved. Write a success event with folder, provider, and note count. The folder menu now offers **Remove from store**.
6. A failure stays visible in the dialog with **Close** available. If cleanup is incomplete, direct the user to **Remove from store**; otherwise a new upload can be started. Do not report a partially indexed folder as uploaded.

```text
┌─────────────────────────────────────────────┐
│ Provider                                    │
│ [ OpenAI                                ▾ ] │
│                                             │
│ API key                                     │
│ [ Personal OpenAI                       ▾ ] │
│                                             │
│                       [ Cancel ] [ Upload ] │
└─────────────────────────────────────────────┘
```

While working, show status in place of the editable fields:

```text
┌─────────────────────────────────────────────┐
│ Uploading 3 of 12 notes…                     │
│                                             │
│                                  [ Cancel ] │
└─────────────────────────────────────────────┘
```

Before submission, Cancel closes the dialog without remote work. During upload or indexing, Cancel stops further upload/polling work, cancels the current network request where possible, and uses the same cleanup path as an upload failure. Show **Cleaning up…** while cleanup runs. Close when cleanup succeeds; otherwise show the cleanup error with **Close** available and retain **Remove from store** for retry. Cancellation must not mark the upload ready, including when a late success response arrives; retain any returned resource IDs for cleanup. It does not promise to cancel processing already accepted by the provider. Application shutdown uses the interruption rules below. No background job interface or separate cancellation record is needed. Give the dialog an accessible name, **Upload folder to store**.

### Removal

**Remove from store** opens the existing style of confirmation dialog, identifying the folder and provider, with **Cancel** and **Remove**. Removal keeps the local notes. Use concise text such as **Remove “Research” from OpenAI?** An incomplete upload uses the same action.

During removal, show concise pending status and disable duplicate submission. A failure remains in the dialog with a close action and an actionable reason; retry uses the retained association. Folder deletion also offers **Delete anyway** after remote cleanup fails, as described below.

```text
┌─────────────────────────────────────────────┐
│ Remove “Research” from OpenAI?               │
│                                             │
│                       [ Cancel ] [ Remove ] │
└─────────────────────────────────────────────┘
```

### Folder deletion

If the subtree has stores, show their count in the existing folder-delete confirmation, even when the folder has no local notes. While cleanup runs, show pending status and disable repeated deletion. After a remote cleanup failure, keep the local folder until the user chooses **Retry**, **Cancel**, or **Delete anyway**. The warning must make clear that Delete anyway deletes local notes and can leave remote data and charges behind.

```text
┌───────────────────────────────────────────────────┐
│ Delete “Research” and all its notes?               │
│ 2 remote stores will also be removed.             │
│                             [ Cancel ] [ Delete ] │
└───────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────┐
│ Removing remote stores…                           │
└───────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────┐
│ Could not remove 1 OpenAI store: access denied.    │
│ Delete anyway removes the local folder and notes. │
│ Remote data and charges may remain.               │
│                                                   │
│             [ Cancel ] [ Retry ] [ Delete anyway ] │
└───────────────────────────────────────────────────┘
```

Use the actual provider, count, and error. These sketches are proposals for review.

## Uploaded content and snapshot behavior

Capture a consistent set of saved notes when Upload starts. Unsaved editor changes are not included. Reuse the existing note-to-Markdown exporter with full text, not compressed summaries. Include the note title, its relative folder path, and its CHA Ref ID in each exported document. Use one UTF-8 Markdown file per note, with a readable name and the note ID to distinguish duplicate titles.

Reuse the exporter's handling of headings, lists, links, code, comments, and image alt text. Do not upload editor JSON, image binaries, generated audio, original imported files, or fetched link targets. Exclude generated `ANNOTATIONS` items, which duplicate information derived from ordinary notes. Skip empty note bodies; an otherwise empty export is a validation error. Do not silently truncate or skip an oversized or failed nonempty note.

The upload is all-or-nothing from the user's perspective: ready means every included document succeeded. Provider file-size, count, and quota limits must be respected, with the failing note and reason identified where available. Use provider defaults for chunking, embeddings, and retrieval settings; no tuning controls are needed.

After upload, the remote content remains the captured snapshot. Renaming or moving the folder does not change its identity, store, or Ref ID. Note edits and note deletions do not remove text from an existing snapshot. Refresh by **Remove from store**, then **Upload to store**. No stale-content badge, content hash, synchronization schedule, or automatic re-upload is required.

Parent and child folders may each have their own store. Each upload is independent, even if it contains overlapping notes. Removing a child's store does not edit the parent's snapshot.

Upload sends the selected note snapshot to the chosen provider's account. Closing CHA or clearing a conversation does not remove it. Provider charges can apply to storage, indexing, retrieval, and model usage; use the providers' current pricing rather than add a cost estimator or automatic expiry policy. [OpenAI retrieval costs](https://developers.openai.com/api/docs/guides/retrieval#pricing), [Google File Search pricing](https://ai.google.dev/gemini-api/docs/file-search#pricing)

## Folder references in conversations

Use CHA's existing Ref ID format, such as `##00000A`, derived from the folder's existing item ID. Do not expose a provider's remote store ID as the folder's Ref ID. Reference IDs are vault-local and stay stable across store removal and replacement.

Follow the current note-reference gesture:

1. The user copies the folder's Ref ID and submits it as the entire chat input.
2. CHA resolves it in the current vault and checks known request limits as described below. A ready store within those limits attaches the folder and produces an existing-style transcript notice, for example **Added folder “Research” (OpenAI).** No model request is made by this submission.
3. The user sends an ordinary question. Eligible models receive access to the attached store.

An ID inside a sentence retains today's ordinary-text behavior; inline reference parsing is outside this version. A folder with no ready store produces **Upload this folder to a store before adding it to the conversation.** It must not fall back to inserting every note into the prompt.

Store the conversation's association with the **folder**, not a copied corpus or a fixed remote store ID. Repeating the reference does not duplicate it. Associations survive restart and cover/uncover, and Clear removes them, matching the current attachment lifetime. Ordinary note references keep their current text-snapshot behavior.

Add one command, **`/detach <Ref ID>`**, for removing a folder attachment from the current conversation without clearing its history or deleting any notes or remote resources. It works even when the store is unavailable. A repeated detach reports that the folder is not attached and makes no change. This command applies only to folder attachments; ordinary note attachment behavior is unchanged. No model request is made.

```text
You: /detach ##00000A
     Removed folder “Research” from this conversation.
```

This small escape route is needed because provider changes, replacement uploads, or an unexpected service limit can make a previously valid attachment set unusable. Error notices should name the affected folders and show the applicable detach command.

Resolve the folder's current store at the start of each model request. Removing a store makes its attached folder unavailable. Uploading that same folder again makes the replacement available to already-attached conversations, including if the provider changes. Deleting the folder removes its associations. Historical answers and notices remain; removing a store does not erase past chat text.

Use existing chat notices to identify an unavailable or incompatible folder for the affected turn. Combine multiple omissions into one concise notice rather than one message per folder. Do not claim the model searched a store merely because access was supplied; the provider/model decides whether to retrieve.

## Matching a store to a model request

A CHA provider configuration identifies a model connection, not a provider account. Several configurations can use the same service and credential. Match stores by **service plus saved API-key ID**, and require a supported direct endpoint and inference protocol. Do not match by a display name, model-name prefix, or configuration ID alone.

This deliberately avoids account discovery. Two different saved keys are treated as different connections even if they happen to belong to the same remote project. Users can reuse one saved key across model configurations to share a store. Replacing that key's value is supported if the replacement still has access to the same project; access failures remain explicit.

| Request | Behavior |
| --- | --- |
| Direct OpenAI Responses with the store's saved key | Add eligible OpenAI vector store IDs to one `file_search` tool. |
| Direct Google (Gemini), `api = "google"`, with the store's saved key | Add eligible store names to `fileSearch.fileSearchStoreNames` in the existing native request. |
| Google through OpenAI-compatible Chat Completions | Omit the store and identify the incompatible API format; keep the configured protocol. |
| Different provider, different saved key, subscription login, or unsupported endpoint/protocol | Omit that store and show a concise notice; the model can still answer normally. |
| Eligible store rejected as missing, unauthorized, or unsupported by the selected model | Show the affected request's error. Do not retry silently without the store. |

Deduplicate remote IDs and check any verified per-request limit **before adding a new folder attachment**. Count ready stores by service and saved key, against the limits of compatible model configurations in that conversation. Reject only the new attachment if it would exceed a known limit; leave the existing conversation intact and explain which folder can be detached. Check again at generation time because model settings and a folder's current store can change. If the provider rejects the count, preserve the conversation and direct the user to `/detach`; do not silently select a subset or retry without the stores.

Do not invent one universal numeric limit. OpenAI documents a maximum of two vector stores specifically for deep research; Google's `FileSearch` schema does not state a store-count maximum. Use limits verified for the supported API/model and retain the error path for undocumented or changed limits. No account-discovery or limit-monitoring service is needed. [OpenAI deep research limits](https://developers.openai.com/api/docs/guides/deep-research#vector-stores), [Google FileSearch schema](https://ai.google.dev/api/generate-content#FileSearch)

In a conversation with several responding characters, resolve each target independently. One target's store error must not cancel other valid targets.

Add stores only to ordinary conversation-answer requests. Exclude recipient selection, session naming, note compression, and the Assistant's maintenance-tool answers in the welcome conversation. These Assistant turns are chat answers with configuration tools, not a background task. File Search joins each provider's supported tool list under the compatibility rules above. Keep eligible store tools on subsequent requests belonging to the same ordinary answer. Reuse Google's existing function-call loop and preserve returned content parts and signatures. Reaching CHA's function-call budget must disable those functions without accidentally removing hosted File Search access.

Do not copy the uploaded corpus into request text as a compatibility fallback. Models from another provider can still see earlier answers in shared conversation history, as they do today; provider matching limits direct store access, not ordinary conversation history.

The first version uses normal answer rendering. Reliable clickable file citations, source previews, and a retrieval-results panel are deferred; CHA does not currently render provider file-citation metadata. Do not invent citations or imply that model-written source names are verified citation links.

## Removal and folder deletion

Attempt remote cleanup before discarding the local association. The explicit **Delete anyway** fallback below is the exception. For OpenAI, cleanup includes both the vector store and the Files resources created for this upload. Record uploaded file IDs as they are returned, including files that fail to attach to the store. Do not reuse these files across stores. OpenAI's Files deletion API removes a file from vector stores as well. [OpenAI file deletion](https://developers.openai.com/api/reference/resources/files/methods/delete)

For Google, delete the store with `force=true` so its documents are deleted too. Prefer direct-to-store upload so a second set of temporary Files resources does not need management. [Google store deletion](https://ai.google.dev/api/file-search/file-search-stores#method:-filesearchstores.delete)

Mark a store unavailable before cleanup begins. For **Remove from store**, clear the local association only after all known remote resources are deleted or confirmed absent. An authenticated not-found result for the expected resource is an idempotent cleanup success. Authentication, permission, network, and storage errors are not remote deletion success. Keep the remaining identifiers and show an actionable error so removal can retry. Do not automatically create a replacement.

Deleting a folder must find stores attached to that folder **and every descendant folder** before the current recursive database delete runs. Attempt cleanup first, then delete the local subtree on success. If remote cleanup fails, including because credentials are missing or access is permanently lost, preserve the local subtree and offer the failure choices shown above. **Retry** attempts only the remaining cleanup. **Cancel** keeps the local folder and remaining associations. Stores already removed stay removed.

**Delete anyway** deletes the local subtree, its store records, and its conversation associations without further remote requests. Save a warning event with the affected folder names/IDs, providers, and all known remote store/file IDs whose deletion was not confirmed, so the user can remove them manually through the provider. State when a remote ID is unknown. Save this event and perform local deletion in one vault transaction; an actual local database failure remains an error. Do not report remote cleanup as successful or schedule later cleanup. The warning uses normal Events retention, not a new orphan registry.

A folder with no stores retains today's local deletion behavior. Deleting an individual note or a child folder does not rewrite a surviving ancestor's uploaded snapshot. **Remove from store** continues to preserve notes and require cleanup; **Delete anyway** belongs only to folder deletion and is never an automatic fallback.

Requests already sent to a provider may finish using their captured store access. Removal prevents new requests from using it; it cannot retract earlier requests or answers.

## Minimum persistence and failure handling

Extend the existing vault database. Keep one small store record per uploaded folder containing service, saved key reference, remote store identifier, and whether it is ready or incomplete. Save the creation/upload identifiers as soon as they are known. OpenAI additionally needs the owned file IDs for cleanup. A ready upload timestamp and note count can support outcome messages; no copied note bodies or per-note synchronization index is needed.

Persist a small conversation-to-folder association using existing session storage conventions. API key values stay in existing credential storage. Read the saved key's current value for remote operations. Provider model configuration changes must not erase the credential reference needed to remove an existing store.

Keep existing API-key deletion and replacement behavior. Show folder-store usage in the key's existing usage display, but do not block key deletion. A deleted or revoked key can prevent remote cleanup; the folder-delete fallback above handles that case without a credential-rebinding workflow.

- Validate and register local work before remote creation. Reject concurrent mutations of the same folder/store, including store operations anywhere inside a subtree being deleted; do not build a general job queue.
- Poll only while an explicit upload is active, because provider indexing is asynchronous. Success requires terminal success for every document, not merely receipt of an operation ID or an HTTP upload response.
- On an upload failure or cancellation, attempt cleanup of known resources. If cleanup completes, restore **Upload to store**. Otherwise retain the incomplete record and **Remove from store**. A cancellation uses this same path and cannot later become a successful upload.
- After shutdown, vault switch, or interruption, leave incomplete uploads unavailable. On reopening, offer cleanup through **Remove from store**, then a fresh explicit upload. Automatic resume and automatic resubmission are outside this version.
- If a create/upload response is lost, its remote side effect may be unknown. Do not repeat it automatically or promise exactly-once creation. Report that remote resources may need manual removal through the provider. Known IDs must remain available for cleanup.
- If the saved key is deleted or loses access, retain the store association and report the credential problem. Do not silently substitute another key. If the saved key still exists, the user can replace its value with a credential that has access to the original project. Otherwise folder deletion can proceed through **Delete anyway**, with the remote resources recorded for manual cleanup. No database repair or credential rebinding is required to delete the local folder.
- Use existing event history for upload success, removal success, and failures that require later action. Keep progress in the operation dialog. Do not log note bodies, key values, or secret upload URLs.

Use existing vault lifetime guards so late work cannot update another vault or a deleted/replaced association. No startup scan of all provider stores, automatic expiry, periodic health check, automatic cleanup daemon, or new scheduler is required.

## Acceptance criteria

1. A real folder offers **Copy Ref ID** and the appropriate upload/removal action. Its Ref ID survives rename, move, removal, and re-upload. The synthetic root is excluded.
2. OpenAI and Google can each upload a folder using an existing saved key. Offer only keys used by compatible direct model configurations: OpenAI Responses or Google (Gemini). Exclude keys used only by Chat Completions, including Google's compatibility endpoint. Multiple configurations sharing a key produce one choice.
3. Upload includes the complete saved ordinary-note subtree, one full-text Markdown document per nonempty note. Generated annotations, binaries, compressed summaries, and unsaved edits are excluded. Duplicate titles remain distinguishable.
4. Empty input fails before remote creation. Any nonempty document failure prevents readiness. Indexing must finish before attachment is allowed. Double submission cannot create concurrent stores for the folder. Cancel during upload and indexing: stop further work, use failure cleanup, retain IDs if cleanup fails, and ignore late readiness results.
5. Note changes after upload do not alter the snapshot. Remove and upload again captures new content and keeps the folder Ref ID.
6. Submitting a folder Ref ID alone attaches it without generation. Repeating it is idempotent. Restart preserves it; Clear removes it. `/detach <Ref ID>` removes only the selected folder attachment, even if unavailable, without losing history or changing remote resources. Existing note refs and inline ordinary text retain their behavior.
7. A mixed-provider conversation sends only matching store IDs to each eligible target. Other targets continue with an omission notice. No full-text fallback or cross-provider direct store access occurs.
8. OpenAI uses Responses File Search alongside supported existing tools. Google adds `fileSearch` to the existing `api = "google"` request for both streaming and non-streaming responses, preserving context, saved-key authentication, cancellation, usage, and the selected model. On supported models, exercise File Search with a CHA web function through the existing continuation loop, including returned parts/signatures and the function-call limit. On a model known not to support mixed tools, omit optional functions with a log warning and accurate model instructions. Assistant maintenance-tool turns receive no stores and keep their tools. Unsupported File Search models fail visibly. Requests without stores and Google Chat Completions configurations retain their existing behavior; no automatic protocol switch occurs.
9. Removing a store deletes all owned remote resources, preserves local notes, and disables future store access. Re-upload makes the replacement available to existing folder attachments. No old remote ID is reused.
10. Deleting a folder attempts cleanup of its own and descendant stores before local deletion. Simulate offline, missing-key, permanent-access-loss, and partial-cleanup failures. Cancel keeps local notes and remaining cleanup IDs; Retry can finish cleanup. Explicit Delete anyway removes the local subtree and associations, saves the remaining known remote IDs in a warning event, and schedules no further work. Empty local folders with stores also require confirmation.
11. Interrupt upload and removal, then reopen the vault. Known resources remain removable; incomplete uploads never appear ready or resubmit automatically. A lost creation response produces an honest uncertainty error.
12. Exercise missing credentials, externally deleted stores, and a vault switch during work. Key deletion and replacement remain available. Errors identify the affected folder/provider without exposing content or credentials. Existing ordinary note attachments and unrelated model requests still work.
13. Adding a folder beyond a verified request limit is rejected without changing existing attachments or history. Change a model or re-upload a folder to another provider so a previously valid set exceeds a limit; the affected request reports the problem and identifies a detach command. Detaching restores a usable set without Clear or remote deletion. An unexpected provider limit follows the same recovery path.

Use focused storage, reference-resolution, and request-serialization tests plus a small live end-to-end check for each provider. Extend the existing Google adapter and provider-client test coverage for File Search and mixed-tool continuation rather than create a second provider test harness. A completed store upload alone does not validate inference integration. Live checks should create a temporary test folder, retrieve a distinctive fact, exercise a supported Google File Search/function-tool combination, and remove the remote resources afterward.

## Deferred

Automatic synchronization, an Update action, multiple providers per folder, local embeddings, cross-provider retrieval, inline reference parsing, custom chunking/search controls, persistent resumable jobs, store discovery/import, account discovery, credential rebinding, citation UI, source previews, and a dedicated store-management screen.
