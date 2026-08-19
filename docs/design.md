# Design: model-provider communication and context caching

## Purpose

This report aggregates the seven provider investigations written on
2026-08-19:

- [Pi detailed report](grok-report.md) and
  [Pi recommendations](provider-communication-report.md);
- [OpenCode detailed report](grok-opencode.md) and
  [OpenCode recommendations](provider-communication-opencode-report.md);
- [Tau detailed report](grok-tau.md) and
  [Tau recommendations](provider-communication-tau-report.md);
- [earlier Pi summary](provider-communication.md).

The reports agree on the important points. Their differences are mostly about
retry parameters, cache configuration names, and provider-specific affinity
headers. This document resolves those differences into one CHA-sized design.

The primary goal is to reduce inference cost by making provider-side prompt
caching effective. The secondary goal is to make provider calls bounded,
retryable when safe, and easier to diagnose.

## Executive decision

Implement provider improvements in two phases:

**Phase 1: Cost measurement (highest ROI first)**

1. Make the prompt prefix stable and log provider-reported cached tokens.
2. Add a per-session-and-character cache key for direct OpenAI requests.

These items unlock cache effectiveness measurement. Phase 1 completion means:
logs show `cache_read_tokens` on repeated turns, same `prompt_cache_key`
sent on each turn, and stable system prefix across submissions. This gives
evidence of whether provider-side caching is working, which directly answers
the cost-reduction question.

**Phase 2: Resilience and diagnostics (after Phase 1 shows cache benefit)**

3. Add bounded timeouts, pre-output retries, and optional generation fields.
4. Classify overflow and improve diagnostic metadata.

Phase 2 improves reliability (transient failures recover gracefully, overflow
is detected, diagnostics are clear). Do not start Phase 2 until Phase 1 logs
show provider caching is effective.

Do not add stored provider conversations, `previous_response_id`, a provider
SDK, a compatibility framework, automatic summarization, token-price catalogs,
or history truncation unless logs demonstrate a real need.

## Current state

CHA already has a suitable architecture:

- `ProviderClient` owns libcurl, authentication, discovery, and HTTP outcomes.
- Chat Completions and Responses have separate request/stream codecs.
- `ModelBackend::prepare` is separate from the slow `perform` call.
- `store: false` keeps CHA's SQLite transcript authoritative.
- cancellation interrupts curl and is distinct from transport failure;
- terminal usage is already parsed as input and output tokens;
- provider configuration remains a small, closed TOML schema.

The important current gaps are:

- `project_model_context()` puts the current submission time in the system
  prompt, changing the beginning of every request;
- no cache key or affinity hint is sent;
- cached input tokens are not parsed or logged;
- generation has no overall or idle timeout;
- a transient 429, 5xx, or connection reset fails the turn immediately;
- `temperature = 1.0` is sent even when it was not configured;
- output is not bounded with `max_tokens` / `max_output_tokens`;
- overflow, quota, authentication, and malformed responses mostly collapse
  into generic protocol errors.

## Provider-side context caching

### How the cache works

OpenAI-compatible provider caches generally reuse a token prefix from an
earlier request. The system instructions, tool definition, and leading
messages must be identical. New material may be appended at the tail without
invalidating the earlier prefix.

This cache is independent of stored conversation state. CHA should retain
`store: false`; a prompt cache key is a routing hint, not a conversation ID
that lets the provider own history.

A cache key cannot repair a changing prefix. Prefix stability is therefore the
first and required change.

### 1. Freeze the system prefix

Remove the per-turn sentence currently appended to the system message:

```text
Conversation timestamps are in UTC. The current prompt was submitted at ...
```

Keep the timestamp already attached to the final user message and the stable
timestamps stored on historical entries. This leaves the model with the same
time information while moving the changing value to the request tail.

If a system-level time hint is desired, use only a stable sentence such as
`Conversation timestamps are in UTC.` A calendar date that changes once per
day is acceptable but unnecessary; omitting it is simpler and maximizes cache
life.

The invariant is:

> For the same character, forum definition, persona roster, tools, and stored
> history, changing only the current submission time must not change the
> system prompt or any earlier message.

The character prompt, forum prompt, roster, and hosted `web_search` tool shape
are already stable during a live session. A workspace reload, persona change,
provider change, `/hide`, or another deliberate history rewrite may cause a
cache miss. Those misses are correct.

OpenCode's persisted “Context Epoch” is not needed. CHA already freezes the
loaded workspace for a live generation, and workspace reload has explicit
lifecycle semantics.

### 2. Use a cache key scoped to the real prompt

