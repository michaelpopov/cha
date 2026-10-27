# Native Google API support

Status: Technical design

Date: October 27, 2026

Review: [Claude CLI output before these revisions](google-design-claude-review.txt)

## Scope

Add `api = "google"` to use Gemini models directly through the Gemini Developer
API with a saved API key. Support ordinary chat, streaming, non-streaming calls,
CHA's existing tools, provider testing, session naming, and note text operations.
Keep the existing OpenAI-compatible Google configuration working.

Use Google's `generateContent` and `streamGenerateContent` REST endpoints.
Google now labels the Generate Content guides as legacy and also documents the
Interactions API. This design deliberately targets Generate Content: its
stateless requests fit CHA's existing transcript and request lifecycle. Model
support is limited to models that accept these endpoints and text output.
[Google text generation documentation](https://ai.google.dev/gemini-api/docs/generate-content/text-generation).

The first implementation excludes Vertex AI, Google OAuth, model discovery,
Google-hosted search, explicit context caches, media generation, and the Live
and Interactions APIs. Use Google's default thinking behavior; custom thinking
levels and budgets can be added when needed. Jev keeps its separate service API.

This document specifies implementation. It does not change application code.

## Extend the existing provider path

Keep `Providers`, `ProviderClient`, `ModelBackend`, `ApiKeyStore`, libcurl,
`SseFramer`, and the current generation events. Add `google_api.h/.cpp` beside
the two existing protocol adapters, with a request builder, a streaming decoder,
and a complete-response decoder.

Add Google branches to the existing protocol switches. Reuse the current tool
execution loop, cancellation, timeouts, logging, and token aggregation. Replace
places that assume every non-Responses request is Chat Completions with explicit
three-way handling. Small JSON helpers are sufficient; no provider registry,
SDK dependency, transport subclass, or new execution framework is needed.

The only additional state is decoder state for the current response. Tool-round
data uses the existing `GenerationResult::continuation` and request body. There
is no new persistent store, transcript format, cache, or background work.

## Configuration and credentials

Use the existing provider fields. Example configuration, with the model and
saved key ID replaced by the user's values:

```toml
display_name = "Google"
api = "google"
mode = "net"
host = "generativelanguage.googleapis.com"
port = 443
https = true
base_path = "/v1beta"
model = "YOUR_GEMINI_MODEL_ID"
api_key = "api_key_1"
stream = true
```

`api_key` refers to the existing vault key store. Keep the legacy `api_key_env`
lookup by saved key name; do not start reading environment variables. Resolve
credentials in the native backend. A configured key reference that cannot be
resolved uses the existing credential error path. With no key configured, send
no key header and let the endpoint's response use the existing HTTP error
handling. This also keeps local test servers usable without credentials.

Send the key in `x-goog-api-key`, never in the URL or a Bearer header. Reuse
credential redaction and header omission from logs. No new authentication enum
is needed: `auth = "none"` already means no special OAuth flow, not no API key.
[Google API key documentation](https://ai.google.dev/gemini-api/docs/api-key).

Build endpoints from the configured origin and base path:

```text
stream = false: /v1beta/models/{model}:generateContent
stream = true:  /v1beta/models/{model}:streamGenerateContent?alt=sse
```

Accept a bare model ID or one `models/` prefix, normalize it once, and reject
path/query delimiters in the remaining ID. Do not append OpenAI's `/v1` path.
Keep custom origins available through the existing configuration, including
local HTTP test servers.

For Google, omit unused OpenAI/OpenRouter settings such as `reasoning_effort`,
`reasoning_format`, `cache_retention`, `openrouter_targets`, and provider-hosted
`web_search`. Warn at workspace load for explicitly configured non-default
values, including character overrides; do not warn again on each request.
Apply the ignore rule in both workspace loading and settings validation;
old unused settings must not prevent a valid Google request. Keep checks for
values actually used, such as endpoint, model, token limit, and credentials.
CHA's `web_search` function remains available independently of provider-hosted
search.

Naming and note operations also copy their own reasoning effort into the
request. Those settings have no effect for Google, including their defaults
of `low` and `high`; they must not produce per-request warnings or failures.
Keep Google out of the rejection cases in `provider_supports_reasoning_effort()`
so `note_editing_provider()` accepts these unused settings. No additional
capability system is needed.

## Request encoding

Call `project_model_context()` exactly as the other adapters do. This preserves
character instructions, shared forum history, timestamps, compression, and the
current prompt. Translate only the resulting wire representation:

| CHA input | Google request |
| --- | --- |
| System message | `systemInstruction.parts[].text` |
| User message | `contents[]` with `role: "user"` and text parts |
| Assistant message | `contents[]` with `role: "model"` and text parts |
| `temperature`, when set | `generationConfig.temperature` |
| `max_tokens`, when set | `generationConfig.maxOutputTokens` |

Combine adjacent projected messages with the same role into separate text parts
of one content item, preserving their order. Omit empty optional fields. Request
one text candidate. Model and streaming mode belong in the URL, not OpenAI-style
body fields. Omit `thinkingConfig` to retain Google's model defaults.
Preserve projected history that starts with a model reply after compression;
do not discard it or invent a user message.

Extend `update_tool_instructions()` to read Google's function declarations and
write its existing instructions into `systemInstruction`. Retain request text
size diagnostics and the `include_tool_instructions` behavior used by auxiliary
requests. [Google request examples](https://ai.google.dev/gemini-api/docs/generate-content/text-generation).

## Tools and continuation

Extend the existing tool helpers for Google, including web search, page reading,
and Assistant maintenance tools. Wrap their shared names, descriptions, and
parameter schemas in `tools[].functionDeclarations[]`. Use
`parametersJsonSchema` for the existing JSON schemas and omit OpenAI's `strict`
field. Do not copy the tool definitions into a second catalog.
[Function declaration reference](https://ai.google.dev/api/generate-content#FunctionDeclaration).

Every Google request, including the first and every tool continuation, must use
Google fields only. In particular, `append_tool()` and the shared loop must not
insert `tool_choice`, `messages`, or OpenAI-style `content` string fields.

Decode `functionCall` parts into existing `ToolCall` values. Serialize `args` as
the argument string expected by the executors. Preserve supplied call IDs. If a
model omits an ID, use a request-local ordinal for CHA's bookkeeping and omit
the ID from Google's response rather than inventing a provider ID.

For each tool round, append the complete returned model content, then one user
content item containing the ordered `functionResponse` parts. Each response
contains the original function name, supplied ID if present, and a JSON object:
use an executor's JSON object directly or wrap other output in `{"result": ...}`.
Preserve call/result order even when execution is parallel. Retain existing
argument validation, limits, cancellation, progress events, and execution rules.
[Google function calling guide](https://ai.google.dev/gemini-api/docs/generate-content/function-calling.md).

Build each response's name and optional ID from the corresponding `functionCall`
part in `GenerationResult::continuation`, paired by call index. Do not use the
internal `ToolCall::id` to decide whether Google supplied an ID. The continuation
already contains the required distinction; no extra map or ID flag is needed.

Preserve every returned `thoughtSignature` on its original part, including
parts without visible text. Keep all model content from earlier tool rounds in
the current request. Do not decode signatures, merge signed parts, or reconstruct
continuations from displayed answer text. Google requires these signatures for
function calling with some models.
[Google thought signature rules](https://ai.google.dev/gemini-api/docs/generate-content/thought-signatures).

Keep the existing display behavior: each round's text streams immediately, with
a blank line between rounds that contain answer text.

At the tool limit, keep Google's declarations and set
`toolConfig.functionCallingConfig.mode = "NONE"`. This is Google's documented
way to disable further calls. Make `update_tool_instructions()` treat this mode
as unavailable, and put the existing final-answer directive in
`systemInstruction.parts[].text`. Leave the function-response content as the
last conversation item; do not append an OpenAI-style user message. Reject any
further calls through the existing limit error. Do not execute calls from
malformed, blocked, or truncated responses.
[Google function-calling modes](https://ai.google.dev/gemini-api/docs/generate-content/function-calling.md#function_calling_modes).

Signatures are retained for the complete tool loop, then discarded with the
request. Later user turns use CHA's text transcript as they do today. This does
not retain Google's optional reasoning state across user turns; it avoids a
second conversation history.

## Response handling

Use `SseFramer` for streaming JSON events and share the semantic parser with the
non-streaming decoder. Decode candidate zero; ordinary text becomes answer
deltas and text marked `thought` becomes reasoning deltas if returned. Opaque
signatures are never displayed. Accumulate tool parts without losing their
metadata or order.

A Google stream has no OpenAI `[DONE]` requirement. Read through the HTTP end,
including trailing usage and signature parts, and require a terminal candidate
status or explicit prompt-block result. Treat malformed JSON and premature EOF
as protocol failures, preserving text already delivered.

Normalize terminal status for existing consumers:

| Google `finishReason` | CHA result |
| --- | --- |
| `STOP` | `finish_reason = "stop"` |
| `MAX_TOKENS` | `finish_reason = "length"`; preserve partial chat text |
| `SAFETY`, `RECITATION`, `BLOCKLIST`, `PROHIBITED_CONTENT`, `SPII` | `refused = true`, `finish_reason = "content_filter"` |
| Any other terminal value | Protocol error; include the status in the diagnostic |

Preserve text already delivered. Note text operations reject incomplete or
refused answers. Empty answers without calls and prompt blocks without
candidates use the existing error path; an explicit prompt block also sets
`refused`. Use Google's `error.message` with the current size limits and
credential redaction. Keep the current HTTP failure handling and add no retry
or automatic continuation policy.
[Google finish reasons](https://ai.google.dev/api/generate-content#FinishReason).

Map reported usage as follows: input is `promptTokenCount`; output is
`candidatesTokenCount` plus `thoughtsTokenCount`; cache reads are
`cachedContentTokenCount`. Output is unknown only when `candidatesTokenCount`
is absent; a missing `thoughtsTokenCount` contributes zero. Leave missing input
and cache-read counts unknown and cache writes unset. Retain the latest value
of each reported field across chunks, then let the existing loop sum across
requests. Never sum cumulative chunks or count `totalTokenCount` as output.
[Response and usage reference](https://ai.google.dev/api/generate-content#GenerateContentResponse).

## Provider form

Add **Google (Gemini)** to the existing **API format** selector. Reuse Model,
Base URL, Credentials, Test, and Save changes. Keep Base URL explicit; choosing
the API format does not overwrite it or the model. Hide Reasoning effort for
Google because this version uses Google's defaults. Offer saved API keys for
Google; do not offer OpenAI OAuth.

When switching to Google, clear an existing OAuth selection to `auth = "none"`
and no selected key; preserve an existing saved-key selection. In
`candidateUpdate()`, send `reasoning_effort = "none"` for Google so Save and Test
do not retain a hidden effort override. Existing Character, Session naming, and
Notes effort controls remain unchanged and have no effect for Google. Naming
keeps its current timeout and timestamp fallback if default thinking is slow.

To change an existing OpenAI-compatible Google provider to native Google, also
change its Base URL from `/v1beta/openai` to `/v1beta`. This is an explicit edit,
not an automatic migration; the Test action checks the resulting endpoint.

Proposed form fragment, using the current layout and styling:

```text
Model
[ YOUR_GEMINI_MODEL_ID                                  ]

Base URL
[ https://generativelanguage.googleapis.com/v1beta       ]

API format                    Credentials
[ Google (Gemini)       v ]    [ My Google API key     v ]

[ Test ]  [ Cancel ]  [ Save changes ]
```

There is no new panel, title, legend, helper text, or setup flow. The existing
Test action must exercise the selected native protocol. This sketch describes
the visible change for review; this documentation task does not implement it.

## Implementation and validation

1. Add the enum/string conversion and endpoint handling in
   `src/characters/character_config.*`. Update Google-specific validation in the
   workspace and settings paths. Add `google` to `resources/dto.yaml`, regenerate
   `webapp/src/api/schema.d.ts`, and update the client guard and provider form.
   Check the early validation in `src/runtime/request_parser.cpp` as well as
   `provider_config_error()`; unsupported OpenRouter settings must not fail
   before the Google ignore rule applies.
2. Add `src/providers/google_api.*` and register it in `CMakeLists.txt`. Integrate
   request preparation, authentication headers, both decoders, tool helpers, and
   continuation handling in `provider_client.*` and `tool_calls.*`.
3. Add focused tests in `tests/providers/unit_google_api.cpp` and extend existing
   provider transport, workspace, settings, and web client tests. Cover:
   - configuration round trips, native URLs, optional API-key headers, ignored
     settings, and switching a provider from OAuth or a hidden effort override;
   - projected history (including a leading model reply), request options, and
     the full shared maintenance tool schemas;
   - full JSON and arbitrarily split SSE, trailing usage/signatures, refusals,
     truncation, malformed responses, HTTP errors, and cancellation;
   - parallel and sequential tool calls, optional IDs, signature preservation,
     maintenance argument limits, visible round text, and the limit request
     with function-calling mode `NONE`; no round contains OpenAI-only fields;
   - native provider Test, naming and note operations with their default effort
     settings, missing thought-token counts, and unchanged existing protocols.
4. Run the affected C++ tests and `npm run check` in `webapp`. With a configured
   Google key, smoke-test streamed chat, a non-streaming request, a tool round,
   and the limit continuation. Include history beginning with a model reply and
   the actual maintenance declarations (nullable fields, empty parameter
   objects, and `additionalProperties`). Documentation defines these request
   shapes, but this design review has not tested them against a live model.
   Make schema adjustments only for a demonstrated incompatibility, within the
   Google encoder. Keep network tests manual so normal tests need no credentials.

Existing provider configurations require no migration. The implementation is
complete when a saved Google provider works through the current character and
auxiliary-operation paths, with the same cancellation and tool behavior as the
other protocols.
