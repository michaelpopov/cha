# Flex processing for note summaries

Date: November 12, 2026

Status: Proposed technical design. No application changes are implemented by
this document. The user selected Flex rather than Batch; the policies below
are proposals for review, not additional user requirements. Section 7 includes
the proposed visible failure message and its visual representation.

## 1. Purpose and ideal outcome

Use discounted Flex processing for note summaries sent directly to OpenAI or
Google. Keep the existing summary algorithm and storage rules. A successful
run produces the same kinds of L1, L2, and folder Summary documents, with
lower inference cost and potentially longer waiting time.

The ideal outcome is:

- Eligible calls use Flex, including retries and the final folder overview.
- Temporary capacity failures can recover through a small, bounded retry loop.
- A request cannot wait forever. Cancel works during requests and retry waits.
- Successful summaries remain saved when another request fails.
- An incomplete run is visible to the user, with details in Settings → Events.
- The implementation adds no job database, scheduler, queue, or resume system.

Flex changes scheduling and price, not the selected model or prompts. It does
not guarantee equal wording, latency, availability, or successful completion.

## 2. Scope and provider selection

Apply Flex automatically to the four operations behind
`notes.summaryGenerate`: `l1`, `l2`, `l3_section`, and `l3_overview`, when the
resolved provider meets the conditions below.

| Provider | Proposed behavior |
| --- | --- |
| Direct OpenAI API, API-key authentication, Responses or Chat Completions | Send `service_tier: "flex"`. |
| Direct Gemini Developer API, API-key authentication, Google API format | Send top-level `serviceTier: "flex"`. |
| ChatGPT subscription, OpenRouter, another compatible endpoint, Anthropic, or test mode | Keep the existing behavior. Do not send a Flex field. |

Use the existing direct-OpenAI host helper. Recognize Google's direct host as
`generativelanguage.googleapis.com`, with the same case and trailing-dot
normalization. Production eligibility also requires HTTPS and port 443.
Protocol compatibility alone does not establish support for a vendor's tier.

There is no new persisted setting or UI toggle in this proposal. This is the
smallest interpretation of the decision to use Flex for summaries. Adding a
user-selectable Standard/Flex mode can be a separate decision if required.

This scope excludes conversation **Save summary**, note compression, chat,
note editing shortcuts, and audio. They share some provider helpers, so the
implementation must apply the override only at the note-summary call site.

Model availability remains the provider's responsibility. Do not maintain a
model-name allowlist or make a separate capability-probe request. An eligible
endpoint with a model that rejects Flex fails that request and records the
failure. It does not silently switch models or tiers. Existing configurations
for other providers continue to work; obsolete settings are ignored with a
warning, not treated as a reason to block an otherwise valid operation.

### Why there is no automatic Standard fallback

The purpose is discounted processing. Automatic fallback would change the cost
policy precisely when Flex is busy. It is particularly easy to get wrong in
CHA: direct OpenAI requests currently request **Priority**, not Standard.
Removing a Flex override would restore Priority in the existing builders.

Keep every retry on Flex. Exhaustion becomes an ordinary summary failure.
This makes the cost policy predictable, although it reduces completion rates
when spare capacity is scarce.

## 3. Existing code to reuse

| Code | Current behavior relevant to this design |
| --- | --- |
| `webapp/src/notes/summaries.ts` | Processes notes in tree order, one note at a time. Up to three calls run concurrently for that note. It converts Markdown and commits each successful level. |
| `src/notes/notes_operations.cpp` | Prepares the provider, collects a complete answer, retries eligible failures, checks cancellation, and records final outcomes. |
| `src/providers/provider_client.cpp` | Uses libcurl, enforces transport timeouts, reports `GenerationResult.retryable`, and stops on cancellation. |
| OpenAI and Google request builders | Construct provider-specific JSON from a resolved `ModelBackendConfig`. |
| `App.tsx` and `NotesTree.tsx` | Own the blocking dialog, refresh/reload, action cleanup, and an existing error area. |

The summary PRD already requires continuation after a failed note, independent
L1/L2 saves, and preservation of the folder Summary when L3 fails. Retain those
rules. This proposal does not introduce fail-fast batch behavior or a rollback
of successful work.

## 4. Request construction