Use one key per forum session and target character:

```text
<forum-id>/<session-id>/<character-id>
```

The character ID is required because each character sees a different
projection of the same forum transcript. A forum-wide or session-only key
would create noisy affinity between different prompt prefixes.

If the composed key is at most 64 ASCII characters, send it unchanged. If it
is longer, replace it with SHA-256, outputting the first 64 characters of the
hex digest. SHA-256 is deterministic across process restarts, collision-resistant,
and available in C++ via OpenSSL. Do not use `std::hash`, whose representation
is not a persistence contract.

Plumb the identity through existing structures:

1. pass `SessionIdentity` from `open_session()` into `SessionController`;
2. add a cache-key string to `RunSpec` or `GenerationRequest`;
3. construct the key in `start_batch()` for each target character;
4. let the request builders and `ProviderClient` consume it.

An empty identity in narrow unit-test constructors should simply omit caching.

### 3. Provider behavior

Start with behavior that is supported by the reports and by CHA's checked-in
providers:

| Provider path | Body | Headers | Notes |
| --- | --- | --- | --- |
| Direct OpenAI Responses | `prompt_cache_key` | `session_id` | Primary CHA caching path; may also support explicit retention. |
| Direct OpenAI Chat Completions | `prompt_cache_key` | none | Tau specifically recommends no affinity header here. |
| Gemini OpenAI-compatible | none | none | Rely on implicit stable-prefix caching and parse reported cache usage. |
| OpenRouter Chat Completions | none initially | none initially | Add `x-session-id` only after its effect is documented or measured for CHA's models. |
| Other compatible endpoints | none by default | none | Opt in only when a checked-in provider needs it. |

Do not set `x-client-request-id` to the session key. Tau's investigation
corrects the earlier Pi report here: it is a per-request diagnostic identifier,
not session affinity. Reusing the session key would make request tracing
ambiguous.

Do not infer broad compatibility from `base_path`. Direct-host matching is a
safe default; a future explicit provider flag can opt a gateway into a key or
header without introducing a compatibility matrix.

### 4. Cache configuration

Use one provider-level option:

```toml
cache_retention = "short"  # off | short | long
```

Semantics:

- `off`: omit cache keys, affinity headers, and retention fields;
- `short` (default): send the supported key/header and use provider-default
  in-memory retention;
- `long`: on direct OpenAI Responses, additionally send
  `prompt_cache_retention = "24h"`; elsewhere behave as `short` until that
  provider is explicitly supported.

This retains the useful Pi distinction while avoiding separate
`prompt_cache`, `cache_ttl`, and affinity options. `long` must remain opt-in
because provider support and billing treatment vary.

Do not put the generated `prompt_cache_key` in TOML. It is request identity,
not connection configuration.

### 5. Observe cache behavior before claiming savings

Extend `GenerationTokenUsage` with:

```cpp
std::optional<std::size_t> cache_read_tokens;
```

Parse:

- Responses: `usage.input_tokens_details.cached_tokens`;
- Chat Completions: `usage.prompt_tokens_details.cached_tokens`;
- optionally as a compatibility fallback:
  `usage.prompt_cache_hit_tokens`.

Keep the provider's raw `input_tokens` value and log
`cache_read_tokens` separately. If an uncached count is useful, derive it as
`max(input_tokens - cache_read_tokens, 0)` and name it explicitly. Do not
silently redefine `input_tokens`, because providers generally report it as an
inclusive total.

Add cache tokens to the existing sanitized HTTP completion log. No cache-rate
widget or dollar-cost calculation is needed. Logs and the provider billing
dashboard are enough for this application.

Expected validation:

1. on two requests with the same history but different submission times,
   system instructions are identical;
2. a sufficiently long direct-OpenAI conversation sends the same
   session/character key on each turn;
3. after the cache is warm, later completions report non-zero cached tokens;
4. `store` remains false and no `previous_response_id` or `conversation` field
   appears.

Short prompts may not cross a provider's minimum cacheable prefix, and a cold
or evicted cache may report zero. Neither is a CHA failure.

### Cache invalidation behavior

The following behavior requires no extra machinery:

- appending a new user turn preserves the earlier prefix;
- growing the final shared-history JSONL block preserves its existing prefix;
- multicast uses one key per target character;
- `/hide`, off-record splicing, or failed-turn removal intentionally changes
  the projected history;
- workspace reload and prompt/config changes intentionally produce a new
  prefix;
- a provider or model switch may reuse history but should be expected to miss
  on the new backend.

## Provider resilience and request hygiene

These recommendations are secondary to caching but strongly supported by all
three source investigations.

