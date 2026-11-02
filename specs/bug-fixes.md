# File-search bugs: causes and proposed fixes

Status: proposed fixes; no product changes implemented by this document.

Basis: the live run on November 1, 2026, defined in
[file-search-test.md](file-search-test.md). See the
[full results](../file-search-live-results/results.md) for the consumer matrix,
case verdicts, execution limits, and cleanup evidence.

## Scope and priorities

The run identified two issues that warrant app changes. The first is a failure
to distinguish an unavailable Gemini store from a healthy empty result. The
second is a tool-schema compatibility failure with the tested Cerebras
endpoint. These are separate changes and should be validated separately.

| Priority | Issue | User-visible consequence |
| --- | --- | --- |
| 1 | Gemini stale-store reporting | A deleted attached store appears to have no matching information instead of an access failure. |
| 2 | Cerebras tool-schema compatibility | The provider rejects the model request before it can search attached files. |

The core successful routes showed correct attachment scope, saved-credential
selection, source extraction, tool-result delivery, and continuation. These
findings do not justify a retrieval rewrite, a provider-capability framework,
automatic retries, or a new resource-monitoring subsystem.

## Bug 1: deleted Gemini stores appear as empty search results

### Expected behavior

C12 requires an unavailable remote store to produce a per-store error and a
safe warning. Other attached stores must remain usable. The consumer must
receive evidence of the access failure rather than an apparent absence of
relevant information.

C13 also requires a healthy Gemini search that completes with `STOP` and no
source passages to return an empty matches list without a search-failure
error. Both requirements must hold.

### Observed behavior and evidence

The test deleted an owned Gemini store externally, retained its local
attachment, and requested information from it alongside healthy stores.

1. CHA sent the actual retrieval request with the expected saved credential.
2. Google returned HTTP 200, `finishReason: STOP`, and no grounding metadata.
3. CHA returned the deleted folder with `matches: []` and no store error.
4. The consumer said that no matches were found and that no errors were
   reported. It suggested irrelevant or missing files as possible causes.
5. A separate store inspection confirmed that the remote store was missing
   with HTTP 404.

The misleading retrieval response occurred twice for the deleted store. A
healthy Gemini peer also returned HTTP 200 / `STOP` without grounding metadata
for a query that supplied no passages. Consequently, the retrieval response
alone cannot distinguish these situations.

Evidence:

- [C12 Gemini conversation](../file-search-live-results/remaining/C12-1.md).
- [Observed empty response shapes](../file-search-live-results/empty-result-observation.json).
- [Raw request/response and tool-output events](../file-search-live-results/remaining/events.jsonl).
- [Final independent absence checks](../file-search-live-results/remote-absence.json).
- [OpenAI control](../file-search-live-results/remaining/C12-0.md), which
  correctly returned a store error for its deleted store.

### Cause

In `FolderStoreClient::search_stores`, the Gemini branch accepts `STOP` or
`MAX_TOKENS`, extracts only `retrievedContext.text`, and raises an error for an
empty result only when the finish reason is not `STOP`.

For a missing store, Google's generation endpoint supplied the same successful
response shape that CHA permits for a healthy empty result. CHA did not inspect
the store, so it lacked the information needed to satisfy C12. Its per-store
error and warning code was never reached.

This is an app reporting gap triggered by provider behavior. It is not an
extraction defect: there were no source passages to extract, and CHA correctly
did not substitute the generated Gemini answer for source text.

Relevant code:

- [Search response handling](../src/providers/folder_store_client.cpp),
  `FolderStoreClient::search_stores`.
- The existing `FolderStoreClient::google_inspect_store` in the same file.
- [Client declarations](../src/providers/folder_store_client.h).

### Proposed fix

When a Gemini retrieval completes with `STOP` but yields no usable source
passages, inspect that same store before returning a normal empty result.
Reuse `google_inspect_store`, the same client's transport, the target's saved
credential, and the current cancellation object.

