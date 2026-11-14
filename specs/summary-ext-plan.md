# Summary lines, shared prompt prefix, and Flex: implementation plan

Date: November 14, 2026

Status: Implementation plan. No implementation was done as part of this
plan.

Source: [Flex processing and concurrent lines for note summaries](summary-ext.md).
This plan calls it "the design". The rules that it extends are in
[summary-prd.md](summary-prd.md), [summary-design.md](summary-design.md), and
[flex-design.md](flex-design.md).

## 1. Scope and completion criteria

Implement the design for the desktop application and for `cha-daemon`. The
work has these parts:

- Note steps (compression, L1, L2, and the L3 section) use a fixed system
  prompt. The note text comes first, and the step's editable prompt comes
  after it.
- The L3 section reads the original text. Summaries have no 256 KiB maximum.
- Compression uses Flex on eligible endpoints. That includes the separate
  **Compress** action and the Compress step of a summary line.
- A folder run makes up to three notes at the same time. Each note is a line
  of steps: Compress, L1, L2, L3 section.
- A new awaited bridge call, `notes.summaryCompress`, runs the Compress step.
- Notes settings gets **Test Flex API**, with a new bridge call
  `notesSettings.testFlex`.

The work is complete when:

- all tasks of plan sections 4 to 9 are done;
- the checks of each phase pass;
- the user approved the UI before Phase 5 started.

### UI approval gate

[AGENTS.md](../AGENTS.md) requires the user's approval of each visible
change before implementation. A request to implement this plan does not
approve the UI. Before Phase 5 starts, show the user these parts of the
design's "Notes settings test" section and wait for an explicit approval:

- the mockup: the **Test Flex API** button beside **Save**, and the status
  line above the buttons;
- the button text while the test runs: "Testing…";
- the four result texts: "Flex test passed.", "CHA does not use Flex with
  this provider.", "The provider did not use Flex for this request.", and
  "Flex test failed: \<reason\>".

The progress dialog keeps its current texts, and the Events texts are the
existing compression and summary texts. Phases 1 to 4 have no visible change
and can start before the approval. If the user changes a text or a control,
update the design first, then implement.

### General rules

- Do not change documents in `docs/`. Do not change other files in `specs/`.
- Do not change other note operations, chat, the session **Save summary**
  operation, or audio, except where a task below says so.
- Add no state, setting, or machinery that the design does not specify. The
  design has no Standard fallback, no source digest, and no prompt cache key.
- Commit only when the user asks.

## 2. Code organization

The work adds no new source file. All changes are in existing files.

| Area | Files |
| --- | --- |
| Request input | [model_context.h](../src/characters/model_context.h) |
| Request builders | [responses_api.cpp](../src/providers/responses_api.cpp), [chat_completions_api.cpp](../src/providers/chat_completions_api.cpp), [google_api.cpp](../src/providers/google_api.cpp), [anthropic_api.cpp](../src/providers/anthropic_api.cpp) |
| Generation result | [model_backend.h](../src/providers/model_backend.h) |
| Notes service | [notes_service.h](../src/notes/notes_service.h), [notes_service_internal.h](../src/notes/notes_service_internal.h), [notes_operations.cpp](../src/notes/notes_operations.cpp) |
| Application | [application.h](../src/app/application.h), [app/notes_operations.cpp](../src/app/notes_operations.cpp) |
| Bridge | [bridge_protocol.h](../src/bridge/bridge_protocol.h), [bridge_protocol.cpp](../src/bridge/bridge_protocol.cpp), [bridge_router.cpp](../src/bridge/bridge_router.cpp) |
| API contract | [dto.yaml](../resources/dto.yaml), generated [schema.d.ts](../webapp/src/api/schema.d.ts) |
| Webapp API | [client.ts](../webapp/src/api/client.ts), [nativeClient.ts](../webapp/src/api/nativeClient.ts), [noteGuards.ts](../webapp/src/api/noteGuards.ts), [fixtures.ts](../webapp/src/test/fixtures.ts) |
| Webapp notes | [summaries.ts](../webapp/src/notes/summaries.ts), [App.tsx](../webapp/src/components/App.tsx) |
| Webapp settings | [NotesSettings.tsx](../webapp/src/components/NotesSettings.tsx) |
| Wire fixtures | [native-method-policies.json](../tests/fixtures/wire/native-method-policies.json), [notes-contract.json](../tests/fixtures/wire/notes-contract.json) |
| Native smoke tests | [test-note-summaries.py](../tests/native/macos/test-note-summaries.py), [note-summaries.js](../tests/native/note-summaries.js), [test-note-compression.py](../tests/native/macos/test-note-compression.py) |