Add one internal, default-false `flex_processing` flag to the resolved
`ModelBackendConfig`. It describes this request's execution policy. Do not
serialize it into workspace settings, provider DTOs, exports, or sync records.
The existing provider configuration is copied for each operation; there is no
need for a second policy store or a general service-tier framework.

At `generate_summary_part`, prepare a summary-specific provider definition:

1. Reuse the current Notes provider, model, credentials, prompt, reasoning
   effort, streaming mode, and output limits.
2. Determine whether the direct endpoint is eligible for Flex.
3. For eligible calls, set the internal flag and the timeout values in section
   5 before creating the immutable definition used by all attempts.
4. Leave other callers of `prepare_note_provider` on their existing path.

In each OpenAI builder, give the Flex flag precedence over the existing
Priority branch. Google adds the field to its existing request body. Keep
streaming and non-streaming decoding unchanged. Add the requested tier to
ordinary request diagnostics so a live check can verify routing without
recording note contents or credentials. A requested tier alone is not proof
of billing; inspect provider usage when available during validation.

## 5. Timeouts and retries

### 5.1 Timeouts

Use fixed constants for eligible Flex summary calls:

| Limit | Proposed value | Reason |
| --- | --- | --- |
| Connection establishment | Existing 10 seconds | A slow network connection is different from waiting for model capacity. |
| Total time per attempt | 900 seconds (15 minutes) | Allows a long capacity/reasoning wait while bounding each HTTP attempt. |
| Idle time after response body bytes begin | 900 seconds | An early body frame or heartbeat must not reintroduce the normal short idle limit. The total limit still bounds the request. |

These are local policy choices, not provider guarantees. They override the
configured timeouts only on the copied Flex summary definition. Other
operations retain their behavior. No folder-wide deadline is introduced.

Today, `prepare_note_provider` sets `timeout_s = 0`. Also, the transport's idle
timer starts at the first response **body** byte; response headers do not start
it. Thus an existing summary can wait indefinitely before the first body byte.
Setting the total limit is necessary; raising only the idle limit is not enough.
Reuse libcurl's existing timeout and cancellation callback. Do not add a timer
thread. The same total limit applies to streaming and non-streaming calls.

### 5.2 Retry policy

Retain the existing maximum of **four attempts total**, with waits of **10,
20, and 40 seconds** before attempts two, three, and four. Each attempt uses
the same source text, model, prompt, and Flex tier. The existing loop already
captures source text before starting the worker and creates a fresh provider
client for each attempt.

| Outcome | Automatic retry? |
| --- | --- |
| HTTP 429 capacity/rate-limit rejection | Yes, within the attempt limit. |
| HTTP 5xx rejection, including Google's 503 capacity response | Yes, within the attempt limit. |
| DNS/proxy resolution failure or failure to connect before sending | Yes; retain the existing transport classification. |
| Recognized authentication, permission, exhausted billing/quota, or invalid input/model/tier | No. Repeating the same request cannot repair these conditions. |
| Local total timeout, idle timeout, HTTP 408, interrupted transfer, or partial response | No. Completion and charge status can be uncertain; avoid automatically duplicating work. |
| Refusal, incomplete output, empty output, oversize result, conversion failure, or save failure | No. These are not temporary capacity failures. |
| Cancel, vault change, or connection cleanup | No. |

Use `GenerationResult.retryable` as the retry signal, rather than parsing an
arbitrary model answer or creating a second HTTP-status taxonomy in the Notes
service. For Flex, known permanent provider diagnoses must take precedence
over the broad 429/5xx rule. The current transport marks all 429 responses
retryable, even when its existing error classifier recognizes exhausted quota;
correct that precedence for this path without changing other workflows' retry
policies. Keep provider-error classification separate from generated text.

No additional SDK retry layer exists: CHA uses libcurl. The Notes loop remains
the sole owner of these retries. Keep the current short cancellation checks
during backoff; never replace them with one uninterruptible sleep. Do not hold
the lifecycle mutex while waiting or making a network request.

Each new attempt discards the previous attempt's answer. Only a complete,
validated result can proceed to conversion and saving. Do not retry commits.

Why keep these waits: they already exist, are easy to understand, and recover
brief capacity failures without adding a long-lived queue. They are not a
promise to outwait an outage. This first version does not add jitter or
`Retry-After` header handling; that limitation is explicit in section 9.