### Bounded generation

Add provider TOML settings:

```toml
timeout_s = 600
idle_timeout_s = 60
```

Keep the existing 10-second connection timeout. For generation, set an overall
10-minute timeout and use curl's low-speed limit/time so a silent SSE stream
dies after about one minute. Both values must be positive; do not provide an
“unbounded” sentinel unless a real provider requires it.

### Safe retries

Retry inside `ProviderClient::perform`, never in `GenerationBatch` or the
controller. A retry is allowed only before `on_delta` has published any answer
or reasoning text.

Recommended policy:

- two retries after the initial attempt;
- transient statuses `{408, 409, 425, 429}` and all `>= 500` statuses;
- transient curl connect, resolve, reset, send, receive, and empty-response
  failures;
- 500 ms exponential backoff with small jitter, computed delay capped at
  10 seconds;
- parse `Retry-After-Ms`, delta-seconds, and HTTP-date `Retry-After`, capped at
  60 seconds;
- sleep in short cancellation-aware intervals;
- log the retry attempt rather than adding transcript entries.

Never retry:

- cancellation;
- 400/401/403/404 and other deterministic client errors;
- quota/billing responses containing `insufficient_quota`, `quota_exceeded`,
  `quota exceeded`, `out of budget`, or `billing`;
- context overflow;
- a transfer after its first visible delta.

A provider error event inside an HTTP-200 SSE stream may use the same retry
budget only when it is clearly transient and arrived before any delta. This is
useful but can follow the simpler HTTP retry implementation.

### Optional request fields

Change `temperature` to optional. If absent in provider TOML, omit it from the
JSON body. Add optional `max_tokens`:

- Chat Completions: `max_tokens`;
- Responses: `max_output_tokens`, clamped to the provider's minimum accepted
  value (the reports use 16).

Do not add `top_p`, a request overlay bag, or a model-name routing table.

For Chat Completions automatic reasoning extraction, probe
`reasoning_content`, then `reasoning`, then `reasoning_text`, taking the first
non-empty field.

### Errors and diagnostics

Classify errors while keeping CHA's small public outcome enum:

- rate limit: retry, then protocol error if exhausted;
- quota: no retry, stable quota message;
- authentication/permissions: no retry;
- context overflow: no retry, stable public message;
- 5xx/connect/reset: retry before output;
- content policy and malformed success bodies: no retry.

Use exclusion-first overflow matching so rate-limit phrases containing words
such as “limit” are not mistaken for context overflow. Match these concrete
overflow patterns (check in this order):

- `”context window”`, `”maximum context length”`, `”prompt is too long”`,
  `”context length exceeded”`, `”exceeds the context window”`,
  `”input token count”` (as prefix, e.g., “input token count X exceeds Y”)
- HTTP 400 with no body or generic “bad request” message
- HTTP 413 (Payload Too Large), especially from llama.cpp

Match these quota/billing patterns as explicitly non-retryable:

- `”insufficient_quota”`, `”quota exceeded”`, `”quota_exceeded”`,
  `”out of budget”`, `”billing”` (in error message body)

Public overflow text should be stable, for example:

```text
Prompt exceeds the model's context window.
```

Keep the vendor body in sanitized diagnostics, capped to a small bound such as
4 KiB. Never echo an unbounded provider body into the transcript.

Capture and log the first available request identifier from:

```text
x-request-id
openai-request-id
request-id
x-goog-request-id
x-amzn-requestid
x-amz-request-id
cf-ray
```

Do not send these response identifiers back as affinity values.

### Context-window warning

Do not add compaction or a tokenizer. Optionally add `context_tokens` to a
provider config. After a successful generation, compare reported use with:

```text
usable = context_tokens - reserve
```

Use configured `max_tokens` as the reserve when available, otherwise a fixed
4096-token reserve. When usage crosses the usable threshold, emit a visible
notice entry in the transcript (or diagnostic log if the UI lacks a notice-entry
mechanism). Example: "Context nearly full; consider /hide or a smaller scope."
This gives the user actionable guidance before overflow occurs. Provider usage
is more trustworthy than a local characters-per-token estimate.

If actual sessions begin overflowing, a later projection-only change may drop
the oldest shared-history JSONL entries first, then the oldest completed
other-character turns. Never drop the system prompt or current user message,
and retry a rewritten request at most once before any visible output.

## Implementation slices

### Phase 1: Cost Measurement (Slices 1–2)

Implement only these slices first. Phase 1 is complete when logs show
non-zero `cache_read_tokens` on repeated turns and the same `prompt_cache_key`
is sent on each turn. This gives evidence of whether provider-side caching is
effective.