The tests of each phase name their test files.

## 3. Phases and order

| Order | Phase | Plan section | Work | Depends on | Visible change |
| --- | --- | --- | --- | --- | --- |
| 1 | 1 | 4 | Step instruction in requests; served tier | — | No |
| 2 | 2 | 5 | Notes service: layout, Flex, L3 source, size limit, summary compression, Flex test | 1 | No |
| 3 | 3 | 6 | Application and bridge contract | 2 | No |
| 4 | 4 | 7 | Webapp API and line scheduler | 3 | No |
| — | — | 1 | UI approval | — | — |
| 5 | 5 | 8 | Notes settings test button | 3, UI approval | Yes |
| 6 | 6 | 9 | Native smoke tests, regression run, live checks | all | — |

## 4. Phase 1: step instruction in requests

### T1.1 Request field

File: `model_context.h`.

Add one field to `GenerationRequest`:

```cpp
// Note steps: the task text that comes after the prompt text.
// Empty for all other requests.
std::string step_instruction;
```

Do not change `project_model_context()`. Its last message is always the
user message with the prompt text, so each builder appends the instruction
after its message loop.

### T1.2 Builders

When `input.step_instruction` is not empty, each builder adds it after the
note text. Add its size to `text_sizes->conversation_bytes`. Requests with an
empty field do not change.

| Builder | Rule |
| --- | --- |
| `build_responses_request_body()` | Direct OpenAI host (`is_direct_openai_host()`): append `{"role": "developer", "content": <instruction>}` to `input`. Other hosts, including the ChatGPT subscription: append `"\n\n" + instruction` to the content of the last message. |
| `build_chat_completions_request_body()` | The same rule, in `messages`. |
| `build_google_request_body()` | Append `{"text": <instruction>}` to the `parts` of the last content. The note is the first part, and the task is the second part. |
| `build_anthropic_request_body()` | Append `{"type": "text", "text": <instruction>}` to the `content` of the last message. |

`ProviderClient::prepare()` in test mode sends only the prompt text. Do not
change it.

### T1.3 Served tier

Files: `model_backend.h`, `responses_api.cpp`, `chat_completions_api.cpp`.

- Add `std::string service_tier;` to `GenerationResult`: "The tier that a
  non-streaming response reports. Empty when the response does not report
  one."
- `decode_responses_response()` and `decode_chat_completions_response()`: copy
  the top-level `service_tier` string when it exists.
- Only the Flex test reads the field (T2.6), and it sends a non-streaming
  request. Do not change the stream decoders or the Google decoder. Gemini
  does not move a Flex request to Standard, so a Gemini response with no
  tier is a pass.

### Checks for Phase 1

- [unit_responses_api.cpp](../tests/providers/unit_responses_api.cpp) and
  [unit_chat_completions_api.cpp](../tests/providers/unit_chat_completions_api.cpp):
  - direct OpenAI host: the user message holds the note text, and a
    developer message with the instruction comes after it;
  - an OpenRouter host and a compatible host: one user message that ends with
    a blank line and the instruction;
  - with an empty field, the body is the same as today;
  - `service_tier` in a non-streaming response is copied; a response without
    it gives an empty field.
- [unit_google_api.cpp](../tests/providers/unit_google_api.cpp): the user
  content has two parts, note first, instruction second;
  `systemInstruction` holds only the system prompt.
