# Note transform through Assistant

Date: November 10, 2026

Status: Design for review. No implementation in this change. This document
records the result of the discussion that followed the review of
[skills-prd.md](skills-prd.md).

## 1. Purpose

The user wants to test layered reading of a book before any larger feature is
built. The test is manual: the user decides each step. Today each step needs
copy and paste between Notes and a chat.

One new operation removes the copy and paste:

> Transform: send note A (the source) and note B (the prompt) to a model, and
> store the answer as a new note in a specified folder.

The user gives the instruction to Assistant in the Welcome chat. For example:
"Transform each note in Book X with Select passages into Book X L2."

A layer chain needs no more machinery. Each output is a note, and the next
transform can use it as its source.

## 2. Requirements

| # | Requirement | Source |
| --- | --- | --- |
| R1 | Assistant can start a transform of one source note with one prompt note. The answer becomes a new note in a specified folder. | User |
| R2 | The web client converts the model answer to note content. The new note has the same formatting quality as other imported notes. | User |
| R3 | The tool call waits for the result. Assistant gets the new note's Ref ID and can use it in the next call. | User |
| R4 | The transform uses the model and the effort level that are selected for Assistant in the chat at that time. | User |
| R5 | The source text and the answer do not go through the Assistant conversation. | Discussion result, see section 3 |

## 3. Results of the discussion

The first idea had four capabilities: read a note, use a note as a prompt,
transform, and store the output. The discussion reduced them to one operation.

- "Use a note as a prompt" and "transform" are the same operation. A prompt
  always needs a source.
- "Store the output" must be part of the transform. If Assistant reads the
  chapter and then writes the output with a separate tool, the full text goes
  through the chat model two times. This costs more and is slower. The chat
  model can also change the words, and a passage layer must keep the author's
  exact words.
- "Read a note" is not necessary for the transform. For a discussion of a note,
  a chat already accepts a message that contains only a Ref ID
  ([text_input.cpp:244](../src/runtime/text_input.cpp)).

Assistant is the correct place for the tool, and other characters are not:

- Assistant already reads the note catalog with Ref IDs
  ([assistant_service.cpp:595](../src/app/assistant_service.cpp)). It can find
  a folder or a note by its name.
- Assistant still reads no note content. It moves only Ref IDs. The rule in
  [ass-tools.md](ass-tools.md) stays true.
- The chat is the control. This design adds no screen element.

## 4. The tool

Add one maintenance tool, `note_transform`. It is available only in Welcome,
as the other maintenance tools are.

| Argument | Meaning |
| --- | --- |
| `source_ref_id` | Ref ID of the source note |
| `prompt_ref_id` | Ref ID of the prompt note |
| `folder_ref_id` | Ref ID of the destination folder |

Result on success: `{"ref_id": "...", "name": "...", "committed": true}`.

Result on failure: the existing error form,
`{"error": "...", "message": "...", "committed": false}`. The message names the
item or the step that failed.

Rules:

- The source and the prompt are items that `note_get_text()` can read. The
  transform uses the full text of each note, not the compressed text.
- The destination is an existing folder. The tool does not create folders.
- The new note gets the name of the source note.
- The tool checks the three items and the name before the model call, so that
  a wrong argument does not cost a model request.

## 5. Sequence

```text
Assistant reply (worker thread)          NotesService            Web client
  tool call note_transform  ───────────► check items, read text
                                         model call (waits)
                                         answer is Markdown
                                         event notes.transformReady ─────────►
                                                                 convert Markdown
                                         commit: create note  ◄──────────────
  tool result {ref_id, name} ◄─────────  wake the tool call
```

1. Assistant calls `note_transform`. The call runs on the worker thread of the
   Assistant reply, as each maintenance tool does
   ([provider_client.cpp:717](../src/providers/provider_client.cpp)).
2. `NotesService` holds the lifecycle lock only to check the items and to read
   the two texts. It does not hold the lock during the model call or the wait.
3. `NotesService` makes one model request with the existing
   `collect_note_prompt()` ([notes_operations.cpp:87](../src/notes/notes_operations.cpp)).
   The prompt note text is the system prompt. The source text is the user
   message. Tools and web search are off.
4. `NotesService` keeps the Markdown answer in one pending transform record
   and sends the event `notes.transformReady` with the run ID and the Markdown.
5. The web client converts the Markdown with `notePageHtml()` and the Notes
   parser. This is the path that `summaryDocument()` uses
   ([summaryImport.ts:10](../webapp/src/notes/summaryImport.ts)), without the
   checks for the two summary titles.