The request-time envelope for four attempts is approximately **61 minutes
10 seconds**, plus local overhead, if each attempt runs nearly to its limit
and returns a retryable rejection. A local timeout itself ends retries, so
four consecutive local timeouts cannot occur under this policy. A whole
folder can take substantially longer. Cancel remains available during both
requests and waits.

## 6. Failure, saving, and cancellation behavior

Keep the existing per-part rules after retries are exhausted:

| Failed part | What happens next |
| --- | --- |
| L1 or L2 | Preserve its previous version, if any. Other calls for this note and later notes continue. |
| L3 section | Skip subsequent L3 sections and the overview. Continue L1/L2 work. Preserve the existing folder Summary. |
| L3 overview | Do not replace the folder Summary. Keep successful L1/L2 results. |
| Markdown conversion or commit | Record the failure and retain the existing independent-save rules. Do not regenerate automatically. |

Use the existing Events recorder for one final error per failed part, including
the part, target, and safe reason. Retry attempts belong in ordinary diagnostic
logs, not separate Events errors: a recovered attempt is not a failed summary.
Log the attempt number and chosen delay without note contents or credentials.
Do not expose raw provider response bodies in UI copy.

Retain the existing cancellation flags, connection ownership, and vault-epoch
checks. Cancel interrupts requests and backoff, prevents new attempts and new
note work, and discards late generation output. Already saved L1/L2 documents
stay saved. A commit already sent is allowed to settle. Cancel stays disabled
while the final folder Summary commit is in progress.

Cancel means CHA stops waiting and sending more work. It does not guarantee a
provider refunds or immediately stops a request it already accepted.

A rerun of a folder skips existing L1/L2 versions and regenerates all L3
sections. It is not a persistent resume operation. A rerun on an individual
note regenerates both levels, as today. A lost commit reply can leave an
uncertain outcome; refresh the store rather than resend the commit blindly.

## 7. User-visible completion result

Retain the blocking progress dialog, its existing note count, and Cancel.
Do not add a background-mode switch, retry countdown, top title, or helper
text. The backend's retry loop needs no new progress notification protocol.

Propose one visible addition: after a run with failures, close the dialog,
finish refresh/reload and release the action guard, then show this message in
the existing Notes tree error area:

> Some summaries could not be generated or saved. Check Settings → Events.

This also covers a run in which every attempted summary failed. Do not show
this message merely because the user cancelled. If the run was cancelled,
individual failures already recorded in Events remain available there.

Visual representation of the proposed behavior (existing styles and controls):

```text
During generation — existing modal
┌───────────────────────────────────────────────┐
│  ◌  Summarizing “Chapter 4”…                   │
│     Note 4 of 12                              │
│                                     [Cancel]  │
└───────────────────────────────────────────────┘

After completion with failures — modal closed
┌───────────────────────────────────────────────┐
│  [existing Notes tree controls]               │
│  Some summaries could not be generated or     │
│  saved. Check Settings → Events.              │
│                                               │
│  ▾ Book                                       │
│      Chapter 1                                │
│      Chapter 2                                │
│      …                                        │
└───────────────────────────────────────────────┘
```

The new copy is a proposed UI change. It is included here for review before
implementation; creating this design file does not implement or approve it.

Implementation uses only one extra transient boolean, `hadFailures`, in the
result of `summarizeAndWait`. Set it for generation, conversion, abandon, and
commit failures. Skipped existing levels and cancellation are not failures.
Catch conversion failures explicitly: `summaryAbandon` can succeed even
though conversion failed, so rejected promises alone are insufficient.

Return the flag with `targetIds` and `folderSummaryCommitted`; do not throw
inside the summary helper merely to report partial failure. `App.tsx` needs
those existing result fields to reload successfully changed documents. After
normal cleanup, report a safe `ChaError` through the existing `onSummarize`
rejection path to `NotesTree.setError`. Check cancellation and the context
before presenting it. A real lifecycle or refresh exception retains its own
error; the summary notice must not replace that error. No new wire DTO, list
of results, or persistent failure counter is needed.

## 8. Validation before enabling

Extend the existing tests rather than build a separate harness:

1. **Request builders:** OpenAI Responses and Chat Completions send Flex, not
   Priority, for eligible summary definitions. Google sends the correct field.
   Unrelated operations and excluded endpoints retain their existing bodies.
