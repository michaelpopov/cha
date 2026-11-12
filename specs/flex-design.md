# Flex processing for note summaries

Date: November 12, 2026

Status: Proposed technical design. No application changes are implemented by
this document. The user selected Flex rather than Batch; the policies below
are proposals for review, not additional user requirements. This design has
no visible UI change.

## 1. Purpose and ideal outcome

Use discounted Flex processing for note summaries sent directly to OpenAI or
Google. Keep the existing summary algorithm and storage rules. A successful
run produces the same kinds of L1, L2, and folder Summary documents, with
lower inference cost and potentially longer waiting time.

The ideal outcome is:

- Eligible calls use Flex, including retries and the final folder overview.
- Temporary capacity failures can recover through the existing bounded retry
  loop.
- Cancel works during requests and retry waits, as today.
- Successful summaries remain saved when another request fails.
- Failures are recorded in Settings → Events, as today.
- The implementation adds no job database, scheduler, queue, resume system,
  timeout, or UI change.

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

Use the existing direct-OpenAI host helper, as the Priority rule does today.
Recognize Google's direct host as `generativelanguage.googleapis.com`, with
the same case and trailing-dot normalization. Eligibility depends on the host
only, like the existing Priority rule. Protocol compatibility alone does not
establish support for a vendor's tier.

There is no new persisted setting or UI toggle in this proposal. This is the
smallest interpretation of the decision to use Flex for summaries. Adding a
user-selectable Standard/Flex mode can be a separate decision if required.

This scope excludes conversation **Save summary**, note compression, chat,
note editing shortcuts, and audio. They share some provider helpers, so the
implementation must apply the override only at the note-summary call site.

Model availability remains the provider's responsibility. Do not maintain a
model-name allowlist or make a separate capability-probe request. An eligible
endpoint with a model that rejects Flex fails that request and records the
failure. It does not silently switch models or tiers. Because there is no
toggle and no fallback, a Notes model without Flex support cannot make
summaries on an eligible endpoint. The only remedy is to select another Notes
provider or model. That selection is shared with note compression and the
other note operations (section 9).

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
3. For eligible calls, set the internal flag before creating the immutable
   definition used by all attempts.
4. Leave other callers of `prepare_note_provider` on their existing path.

In each OpenAI builder, give the Flex flag precedence over the existing
Priority branch. Google adds the field to its existing request body. Keep
streaming and non-streaming decoding unchanged. Add no new diagnostics: the
existing debug log already records each request body, which contains the
tier, and each raw response. A requested tier alone is not proof of billing;
inspect provider usage when available during validation.

## 5. Timeouts and retries

### 5.1 Timeouts

Make no timeout change. Today, `prepare_note_provider` sets `timeout_s = 0`,
so a summary call has no total time limit. The transport's idle timer starts
at the first response **body** byte; response headers do not start it. Thus a
long Flex wait before the first body byte already works. Both providers ask
clients to allow long waits (OpenAI suggests 15 minutes, Google 10 minutes or
more); CHA already allows more. Cancel interrupts a silent request through the
existing libcurl progress callback.

The configured idle limit still applies after the first body byte. If a
provider sends early stream events and then waits for capacity, the idle
limit ends that call. The live check in section 8 confirms that streaming
Flex calls are not cut this way.

### 5.2 Retry policy

Retain the existing maximum of **four attempts total**, with waits of **10,
20, and 40 seconds** before attempts two, three, and four. Each attempt uses
the same source text, model, prompt, and Flex tier. The existing loop already
captures source text before starting the worker and creates a fresh provider
client for each attempt.

| Outcome | Automatic retry? |
| --- | --- |
| HTTP 429 (capacity, rate limit, or quota) | Yes, within the attempt limit, as today. |
| HTTP 5xx rejection, including Google's 503 capacity response | Yes, within the attempt limit. |
| DNS/proxy resolution failure or failure to connect before sending | Yes; retain the existing transport classification. |
| Other HTTP errors, for example authentication, permission, or invalid input/model/tier | No. Repeating the same request cannot repair these conditions. |
| Idle timeout, HTTP 408, interrupted transfer, or partial response | No. Completion and charge status can be uncertain; avoid automatically duplicating work. |
| Refusal, incomplete output, empty output, oversize result, conversion failure, or save failure | No. These are not temporary capacity failures. |
| Cancel, vault change, or connection cleanup | No. |

Use `GenerationResult.retryable` unchanged as the retry signal. Make no change
to the error classifier: a 429 that reports exhausted quota is retried like
any other 429, as today. A 429 rejection is not charged, so those retries cost
about 70 seconds of waiting and no tokens.