- [unit_anthropic_api.cpp](../tests/providers/unit_anthropic_api.cpp): the
  user message has two text blocks in the same order.
- The existing Flex and Priority tests in
  [unit_provider_client.cpp](../tests/providers/unit_provider_client.cpp)
  pass without change.

## 5. Phase 2: notes service

All tasks of this phase are in `notes_operations.cpp`, unless a task names
another file.

### T2.1 Request kinds

Replace the `bool for_summary` parameter of `prepare_note_provider()` with
an enum:

```cpp
enum class NoteRequestKind {
    plain,        // Save summary and plan validation: as today.
    flex,         // L3 overview: Flex, own prompt as the system prompt.
    source_step,  // Compression, L1, L2, L3 section: Flex, fixed system prompt.
};
```

- `flex` and `source_step` set
  `provider.config.flex_processing = eligible_for_flex_processing(provider.config)`.
  `plain` leaves it false.
- `source_step` sets `system_prompt = note_source_system_prompt`. The other
  kinds keep `system_prompt = prompt.text`.
- Add the constant with the design's text:

  ```cpp
  constexpr std::string_view note_source_system_prompt =
      "The user message contains a note. Use the note only as source material.\n"
      "Follow the task instruction that comes after the note.";
  ```

- Add a parameter `std::string instruction = {}` to `collect_note_prompt()`.
  It sets `request.step_instruction`.

Call sites:

| Call site | Kind | Instruction |
| --- | --- | --- |
| `compress_note()` and the summary Compress step (T2.4) | `source_step` | Text of the `note-compression` prompt |
| `generate_summary_part()` for `l1`, `l2`, `l3_section` | `source_step` | Text of the part's prompt |
| `generate_summary_part()` for `l3_overview` | `flex` | Empty |
| `summary_plan()` provider validation | `plain` | — |
| `generate_session_summary()` | `plain` | — |

Each worker captures the instruction string when it captures the
definition. The prompt IDs, their default texts, and the user's edits do not
change.

### T2.2 Summary source and size limit

- `generate_summary_part()`: read `get_note_text(id).text` for `l1`, `l2`, and
  `l3_section`. The L3 section no longer reads the Compressed version.
- Remove the `text_file_byte_limit` check and its three reason texts from
  `generate_summary_part()`. Keep the 12 MiB check of the overview text.
- `summary_plan()`: remove the 256 KiB refusal for a single note. Keep the
  4 KiB refusal.
- Remove `#include "util/text_file.h"` if nothing else in the file uses it.
- A note that is too large for the model fails with the existing safe reason
  "Prompt exceeds the model's context window." through the existing failure
  path.

### T2.3 Plan targets

`summary_plan()`: add `{"has_compressed", note.has_compressed}` to each
target.

### T2.4 Compression shared by two entry points

Move the body of `compress_note()` into two helpers. The behavior of the
separate **Compress** action does not change, except for T2.1.

- `std::optional<NoteText> Impl::compression_source_locked(std::int64_t id,
  bool skip_ineligible)`: the current item checks, the check for a running
  compression, the text read, and the 4 KiB and empty-text checks. It returns
  `std::nullopt` for a skip, and it throws the current errors otherwise.
- `CompressionResult Impl::run_compression(control, definition, instruction,
  note, epoch, cancel, bool serialized)`: the current worker body.
  `CompressionResult` holds the result kind and, for `failed`, the failure
  text. When `serialized` is true, the function waits for
  `compression_mutex` as today. It keeps the retry loop, the changed-text
  check before each attempt, the answer checks with the safe reasons,
  `commit_compressed_note()`, and `retire_compression()` with the current
  Events texts. It returns one of `saved`, `discarded` (text changed),
  `cancelled`, or `failed` with the failure text. The early return when the
  reservation was already retired gives `cancelled`.

Declare both helpers in `notes_service_internal.h`. `compress_note()` calls
them with `serialized = true`. It keeps its result JSON and its start event.

### T2.5 `compress_summary_note()`

Files: `notes_service.h`, `notes_operations.cpp`.