| Inspection outcome | Search outcome |
| --- | --- |
| `ok` | Keep `matches: []` without an error. Do not claim that the fact is exhaustively absent. |
| `cancelled` | Return `{"error":"Search cancelled."}` and stop dispatching later stores. |
| Any other result | Throw `FolderStoreError` with the message that `google_inspect_store` returns. The existing catch block adds the per-store error and `log_warn` adds the safe warning. |

Use the inspection result's existing safe message. Do not add a message-mapping
layer or replacement verification-error text. Preserve the current cancellation
check as well as handling the inspection's `cancelled` kind.

Successful grounded results need no additional inspection. Empty
`MAX_TOKENS` responses remain incomplete-retrieval errors; they do not need a
second request to decide whether they are normal empty results.

The extra GET is conditional on an otherwise ambiguous empty result. Google
provides the [store inspection endpoint](https://ai.google.dev/api/file-search/file-search-stores),
and CHA already implements it. No cache, timer, background check, new setting,
new stored status, or generic retry mechanism is required.

Keep inspection failures local to their store so healthy peers remain usable.
Keep the existing tool-result structure, source-only extraction, output
budgets, and safe logging. Do not delete or detach the user's local reference
as a side effect of a search failure.

### Limitations and tradeoffs

An empty Gemini result now costs one additional metadata request. A temporary
inspection failure produces the inspection's existing error instead of an
unqualified empty result. An existing store with malformed document counts
also returns the existing error, "The store document counts are not valid."
This is an accepted side effect of reusing `google_inspect_store`; no separate
inspection implementation is needed.

A successful inspection establishes that the store is accessible at inspection
time. It does not prove that File Search ran, that indexing is complete, or
that the requested fact is absent. A deletion can also race with inspection;
the fix improves reporting without promising an atomic existence-and-search
operation that the provider does not expose here.

Do not classify every missing `groundingMetadata` field as an error. The live
healthy-store response demonstrates why that would break C13.

### Validation and acceptance

Extend the existing folder-store client tests with request-aware transport
responses. The fake must distinguish the retrieval POST from the inspection
GET rather than returning the same response to both.

Required regression checks:

1. `STOP`, no passages, then inspection 404: empty matches plus a per-store
   error and safe warning; a healthy peer still returns its passages.
2. `STOP`, no passages, then successful inspection: empty matches without an
   error. Cover absent metadata and an explicitly empty chunks array.
3. One inspection failure: its safe message becomes the per-store error and
   warning, with no credential values in output or logs. No new matrix of
   classifier-message tests is needed.
4. Cancellation during inspection: the search is cancelled and later stores
   are not dispatched.
5. Grounded `STOP` and grounded `MAX_TOKENS`: source passages are preserved
   without an inspection request. Empty `MAX_TOKENS` remains an error.

Update `GeminiNormalEmptyResultsAreNotErrorsAndGenerationLimitDoesNotTruncatePassages`
in [unit_folder_store_client.cpp](../tests/providers/unit_folder_store_client.cpp).
Its current fake returns the same HTTP 200 JSON object for every request;
inspection accepts that object when document-count fields are absent. The
test can therefore keep passing even if the inspection uses a wrong URL or
key. Make the fake request-aware and explicitly assert:

- An empty `STOP` result sends a GET to
  `/v1beta/fileSearchStores/one` on the Google API origin, with that store's
  `x-goog-api-key` value.
- Grounded `STOP` and grounded `MAX_TOKENS` send no GET.
- Empty `MAX_TOKENS` sends no GET and remains an error.

Retain the C13 assertion that a healthy inspected empty `STOP` result has no
error. Then perform one focused live C12/C13 verification with owned resources,
retaining the retrieval response,
inspection response, normalized output, warning, healthy-peer result, and
consumer continuation. Remove the temporary observer and owned resources
afterward. Unit checks alone do not establish the live provider behavior.

## Bug 2: Cerebras rejects the tool schema before retrieval

### Expected behavior

A configured tool-capable consumer should accept CHA's function declarations,
request `search_attached_files`, receive actual source passages, and finish a
source-based answer. CHA must continue to validate tool arguments locally.

### Observed behavior and evidence

The configured Cerebras connection used `qwen-3.8-27b` through Chat
Completions. CHA advertised summary tools and `search_attached_files`, with
the expected OpenAI and Gemini targets.

The provider rejected the model request with HTTP 400:

```text
Invalid fields for schema with types ['string']: {'maxLength'}
```

The error carried `code: wrong_api_format` and `param: response_format`.
Despite that parameter label, the captured request contains the unsupported
keyword in function-parameter schemas. The request failed before any model
search call or store dispatch.

Evidence:

- [Cerebras conversation and rejection](../file-search-live-results/expanded/C01-extra-provider_8.md).
- [Captured prepared request](../file-search-live-results/expanded/events.jsonl),
  case `C01-extra-provider_8`.
- [Provider mapping](../file-search-live-results/expanded/consumer-mapping.json).

### Cause

The shared tool declarations in
[tool_calls.cpp](../src/providers/tool_calls.cpp) emit:

- `maxLength: 2048` for `search_attached_files.query`.
- `maxLength: 512` for `summary_search.query`.

The Chat Completions builder sends these declarations without adapting the
keyword. The tested endpoint rejects it during schema validation. Removing it
from only the folder-search declaration would leave the summary declaration
with the same incompatibility in an ordinary Forum chat.

This is a provider schema-subset limitation that CHA can accommodate. It is
an app integration issue, not a model's refusal to call a tool and not a store
credential or retrieval failure.

There is a second reason not to rely on this keyword: JSON Schema `maxLength`
counts characters, whereas CHA's documented query limits count UTF-8 bytes.
Local byte validation is the authoritative limit for non-ASCII input.

### Proposed fix

Remove these two `maxLength` properties from the shared emitted declarations.
Keep their object structure, field types, required fields, tool names, and
other existing schema properties. State the 2,048-byte folder-query limit in
its field description, as the summary-query description already does for its
512-byte limit.

Use the same small schema definition across consumers. This avoids a
Cerebras-specific host branch, a schema-rewriting pass, or a capability
registry for a constraint that CHA already enforces itself.

Preserve the existing local checks:

- [Provider tool execution](../src/providers/provider_client.cpp) rejects
  invalid, empty, or over-2,048-byte folder-search queries and extra fields.
- [Store client](../src/providers/folder_store_client.cpp) independently
  enforces its non-empty, 2,048-byte query limit.
- [Summary storage tool execution](../src/storage/note_storage.cpp) enforces
  the 512-byte summary-query limit and validates IDs and argument structure.
  Empty summary queries remain valid because they list summaries.

Do not disable summary tools or file search, silently truncate arguments,
relax local limits, turn off all strict schemas, or retry every provider 400
with progressively weaker declarations.

### Limitations and tradeoffs

Providers that supported `maxLength` lose that generation-time constraint.
They still receive the limit in the description, and CHA rejects invalid
arguments before execution. A model can therefore generate an overlong query,
but it cannot make CHA execute one.

The observed rejection establishes that `maxLength` must be addressed. It does
not establish that all remaining schema keywords are supported or that the
model will use tools correctly afterward. In particular, do not remove
numeric constraints or change other schema features without evidence of a
separate incompatibility.

### Cerebras documentation check

Checked November 2, 2026 UTC, before the proposed follow-up live run:

- The [tool-calling guide](https://inference-docs.cerebras.ai/capabilities/tool-use#schema-requirements)
  explicitly prohibits `pattern`, `minLength`, and `maxLength` in strict tool
  schemas for `qwen-3.8-27b`. This supports the observed `maxLength` fix.
- The [strict-schema reference](https://inference-docs.cerebras.ai/capabilities/structured-outputs#supported-features)
  applies to strict function tools as well as structured responses and lists
  `minimum` and `maximum` as supported. Its unsupported-features section says
  to treat unlisted keywords as unsupported unless model documentation says
  otherwise. It does not explicitly establish acceptance of the exact
  `type: ["integer", "null"]` form used by CHA's summary tool.
- The [October 2, 2025 changelog entry](https://inference-docs.cerebras.ai/support/change-log)
  enables numeric bounds under an update explicitly headed for
  `gpt-oss-120b`; that entry alone does not establish Qwen support.

Keep CHA's numeric constraints, including `maximum: 9007199254740991`, and its
nullable type array. Documentation does not prove acceptance of the entire
request. Recheck these sources before the live run and record any changed
contract. If the actual request reveals another schema incompatibility,
record it separately and assess it before changing those features.

### Validation and acceptance

Check the completed request built through the normal Chat Completions path,
including both summary and folder tools. Assert that the two query schemas
omit `maxLength` while retaining their types and required arguments.

Update the schema-presence assertion in
[test-summary-live.py](../tests/native/macos/test-summary-live.py), currently
labelled "both unmodified production summary tool schemas sent with web
disabled". Remove only the `'maxLength' in dump(tools)` term. Keep its tool-name,
`maximum`, `minimum`, `additionalProperties`, and `["integer", "null"]`
checks. The removed term asserts the obsolete schema constraint; the other
checks continue to verify the production declarations.

Add one provider execution check in
[unit_provider_client.cpp](../tests/providers/unit_provider_client.cpp) that
passes an over-2,048-byte folder query through `ProviderClient::perform` and
confirms rejection before the folder-search callback executes. Existing
folder-search tests do not exercise this query-length boundary.

Retain the existing 513-byte ASCII summary-query rejection in
`SummaryToolsReportInvalidContentArgumentsAndCancellationSafely` in
[unit_note_storage.cpp](../tests/storage/unit_note_storage.cpp). One overlong
case for each tool is sufficient; a separate multibyte matrix is not needed
for the existing `std::string::size()` byte checks. Retain valid-query and
empty-summary-query behavior. Use the existing provider/storage tests.

Then run one focused live mixed-store conversation through the same configured
Cerebras connection. Acceptance requires provider request acceptance, an
actual model tool call, correct saved-key dispatch, real source delivery,
accepted continuation, and a correct answer. Keep the first outcome. If the
provider reveals another rejection or the model fails to use the tool, record
that separately rather than claiming the route passes because HTTP 400 went
away. Cleanup and credential-sanitization requirements remain those of the
original live-test specification.

## Other failures and gaps that do not justify these app fixes

| Observation | Classification and action |
| --- | --- |
| `venice-uncensored-1-2` promised a search but made no call; `venice-uncensored-role-play` claimed zero matches without searching | Model usage/reporting failures. The tool was advertised. These outcomes do not establish a broken search implementation. Keep them as failed model outcomes. |
| Mistral called a retained source-0/document-4 code “Source 4” when Longcase4 evidence was omitted | Model attribution failure. The tool result supplied the correct folder and filename and set `truncated: true`. No extraction or relabelling defect was evidenced. |
| Fireworks model 404s and OpenRouter's lack of a tool-compatible endpoint for the configured GLM route | Provider availability or routing configuration. Correct the configuration if that connection is needed; do not invent replacement model IDs in code. |
| Original API-key-to-Codex routes returned 403 | Authentication configuration. Astra passed using the existing host OAuth owner. That does not prove the separate Sol route; it needs its own correctly configured verification. |
| Buffered Kimi and C03 were stopped at runner bounds | Incomplete validation. C03 delivered the terminal code after three dependent searches, but the model kept generating extensive text. No app deadlock or completed-answer pass was established. |
| C16 calibration exposed the terminal code before the tool allowance was exhausted | Fixture/coverage gap. Prepare a chain that remains dependent through the allowance before claiming an exhaustion result. No budget defect was established. |
| Native Anthropic, exact Kimi 3.1, and disjoint-project credentials were unavailable | Missing prerequisites. Mark those checks blocked; do not count other models or duplicate saved-key IDs as substitutes. |
| Excess-match trimming, OpenAI multipart joining, and later-target skipping after an overflowing entry were not observed | Uncovered branches. They are not demonstrated failures. |

Implementation should address the two evidenced app issues above, with no UI
redesign, automatic model retries, persistent observer, account creation, or
changes to the user's original provider and credential entries.
