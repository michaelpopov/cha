# Bug-fix implementation and validation

Both fixes in `specs/bug-fixes.md` are implemented. Validation completed November 1, 2026 PST (November 2 UTC).

- Empty Gemini `STOP` searches now inspect the same store with its saved key, transport, and cancellation object. Accessible stores retain empty matches; unavailable stores return the existing safe inspection error and warning. Healthy peers remain usable. Grounded searches and empty `MAX_TOKENS` searches do not inspect.
- Summary and folder query declarations omit `maxLength`. Descriptions retain byte limits, and existing local argument checks remain authoritative. Numeric limits, nullable types, strict flags, and other schema properties are preserved.

## Automated validation

- Full unit suite: **861 passed**, two opt-in OAuth/live tests skipped.
- Application suite: **613 passed**.
- Final affected provider/store/summary regressions after removing the observer: **93 passed**.
- Final complete Ninja build, Python syntax check, and `git diff --check`: passed.

The regressions cover exact inspection URL/method/saved key, absent metadata and empty chunks, safe errors and logs, healthy peers, cancellation, grounded STOP/MAX_TOKENS, empty MAX_TOKENS, complete Chat Completions declarations, and local rejection of a 2,049-byte query before its callback. Existing valid and empty summary-query tests and the 513-byte rejection remain intact.

## First live outcomes

| Case | Result | Evidence |
| --- | --- | --- |
| Cerebras `qwen-3.8-27b`, configured Chat Completions connection | Passed. Accepted production summary and folder schemas, made three real searches with the correct saved keys, received both services' real source codes and filenames, accepted continuations, and answered 3 cases / 102 credits / 98 credits remaining correctly. | [Conversation](Cerebras-mixed.md), [events](events.jsonl), [oracle](oracle.json) |
| C13 healthy empty Gemini store, `gpt-4o` consumer | Passed. Retrieval 200/STOP with no passages, inspection GET 200, normalized empty matches without an error, and a completed answer reporting no matches. The fixture was uploaded normally, then its owned document was removed remotely. | [Conversation](gemini/C13-healthy-empty.md), [preparation](gemini/empty-store-preparation.json), [events](gemini/events.jsonl) |
| C12 externally deleted Gemini store with healthy OpenAI/Gemini peers, `gpt-4o` consumer | Passed. Retrieval 200/STOP without grounding, inspection GET 404, safe per-store error and warning, both healthy sources delivered on the first search, and a completed answer using both source codes and reporting the missing store. The consumer's second query also exercised an accessible populated Gemini store returning an inspected empty result. | [Conversation](gemini/C12-deleted-gemini.md), [events](gemini/events.jsonl) |

All **48 evidence checks passed**, including complete resolved target sets, outgoing credential comparisons, source-only extraction, and exact normalized results delivered with actual tool-call IDs. See [verification](verification.json) and [request counts and usage](request-statistics.json). Raw model continuation bodies and warnings remain in each batch's `model.log`.

The first fixture attempt completed the Cerebras case, then stopped because the existing local store-registration rule rejects a zero-note store. That setup failure is retained in [run-error.json](run-error.json). Its resources were cleaned up. The corrected Gemini fixture uploaded a document and removed it remotely; no model outcome was discarded or rerun to obtain a pass.

Before the live run, Cerebras's [tool schema requirements](https://inference-docs.cerebras.ai/capabilities/tool-use#schema-requirements), [strict-schema reference](https://inference-docs.cerebras.ai/capabilities/structured-outputs#supported-features), and [changelog](https://inference-docs.cerebras.ai/support/change-log) were rechecked. No changed contract was observed. The exact production request, including retained numeric and nullable constraints, was accepted live. See [contract check](cerebras-contract-check.json).

## Cleanup and limits

All seven owned stores and both owned OpenAI Files were removed. Ten independent GET checks confirmed 404 for those resources and the explicitly removed C13 document. See [remote absence](remote-absence.json) and each batch's `cleanup.json`.

The temporary vault and copied keys were removed. The provider-binding observer and temporary CMake target were removed, their original files matched pre-observer bytes, and the production targets were rebuilt. Existing provider settings matched across both fixture runs. Credential scanning found no saved values in retained evidence. See [source restoration](source-restoration.json), [credential audit](gemini/credential-audit.json), and [local cleanup](local-cleanup.json).

This is a focused C12/C13 and Cerebras acceptance run, not a repeat of the original full consumer matrix. An inspection confirms accessibility at inspection time; it does not establish exhaustive absence or an atomic search-and-inspection operation.