```cpp
std::shared_ptr<OperationReply> compress_summary_note(
    std::string connection_id, std::int64_t id, std::uint64_t epoch);
```

Under the lifecycle lock:

1. `require_admitted(epoch)`.
2. `compression_source_locked(id, true)`. On a skip, complete the reply with
   `{"compressed": false}` and return it. This covers an existing Compressed
   version and a running compression of the same note.
3. Prepare the definition with `source_step` and capture the
   `note-compression` prompt text.
4. Make the reservation as `compress_note()` does: a `CompressionControl` in
   `compressions`, the start event, and `notify_compression()`.
5. Make a `SummaryCall` with the connection ID and
   `cancel = control->cancel`, and add it to `summary_calls`. Thus
   `notes.summaryCancel`, connection close, and a vault change all stop the
   same request.
6. Launch the worker with the same cancel flag.

The worker calls `run_compression(..., /*serialized=*/false)`. Then, under the
lifecycle lock, it calls `remove_summary_call()` and settles the reply:

| Result | Reply |
| --- | --- |
| `saved` | `{"compressed": true}` |
| `discarded` | `{"compressed": false}` |
| `cancelled` | Fail with `vault_changed` if the epoch changed, else `operation_cancelled` |
| `failed` | Fail with `internal_error` and the failure text |

Every exit path removes the call and settles the reply exactly once. If the
launch fails, remove the call and the reservation and throw
`command_queue_full`, as `compress_note()` does. The Events texts are the
existing compression texts. No new event text is added.

### T2.6 `test_flex()`

Files: `notes_service.h`, `notes_operations.cpp`.

```cpp
std::shared_ptr<OperationReply> test_flex(std::string provider_id, std::uint64_t epoch);
```

- Move the legacy key binding of `prepare_note_provider()` into a helper,
  `bind_note_api_key(config, api_keys)`, and use it in both places. Without
  it, a provider that names its key by its old name fails the eligibility
  check in the test but not in generation.
- Under the lifecycle lock: `require_admitted(epoch)`; find the provider in
  the workspace snapshot (a missing ID is `not_found`, as `provider.test`
  reports it); copy its config; call `bind_note_api_key()`.
- If `eligible_for_flex_processing(config)` is false, throw
  `invalid_argument` with "CHA does not use Flex with this provider." Send no
  request.
- Set `flex_processing = true`, `stream = false`, `timeout_s = 900`,
  `web_search = off`, and `max_tokens = 1024`. Keep the provider's own model,
  API, and reasoning effort, as `provider.test` does.
- Launch a background job that sends "Reply with OK." with an empty history,
  as `settings::test_provider()` does. Map the result with a small helper,
  `flex_test_failure(const GenerationResult&)`, which returns the failure
  text or nothing:

| Result | Reply |
| --- | --- |
| Cancelled | Fail with `operation_cancelled` |
| `outcome` is not `completed` | Fail with `invalid_argument` and "Flex test failed: " + the message, or "Flex test failed: The provider returned an error." when the message is empty |
| `service_tier` is not empty and is not `flex` | Fail with `invalid_argument` and "The provider did not use Flex for this request." |
| Other | Complete with `{}` |

A completed response with any finish reason passes, because the provider
served the request. The test sends no note text and writes no event.

### Checks for Phase 2

Update these tests in
[unit_bridge_router.cpp](../tests/bridge/unit_bridge_router.cpp):

- `CompressesOriginalAndReplacesOnlyCompleteAnswersAcrossProviderApis`,
  `CompressionUsesSavedTextAndOmitsEmptyResponsesInstructions`, and the other
  compression body checks: the system prompt is the fixed text, and the
  compression prompt comes after the note text by the T1.2 rule of each API.
- `NoteSummaryGenerateUsesPartPromptsAndSavedSource`: L1, L2, and the L3
  section have the fixed system prompt and their part prompt after the note.
  The L3 section receives the original text also when a Compressed version
  exists. The L3 overview keeps its prompt as the system prompt.