#### Slice 1: stable prefix and cache metrics

Files:

- `src/agents/model_context.cpp`;
- `src/agents/model_backend.h`;
- `src/agents/provider_response.cpp`;
- `src/agents/responses_api.cpp`;
- `src/agents/provider_client.cpp` logging;
- corresponding model-context and decoder tests.

Acceptance criteria:

- changing only the current timestamp leaves system instructions identical;
- cached token fields parse for both protocols;
- logs show `cache_read_tokens` without exposing prompt contents.

#### Slice 2: direct-OpenAI cache identity

Files:

- `src/agents/model_context.h`;
- `src/session/session_controller.h/.cpp`;
- `src/workspace/session_open.cpp`;
- `src/agents/character_config.h/.cpp`;
- `src/agents/provider_client.h/.cpp`;
- `src/agents/responses_api.cpp`;
- test constructors and unit tests.

Acceptance criteria:

- key is forum/session/character and stable across turns;
- long keys are deterministically hashed;
- direct OpenAI Responses gets body key plus `session_id` header;
- direct OpenAI Completions gets body key only;
- unsupported hosts get no speculative cache fields;
- `off`, `short`, and `long` configuration validates;
- `x-client-request-id` is not reused as affinity;
- `store: false` remains.

### Phase 2: Reliability and Diagnostics (Slices 3–4)

Implement these slices only after Phase 1 shows provider caching is effective.
Phase 2 improves reliability (transient failures recover, overflow is detected)
and diagnostics (clear error messages, request tracing).

#### Slice 3: transport resilience and legal request bodies

Files:

- `src/agents/character_config.h/.cpp`;
- `src/agents/provider_client.cpp`;
- `src/agents/responses_api.cpp`;
- mock HTTP, request-body, and config-loader tests.

Acceptance criteria:

- scripted 429 then 200 succeeds with two requests;
- quota 429, 401, and post-delta failure are not retried;
- cancellation interrupts retry backoff;
- overall and idle timeouts terminate stalled requests;
- unset temperature is absent;
- max token field matches the selected API.

#### Slice 4: overflow and diagnostics

Files:

- `src/agents/provider_client.cpp` and possibly a small nearby helper;
- decoder/error tests;
- documentation for new TOML keys and log fields.

Acceptance criteria:

- overflow is stable and non-retryable;
- rate limit is not misclassified as overflow;
- public error bodies are bounded;
- request IDs are logged;
- empty failed assistant entries are not projected into the next request.

## Deferred improvements

Only implement these in response to observed behavior:

- OpenRouter `x-session-id` or other gateway affinity headers;
- generic `[headers]` TOML for referer, title, organization, or project;
- history truncation after real overflow;
- a shared incremental SSE splitter to replace repeated front erases;
- cache-write token accounting if a checked-in provider reports it;
- Anthropic `cache_control` breakpoints if CHA adds a direct Claude Messages
  provider.

## Explicit non-goals

- official vendor SDKs or a provider factory/catalog layer;
- OAuth and subscription-specific transports;
- provider-owned conversation state, `store: true`,
  `previous_response_id`, or `conversation`;
- automatic LLM summarization or a compaction pipeline;
- local tokenizer integration or approximate cost accounting;
- transcript-visible retry countdowns;
- a broad protocol compatibility flag matrix;
- request-body overlays that bypass CHA's codecs;
- WebSocket Responses or a CHA-owned tool loop;
- dollar pricing in the application;
- one cache key shared by all characters in a forum.

## Recommended implementation order: Phase 1 then Phase 2

**Phase 1 deliverable:** Stable-prefix projection, cache-read usage logging,
and direct-OpenAI cache identity (Implementation Slices 1–2). This unlocks
cost measurement: CHA will preserve the provider's cacheable prefix, route
repeated turns consistently, and log how many input tokens the provider
actually reused. Phase 1 is complete when logs show non-zero `cache_read_tokens`
on repeated turns in the same session.

**Phase 1 expected outcome:** Evidence of whether provider-side caching is
working and what the cost reduction is. If caching is effective, proceed to
Phase 2. If caching is not hitting (zero cached tokens despite stable prefix),
investigate cache eviction or provider configuration before starting Phase 2.

**Phase 2 (after Phase 1 shows cache benefit):** Resilience work (Slices 3–4):
bounded timeouts, retries, overflow detection, and improved diagnostics.
Decide from Phase 1 logs whether long retention, OpenRouter affinity, or
history truncation is worth the additional implementation.
