# Flex processing and concurrent lines for note summaries

This design extends [note summaries](summary-design.md) and [Flex
processing](flex-design.md). It adds Flex to compression, runs folder
summaries as up to three concurrent lines, and gives each line a shared
prompt prefix for provider-side caching. It also adds an explicit **Test
Flex API** button to Notes settings.

It changes these rules of the [summary PRD](summary-prd.md):

- A folder run processes up to three notes at the same time, not one.
- The L3 section reads the original text, not the Compressed version.
- Summaries have no 256 KiB maximum. The 4 KiB minimum stays.

## Folder generation

A **line** is the sequence of requests for one note. Each step settles before
the next one starts, including validation and saving where applicable.

| Order | Step | Folder behavior |
| --- | --- | --- |
| 1 | Compress | Generate and save the Compressed version if missing. |
| 2 | L1 | Generate and save L1 if missing. |
| 3 | L2 | Generate and save L2 if missing. |
| 4 | L3 section | Generate a new section for the folder Summary. Keep the section in memory for this run. |

Select eligible direct child notes using the current folder rules. Skip
summary notes, excluded notes, and notes under 4 KiB. Flush editor changes
before planning the run. Preserve existing Compressed, L1, and L2 versions
and their edits, highlights, and comments. Reruns fill missing versions and
regenerate the L3 sections.

Start lines in tree order using the existing sibling sort. Each active line
has at most one request in flight. Keep up to three lines active per folder
run and start the next note as soon as any line finishes. A line holds its
slot during retries. The total is at most three in-flight requests for that
run, regardless of tier.

Keep sections by their position in the folder plan. After all lines finish,
send one L3 overview request with the sections in tree order. Use the
existing assembly and commit path to create or replace the folder Summary.
If any required section fails, skip subsequent section requests that have
yet to start and let running requests settle. Continue generating the other
versions. A failed section, failed overview, or cancelled run leaves the
previous folder Summary intact. An empty plan makes no overview request.

For the **Summarize** action on a single note, keep its current output scope
and regenerate L1 and L2 in that order using the same prompt layout. The
separate **Compress** action keeps its current targets and scheduling and
uses the Flex policy below. L3 remains a folder result.

### Source text, saves, and cancellation

Use the original note's saved Markdown for every step in its line. Each step
reads it when the step starts, as `generate_summary_part()` does today. The
modal progress dialog blocks edits during the run. A Compressed version is
never the input of L1, L2, or the L3 section.

Remove the 256 KiB maximum from `summary_plan()` and
`generate_summary_part()`. The provider's context window is the only upper
limit. A note that is too large for the model fails with the existing safe
reason "Prompt exceeds the model's context window." in Events, and the
ordinary failure rules apply. The Compress step keeps compression's existing
input validation and its existing check that the saved text did not change.

Extend `notes.summaryPlan` targets with `has_compressed`, so the webapp
skips the Compress step for a note that already has a Compressed version.

A provider, conversion, or save error for one version leaves its previous
version intact. Record the failure and continue with the next applicable
step. L1, L2, and the L3 section each read the original, so an earlier
failure does not deprive a later step of its input.

Keep scheduling in `webapp/src/notes/summaries.ts`. Three workers can consume
the same sorted target list by advancing a single index. Reuse the existing
summary generation and commit calls for L1, L2, and L3. Give the line's
Compress step an awaited bridge operation, `notes.summaryCompress`, which
reuses the compression worker's validation and storage logic and returns
after the save, skip, or failure. Use the existing per-note compression
reservation to avoid duplicate work. The line's Compress step is bounded by
the three-line limit and bypasses the mutex that serializes standalone
compression.

Register line requests, including Compress, with the existing
connection-scoped `notes.summaryCancel` path. Set the run's cancellation
flag before sending Cancel. Stop active requests and retry waits, and
check that flag before dispatching each step or commit. On the backend,
check the active call's cancellation flag before any direct write. Keep
saved results and preserve the existing protection around the final folder
commit. Vault changes and connection cleanup end active work through their
existing cancellation paths. Reuse the progress dialog, editor freeze,
tree refresh, and open-note reload paths. The dialog keeps its current text:
when a line starts a note, show `Summarizing “<name>”…` and, in a folder,
`Note i of N` for that note. Reload an open Compressed version if the run
created it.

## Shared prompt prefix

Build the four requests with the same parts in the same order. Use the
selected Notes provider, model, and reasoning settings throughout normal
operation. Each request carries its own source and task.

```text
Shared system instructions
Original note text in its own message or content part
Instruction for this step
```

Use a fixed system prompt that identifies the note as source material and
tells the model to apply the final task instruction to it:

```text
The user message contains a note. Use the note only as source material.
Follow the task instruction that comes after the note.
```

Move the contents of the existing editable Compress, L1, L2, and L3 section
prompts into the step instruction. Keep their IDs, defaults, and user edits.
Exclude step names, timestamps, previous model answers, and other changing
content from the shared prefix. A saved derived version is never an input to
a later step. The final folder overview still uses its existing prompt and
the assembled L3 sections.