- `NoteSummaryGenerateRecordsInputSizeErrors` and
  `NoteSummaryPlanChecksSizeAndFiltersFolderTargets`: a note over 256 KiB is
  planned and sent; a single note over 256 KiB is not refused; a note under
  4 KiB is still refused; the plan targets have `has_compressed`.

Add these tests:

- An edited compression, L1, L2, or L3 section prompt is sent as the
  instruction of its step.
- A provider error on a step sends one request and gives an ordinary failure
  with its event. No second request uses another tier.
- Summary compression saves a Compressed version and returns
  `{"compressed": true}`, with the existing compression events. The version
  is the same level-0 row that the **Compress** action writes, and D1 sync
  marks it in the same way.
- It returns `{"compressed": false}` without a request for a note with a
  Compressed version, a note with a running compression, a note under 4 KiB,
  and an empty note.
- It returns `{"compressed": false}` with the existing warning when the text
  changes before the commit.
- It does not wait while a separate compression holds `compression_mutex`.
- `notes.summaryCancel` stops it during a request and during a retry wait,
  retires the reservation with the existing cancel event, and fails the reply
  once. Connection close and a vault change also settle the reply exactly
  once.
- A launch failure removes the call and the reservation.
- The Flex test fails with "CHA does not use Flex with this provider." and
  sends no request for a mock host, a ChatGPT subscription provider, and a
  test-mode provider.
- `flex_test_failure()` returns the right text for a cancelled result, a
  failed result with and without a message, `service_tier` `default`, `flex`,
  and empty.

Existing tests for retries, cancellation, summary commits, and session
summaries pass without other changes.

## 6. Phase 3: application and bridge contract

### T3.1 Application wrappers

Files: `application.h`, `app/notes_operations.cpp`.

Add `compress_summary_note()` and `test_notes_flex()` wrappers, as the
wrappers of `generate_summary_part()` and `compress_note()` do.

### T3.2 Method names

Files: `bridge_protocol.h`, `bridge_protocol.cpp`.

Add `Method::notes_summary_compress` as `notes.summaryCompress` and
`Method::notes_settings_test_flex` as `notesSettings.testFlex`. Both require
the context epoch. Neither is a control method.

### T3.3 Dispatch

File: `bridge_router.cpp`.

- `notes.summaryCompress`: `require_only_keys(params, {"id"})` and
  `require_safe_id()`. Call `start_background()` as `notes.summaryGenerate`
  does, with the connection ID.
- `notesSettings.testFlex`: `require_only_keys(params, {"provider_id"})` and
  `require_identifier()`. Call `start_background()` as `provider.test` does.
- Add both methods to `deadline_exempt()`. A compression and a Flex test can
  wait for minutes.

### T3.4 DTOs and generated types

File: `dto.yaml`.

- `SummaryTarget`: add the required boolean `has_compressed`.
- Add `SummaryCompressRequest` (`id`), `SummaryCompressed` (`compressed`,
  boolean), and `NotesFlexTestRequest` (`provider_id`), with
  `additionalProperties: false`. The Flex test result is an empty object.
- Run `npm --prefix webapp run api-types`. Do not edit `schema.d.ts` by hand.

### T3.5 Wire fixtures

- `native-method-policies.json`: add the two methods.
- `notes-contract.json`: add `has_compressed` to the `SummaryPlan` example
  and add a `SummaryCompressed` example.
- Update [unit_wire_fixtures.cpp](../tests/runtime/unit_wire_fixtures.cpp) and
  [wireFixtures.test.ts](../webapp/src/api/wireFixtures.test.ts) for the new
  examples.

### Checks for Phase 3

- `NoteSummaryMethodsRequireExactKeysPartsAndEpoch` in
  `unit_bridge_router.cpp`: the two new methods refuse extra keys, missing
  keys, invalid IDs, and a missing or old epoch.
- The method policy test passes with the new entries.
- `npm --prefix webapp run check` passes.

## 7. Phase 4: webapp API and line scheduler

### T4.1 Client methods and guards

- `client.ts`: add `summaryCompress(id: number): Promise<boolean>` to
  `NotesClient`, and `testNotesFlex(providerId: string): Promise<void>` to
  `ChaClient` beside `saveNotesSettings()`.