No additional SDK retry layer exists: CHA uses libcurl. The Notes loop remains
the sole owner of these retries. Keep the current short cancellation checks
during backoff and the current lock rules.

Each new attempt discards the previous attempt's answer. Only a complete,
validated result can proceed to conversion and saving. Do not retry commits.

Why keep these waits: they already exist, are easy to understand, and recover
brief capacity failures without adding a long-lived queue. They are not a
promise to outwait an outage. This first version does not add jitter or
`Retry-After` header handling; that limitation is explicit in section 9.

A retryable rejection can come after a long Flex wait, so one summary call can
take much longer than its retry waits suggest. A whole folder can take
substantially longer. Cancel remains available during both requests and
waits.

## 6. Failure, saving, and cancellation behavior

Keep the existing per-part rules after retries are exhausted:

| Failed part | What happens next |
| --- | --- |
| L1 or L2 | Preserve its previous version, if any. Other calls for this note and later notes continue. |
| L3 section | Skip subsequent L3 sections and the overview. Continue L1/L2 work. Preserve the existing folder Summary. |
| L3 overview | Do not replace the folder Summary. Keep successful L1/L2 results. |
| Markdown conversion or commit | Record the failure and retain the existing independent-save rules. Do not regenerate automatically. |

The existing Events recorder keeps one final error per failed part, including
the part, target, and safe reason. As today, retry attempts record no Events
errors: a recovered attempt is not a failed summary. The debug log shows each
attempt's request. Do not expose raw provider response bodies in UI copy.

Retain the existing cancellation flags, connection ownership, and vault-epoch
checks. Cancel interrupts requests and backoff, prevents new attempts and new
note work, and discards late generation output. Already saved L1/L2 documents
stay saved. A commit already sent is allowed to settle. Cancel stays disabled
while the final folder Summary commit is in progress.

Cancel means CHA stops waiting and sending more work. It does not guarantee a
provider refunds or immediately stops a request it already accepted.

A rerun of a folder skips existing L1/L2 versions and regenerates all L3
sections. It is not a persistent resume operation. A rerun on an individual
note regenerates both levels, as today.

## 7. User-visible behavior

No UI change. The blocking progress dialog, its note count, and Cancel stay as
they are. Failures are recorded in Settings → Events, as the summary PRD
requires. There is no retry countdown and no new progress notification.

## 8. Validation before enabling

Extend the existing tests rather than build a separate harness:

1. **Request builders:** OpenAI Responses and Chat Completions send Flex, not
   Priority, for eligible summary definitions. Google sends the correct field.
   Unrelated operations and excluded endpoints retain their existing bodies.
   Cover the host cases of the existing Priority test, plus the Google host.
2. **Summary preparation:** An eligible Notes provider gets the Flex flag for
   summary parts. Note compression and the other note operations do not. All
   four parts go through `generate_summary_part`, so one call site covers
   them. The retry loop and classifier do not change, so the existing retry
   tests stay valid.
3. **Live smoke checks during implementation:** With configured supported
   OpenAI and Gemini models, run a small note and a small folder, with
   streaming on and off. Confirm that the idle limit does not end a Flex call.
   Record the model, requested/returned tier where available, duration,
   attempt count, and outcome. Confirm actual usage pricing separately. Do not
   induce an outage or claim that live success tests retry behavior.

Live checks use the user's existing credentials without logging them. No live
calls are needed to create this proposal.

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
- **There is no total time limit.** As today, a summary call can wait as long
  as the provider holds it; Cancel is the local stop. A cancelled or
  idle-timed-out request may still incur cost; neither establishes that no
  work happened remotely.
- **One exhausted L3 section still prevents a new folder Summary.** This is
  the established completeness rule. Keeping successful sections for later
  runs would require additional state and stale-source rules, so it is excluded.
- **Model and endpoint support can change.** There is no capability registry.
  An unsupported model can fail repeatedly across a folder because the PRD
  says to continue after failures. Cancel is the escape; no circuit breaker
  or new fail-fast rule is introduced here. With no toggle and no fallback,
  summaries on an eligible endpoint work only with a Flex-capable Notes model.
  To change the model, the user changes the provider for note compression and
  the other note operations too.
- **Some diagnostics remain broad.** The current classifier can label a Flex
  capacity 429 as a rate-limit failure and can reduce unknown model/tier errors
  to a generic public error. A 429 for exhausted quota is retried three times
  before it fails. Retain safe messages and provider request IDs in
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