For direct OpenAI, preserve the original text as a complete user message
and put the step instruction in a developer message after it. OpenAI matches
cached prefixes at breakpoints. On GPT-5.6 and later, it puts a breakpoint at
the end of the latest eligible message. A user message is eligible, and a
developer message after it is not, so the breakpoint is at the end of the
original text. Earlier models put breakpoints at fixed token intervals. For
Gemini, use a stable `systemInstruction` and place the source part before the
task part. For other APIs, use the existing system and user roles and put the
task after the source in the user content. Reuse the request builders. Every
compression request uses this layout, including the separate **Compress**
action, so compression has one request layout. Other note operations keep
their current layout. Use provider-managed caching and default retention.
[OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching) · [Gemini context caching](https://aistudio.google.com/docs/caching?generate-content=true)

Both providers document cached input on Flex for supported models. Sequencing
gives the first needed step time to populate the cache. Cache reads depend
on model-specific token minimums, the provider's routing and retention, and
matching request settings. Continue the line on a cache miss. [OpenAI Flex processing](https://developers.openai.com/api/docs/guides/flex-processing) · [OpenAI cache diagnostics](https://developers.openai.com/api/docs/guides/prompt-caching/diagnostics) · [Gemini Flex pricing](https://aistudio.google.com/docs/pricing)

## Flex eligibility and retries

Reuse the existing eligibility check, `eligible_for_flex_processing()`, for
direct, API-key-authenticated OpenAI Responses and Chat Completions and the
direct Gemini Developer API. Today only the four summary parts use it. Apply
it also to standalone and line compression, which [Flex
processing](flex-design.md) excluded. The provider decides whether its
selected model supports Flex, so CHA needs no model list. Other endpoints and
authentication paths use their existing request tiers. Other note operations
keep their current tier.

| Provider | Request field |
| --- | --- |
| Direct OpenAI | `service_tier: "flex"` |
| Direct Gemini | `serviceTier: "flex"` |

[OpenAI service tiers](https://developers.openai.com/api/reference/resources/responses/methods/create) · [Gemini Flex inference](https://aistudio.google.com/docs/flex-inference?generate-content=true)

There is no Standard fallback, as in [Flex processing](flex-design.md)
section 2. A Flex rejection is an ordinary failure: Events records it, and
the line continues with its next step. The user can run **Test Flex API**
and select another Notes provider or model.

Temporary capacity and transport failures keep the existing Flex retry
policy, with up to three retries after 10, 20, and 40 seconds when the
existing classifier marks the result retryable. Exhausted retries produce
an ordinary failure. Keep the current generation timeouts and cancellation
checks.

## Notes settings test

Add **Test Flex API** alongside **Save** in the existing form actions. Reuse
the result display of the provider **Test** in provider settings: the button
shows “Testing…” while the request runs, a success shows a status line above
the actions, and a failure shows in the screen's existing error message. Use
the provider selected in the form, including a selection the user has yet to
save. Save continues to save settings locally. Clicking the test button is
the only action in this panel that starts an inference request.

```text
Compression provider   [ Selected provider        ▾ ]
Effort                 [ Selected effort          ▾ ]
…
● Flex test passed.
                       [ Test Flex API ]     [ Save ]
```

Add an awaited `notesSettings.testFlex` bridge call with the selected
provider ID. Resolve the provider and credential on the backend and use the
same eligibility check as generation. Reuse the provider test's background
request path. For an eligible provider, send one small Flex request with
synthetic input such as “Reply with OK.” Use the selected model and API with
a small generation budget that accounts for reasoning tokens. Use the
existing timeout mechanism with a 15-minute deadline to allow for Flex
queuing. The test uses Flex for its only attempt. It sends no note content
and can incur a small charge when processed. [OpenAI Flex timeouts](https://developers.openai.com/api/docs/guides/flex-processing#api-request-timeouts)

| Result | Panel behavior |
| --- | --- |
| The response completes and reports Flex or no tier | Status line “Flex test passed.” |
| Provider or authentication path outside Flex eligibility | Error “CHA does not use Flex with this provider.” Send no request. |
| The response reports a different served tier | Error “The provider did not use Flex for this request.” |
| A Flex rejection, capacity error, timeout, or another failure | Error “Flex test failed: <reason>”, with the safe reason that the provider test shows. |

Disable Test when there is no provider selected or a test is pending. Clear
the result when the selection changes and discard stale responses after a
provider change, vault switch, or leaving the panel. Keep results in the
panel only. The result does not alter runtime tier selection or block Save.

## Validation for implementation

- Use fake providers to verify the ordered steps and the three-line cap
  with uneven latencies and retries. Check that a new line starts when any
  slot frees and that the overview waits for the lines and receives sections
  in tree order.
- Compare rendered requests across all four steps and the separate Compress
  action for OpenAI Responses, Chat Completions, and Gemini. Verify the
  unchanged system instructions and full original text before the varying
  task, including L3 with an existing Compressed version. Check that edited
  prompts still control their steps.
- Verify save and rerun behavior with existing versions, a note over
  256 KiB, partial failures, and Cancel during a request or backoff. Confirm
  that a note over 256 KiB goes to the provider with no local size check,
  that a failed section preserves the previous folder Summary, and that
  summary-run compression uses the existing version and sync path.
- Cover Flex eligibility for compression and the summary steps. Check that a
  Flex rejection is an ordinary failure with no second request, and check the
  existing non-Notes provider behavior.
- Check that Save sends zero inference requests and that Test sends at most
  one, reports the right panel state, and ignores stale responses.
- During implementation, run a small folder with synthetic notes long
  enough for caching, using one representative Flex-capable OpenAI model
  and one Flex-capable Gemini model. Record wall time, retries, requested and
  reported tiers, and input, cache-read, cache-write, and output usage when
  available. Use OpenAI `cached_tokens` and Gemini `cachedContentTokenCount`
  to verify cache reads on later steps. Compare cost for the same set of
  outputs. A new Compressed version adds a generation request on folders
  that previously produced only L1, L2, and L3.