- `nativeClient.ts`: `summaryCompress` calls `notes.summaryCompress` and
  returns `compressed`; `testNotesFlex` calls `notesSettings.testFlex` with
  `isEmptyResult`.
- `noteGuards.ts`: `isSummaryTarget()` requires `has_compressed`, a boolean.
  Add `isSummaryCompressed()`.
- `fixtures.ts`: add both methods as unavailable stubs, as the other summary
  methods are.

### T4.2 Lines in `summarizeAndWait()`

File: `summaries.ts`.

Replace the per-note loop. Keep the plan call, the tree-order sort, the
conversion helpers, the overview step, the commit, and the cancel function.

```text
targets = plan.notes in tree order
sections = array with one empty slot for each target
next = 0
line():
    while next < targets.length and Cancel was not requested:
        index = next; next += 1
        target = targets[index]
        check the vault context
        show 'Summarizing “<name>”…', and 'Note <index + 1> of N' for a folder
        if folder and not target.has_compressed: step(compress(target))
        if not folder or not target.has_l1:      step(makeLevel(target, 'l1'))
        if not folder or not target.has_l2:      step(makeLevel(target, 'l2'))
        if makeL3:                               step(makeSection(target, index))
run min(3, targets.length) lines for a folder, one line for a note
wait until all lines settle
if makeL3 and no Cancel: check the vault context; overview and commit as today
```

Rules:

- `step()` returns at once when Cancel was requested. Otherwise it awaits the
  step and ignores its failure: the backend already recorded the event.
- `compress(target)` calls `client.summaryCompress(target.id)` and adds the
  ID to `result.compressedIds` when it returns true.
- `makeSection(target, index)` returns at once when `makeL3` is false. It
  stores the section in `sections[index]`. On failure, it sets `makeL3` to
  false, so lines start no more sections. Running requests settle.
- A vault-context error stops its line. After all lines settle, the
  existing `checkContext()` before the overview ends the operation, as
  today.
- The overview text joins `sections` in index order, which is tree order.
- A single note makes L1 and then L2 in one line. It has no Compress step
  and no L3.
- The return value gets `compressedIds: number[]`.

### T4.3 Reload of an open Compressed version

File: `App.tsx`.

In the summary branch of `runNoteOperation()`, also set `reload` when
`state.noteTextMode === 'compressed'` and `result.compressedIds` includes the
open note.

### Checks for Phase 4

- [summaries.test.ts](../webapp/src/notes/summaries.test.ts), with a fake
  client and controlled promises:
  - each line runs Compress, L1, L2, and the L3 section in this order, and
    each step starts after the previous one settles;
  - with uneven latencies, at most three requests are in flight, and the next
    note starts when any line becomes free;
  - existing versions skip their steps, and `has_compressed` skips Compress;
  - a failed step does not stop its line;
  - after a section fails, no new section starts, L1 and L2 continue, and no
    overview is sent;
  - the overview waits for all lines and receives the sections in tree
    order;
  - a folder with no targets sends no overview;
  - Cancel during a step starts no new step and commits no late result;
  - a single note runs L1 and then L2, with no Compress and no section;
  - a vault change ends the operation;
  - `compressedIds` lists only notes with a new Compressed version.
- [App.summary.test.tsx](../webapp/src/components/App.summary.test.tsx): an
  open Compressed version reloads when the run made it, and does not reload
  otherwise.
- [nativeClient.test.ts](../webapp/src/api/nativeClient.test.ts): the two new
  calls send the right method and keys and refuse invalid results.

## 8. Phase 5: Notes settings test button

Start this phase only after the UI approval of plan section 1.

### T5.1 Button and result

File: `NotesSettings.tsx`. Use the result display of the provider **Test**
in [ProviderSettings.tsx](../webapp/src/components/ProviderSettings.tsx).