6. The client sends `notes.transformCommit` with the run ID and the document.
   `NotesService` creates the note and saves the document in one transaction.
   The Notes tree updates through the existing `notes.changed` event.
7. If the conversion fails, the client sends `notes.transformAbandon` with the
   run ID. The tool call returns an error.
8. The commit or the abandon wakes the tool call. The tool call returns its
   result to Assistant.

The wait also ends when the reply is cancelled or when the vault changes. The
tool call checks the cancel flag and the context epoch at a short interval, as
the compression worker does. A commit that arrives after the end of the wait
gets `not_found`, and the client ignores it.

Only one transform can wait at a time, because one Welcome reply runs its tool
calls one after the other. One pending record is sufficient. No map and no
connection ownership are necessary.

## 6. Model and effort (R4)

The transform request uses the provider configuration of the Assistant reply
that made the tool call. That configuration already contains the selected
model and effort.

The reply's definition is available where the tool is bound
([providers.cpp:217](../src/providers/providers.cpp)). Pass its
`ProviderSelection` to the executor together with the existing
`MaintenanceContext`. Do not resolve the settings a second time. A second
resolution can disagree with the request that is in progress.

The Notes provider setting is not used. Compress, summaries, and page import
do not change.

One effect to know: the provider that is selected for Assistant receives the
text of the source note and the prompt note.

## 7. Scope and limits

- **Desktop only.** ChaWeb reads snapshots by polling and has no event path
  to its browser, and its notes are read-only. On ChaWeb the tool returns
  `unavailable`. `NotesService` already knows its host (`native_host`).
- **One note for each call.** A folder is not a source. Assistant makes one
  call for each note.
- **24 tool calls for each reply.** This existing limit
  ([maintenance.h:11](../src/providers/maintenance.h)) does not change. A long
  book needs more than one request for each layer.
- **No retry.** A failed model request returns an error. The user asks again.
- **No Events entries and no progress display.** The Assistant reply reports
  each result.
- **No links to source passages.** The transform gives the layers, not the
  navigation between them.
- **No UI change.** The client handles the event without a visible element.
- **A lost event.** If the web view reloads during the wait, the event is
  lost and the reply continues to wait. The user stops the reply with Stop.
  This design adds no timer for this case.

## 8. Changes by area

| Area | Change |
| --- | --- |
| [tool_calls.cpp](../src/providers/tool_calls.cpp) | Add the `note_transform` schema to `add_maintenance_tools()`. |
| [provider_client.cpp:704](../src/providers/provider_client.cpp) | Add `note_transform` to the write tools, so that its result is never replaced after it runs. |
| [providers.cpp:217](../src/providers/providers.cpp) | Pass the reply's provider selection to the executor. |
| [assistant_service.cpp](../src/app/assistant_service.cpp) | Parse the three Ref IDs and call `NotesService` through a new link, as `pending_notes` does. |
| [notes_operations.cpp](../src/notes/notes_operations.cpp) | The transform: checks, model request, pending record, wait, commit, abandon. |
| [bridge_protocol.cpp](../src/bridge/bridge_protocol.cpp), [bridge_router.cpp](../src/bridge/bridge_router.cpp) | The event `notes.transformReady` and the operations `notes.transformCommit` and `notes.transformAbandon`. The session summary commit and abandon operations are the model. |
| [resources/dto.yaml](../resources/dto.yaml) | The request types of the two operations. |
| [nativeClient.ts](../webapp/src/api/nativeClient.ts) and a small conversion function next to `summaryDocument()` | Receive the event, convert, then commit or abandon. |
| [assistant-maintenance.md](../resources/assistant-maintenance.md) | Tell Assistant what the tool does, that it is desktop only, and that Ref IDs come from `assistant_inspect`. |

## 9. Validation

- Transform one chapter. The new note is in the folder, has the source note's
  name, and shows headings, lists, and emphasis correctly.
- Chain two transforms in one request. The second uses the Ref ID that the
  first returned.
- Change the Assistant model and effort, and transform again. The request log
  shows the new model and effort.
- Give a Ref ID that does not exist, a folder as the source, and a note as the
  destination. Each returns an error before any model request.
- Press Stop during the model request and during the conversion wait. The
  reply stops, and no note is created.
- Switch the vault during the wait. No note is written to the other vault.
- Transform a long chapter that needs several minutes. The call completes.
- On ChaWeb, the tool returns `unavailable`.

## 10. Questions for the user

1. **Name already used.** If the destination folder already has an item with
   the source note's name, does the tool return an error, or does it add a
   number to the name?
2. **ChaWeb.** Is desktop only acceptable?
3. **Retry.** Is "no retry" acceptable for provider rate limits during a run
   of many chapters?