2. **Summary worker:** All four parts use the override. Verify four-attempt
   bounds, the 10/20/40 waits, permanent-error precedence, no tier fallback,
   and no retry after timeout or partial output. Use the existing wait hook.
3. **Transport:** Verify a silent first response is bounded, body activity
   cannot defeat the total deadline, and Cancel interrupts a silent request.
   Use short test limits and existing local HTTP fixtures, not real 15-minute
   sleeps. Cover streaming and non-streaming behavior.
4. **Saving and UI:** Keep tests for independent commits and L3 failure.
   Add coverage for conversion failure, mixed success, all failures, Cancel,
   and vault changes. Confirm successful documents reload before the error
   appears and the modal/action guard always clear.
5. **Live smoke checks during implementation:** With configured supported
   OpenAI and Gemini models, run a small note and a small folder. Record the
   model, requested/returned tier where available, duration, attempt count,
   and outcome. Confirm actual usage pricing separately. Do not induce an
   outage or claim that live success tests retry behavior. Mock errors do that.

Tests should cover the behavior above in the existing provider, Notes, and
webapp suites. Live checks use the user's existing credentials without logging
them. No live calls are needed to create this proposal.

## 9. Known hurdles and accepted limits

- **A blocking dialog can last a long time.** Notes are deliberately processed
  one at a time under the current PRD. For example, 30 notes at two minutes
  each take roughly an hour before the overview. This is an illustration, not
  a latency estimate. Measure representative folders before treating Flex as
  a comfortable default; a nonblocking workflow would be a separate design.
- **Flex capacity can remain unavailable.** Three retries may all fail. Longer
  backoff or provider retry hints could improve recovery, but this proposal
  keeps the existing bounded mechanism. It does not honor `Retry-After`, and
  parallel calls can retry together. Revisit only with observed evidence.
- **The 15-minute bound can reject legitimate slow work.** It is a deliberate
  tradeoff against an unlimited wait. A timed-out request may still incur cost;
  neither local timeout nor Cancel establishes that no work happened remotely.
- **One exhausted L3 section still prevents a new folder Summary.** This is
  the established completeness rule. Keeping successful sections for later
  runs would require additional state and stale-source rules, so it is excluded.
- **Model and endpoint support can change.** There is no capability registry.
  An unsupported model can fail repeatedly across a folder because the PRD
  says to continue after failures. Cancel is the escape; no circuit breaker
  or new fail-fast rule is introduced here.
- **Some diagnostics remain broad.** The current classifier can label a Flex
  capacity 429 as a rate-limit failure and can reduce unknown model/tier errors
  to a generic public error. Retain safe messages and provider request IDs in
  diagnostics rather than build a large new error taxonomy for this change.
- **Savings depend on the baseline and actual usage.** OpenAI currently uses
  Priority in CHA, so its savings are not simply 50% of today's bill. Retries,
  reasoning tokens, caching, and uncertain interrupted requests affect totals.
- **No restart recovery.** Closing CHA or changing its context interrupts the
  local flow. Completed saves survive, but unsaved sections are not durable.

## 10. Source basis

Checked November 12, 2026. These documents establish provider behavior; the
specific limits, retry policy, and integration choices above are CHA proposals.

- [OpenAI Flex processing](https://developers.openai.com/api/docs/guides/flex-processing):
  Responses and Chat Completions accept `service_tier: "flex"`; Flex uses
  Batch token rates, has limited model availability, and can return an uncharged
  429 capacity rejection. Longer timeouts and client retry decisions are needed.
- [Gemini Flex on Generate Content](https://ai.google.dev/gemini-api/docs/generate-content/flex-inference):
  Flex supports the existing Generate Content path, has a 50% Standard-price
  discount, best-effort capacity, and a 1–15 minute latency target. Google
  documents 503/429 failures, longer client timeouts, and no automatic upgrade
  to Standard. These are not completion guarantees.
- [Gemini Generate Content API](https://aistudio.google.com/api-reference/generate-content/generate-content#request-body):
  The REST request/response schema uses the `serviceTier` field.
- Repository basis: [summary-prd.md](summary-prd.md), especially sections 6–6.3,
  and the current implementations named in section 3. This document proposes
  no changes to `docs/` or to those existing design files.