- Add the states `testing` and `testPassed`, and a request counter in a ref.
- In `cha-settings-form-actions`, add a `cha-button` with the text
  **Test Flex API** before **Save**. While a test runs, it shows "Testing…".
  It is disabled when no provider is selected or a test is running.
- Click: clear `error` and `testPassed`, increase the counter, set
  `testing`, and call `client.testNotesFlex(settings.compression_provider)`.
  The call uses the provider in the form, also when it is not saved.
- When the call ends, use its result only if the counter did not change.
  Success sets `testPassed`. A failure sets `error` with
  `publicErrorMessage(failure, 'Flex test failed.')`. The screen's existing
  error message shows it.
- Show `testPassed` as the provider test does, above the buttons:

  ```tsx
  <p className="cha-settings-saved" role="status"><span aria-hidden="true"
    className="cha-settings-status-marker" /> Flex test passed.</p>
  ```

- A change of the provider select clears `testPassed` and `error` and
  increases the counter. Unmount and a change of `reload.epoch` also increase
  the counter, so a late response after leaving the panel or a vault switch
  has no effect.
- **Save** does not depend on the test, and the test does not change Save.

### Checks for Phase 5

[Settings.test.tsx](../webapp/src/components/Settings.test.tsx):

- Test sends one `testNotesFlex` call with the selected provider ID, also
  when the selection is not saved. Save sends no test call.
- The button shows "Testing…" and is disabled while the call runs, and it is
  disabled when no provider is selected.
- Success shows "Flex test passed."; a failure shows its message in the error
  message.
- A response that arrives after a provider change or after unmount changes
  nothing.

## 9. Phase 6: native smoke tests, regression run, and live checks

### T6.1 Native smoke tests

- `test-note-summaries.py`: the system message is now the fixed prompt. Find
  the step from the instruction at the end of the user message (the test
  provider is not a direct OpenAI host). Count the Compress requests of the
  folder notes. Update the expected request counts and the condition of the
  held section request.
- `note-summaries.js`: the lines start notes at the same time, so "Note 1 of
  2" can show for a very short time. Wait for the text of the last started
  note, or for the held request, instead of each count in turn.
- `test-note-compression.py`: the compression request has the fixed system
  prompt and the compression prompt after the note.

### T6.2 Commands

Run, in this order:

```bash
npm --prefix webapp run api-types
```

```bash
npm --prefix webapp run check
```

```bash
make test
```

```bash
make test-ui
```

On Linux, `make test` runs the daemon test set. Run `make itest-daemon` for
the ChaWeb path.

### T6.3 Live checks

These checks use the user's configured providers and cost a small amount of
money. Ask the user before you start them. Do not log credentials.

- Run **Test Flex API** with one Flex-capable OpenAI model, one Flex-capable
  Gemini model, and one ineligible provider. Record the panel results.
- Summarize a small folder of synthetic notes that are long enough for
  caching (more than 4,096 tokens, for Gemini), once with each model. Record
  the wall time, retries, requested and reported tiers, and input,
  cache-read, cache-write, and output usage from the debug log. Use OpenAI
  `cached_tokens` and Gemini `cachedContentTokenCount` to confirm cache reads
  on the later steps of a line.
- Compare the cost with a run of the same outputs before this change. A new
  Compressed version adds one request for each note.

### T6.4 Manual acceptance

Use a copy of a vault with an imported book and a configured Notes
provider:

- Summarize a folder where some notes have no Compressed version. Each note
  gets the missing Compressed version, L1, and L2, and the folder gets a new
  Summary note.
- Include a note over 256 KiB. CHA sends it. If the model refuses its size,
  Events shows the context-window failure and the previous Summary note stays.
- Cancel during a run. Saved versions stay, and the Summary note does not
  change.
- Open a note in the **Compressed** mode and summarize its folder. The new
  Compressed version opens after the run.
- Check Settings → Events for the compression and summary texts.

### T6.5 Delivery check

- `git status` shows only the files of plan section 2 and test files.
- No file in `docs/` changed, and no other file in `specs/` changed.
- `schema.d.ts` equals the generated output.
- Report the test results with their output. Commit only when the user asks.
