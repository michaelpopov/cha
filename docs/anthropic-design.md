# Native Anthropic API support

Status: Technical design

Date: October 27, 2026

Review: [Claude CLI output before these revisions](anthropic-design-claude-review.txt)

## Scope

Add `api = "anthropic"` so CHA can call Claude models directly through the
Anthropic Messages API. Support chat, streaming and non-streaming responses,
CHA's existing tools, provider testing, session naming, and note text operations.
Users enter a model ID and select a saved API key, as they do for other providers.
Existing OpenRouter and other OpenAI-compatible configurations continue to work.

Use `POST /v1/messages`. The first implementation uses workspace-scoped API keys
and model defaults for thinking. It excludes Claude subscription login, cloud
platform authentication, organization-wide keys that require workspace selection,
model discovery, media uploads, Anthropic-hosted tools, and explicit prompt
caching. Jev keeps its separate service API.
[Anthropic API overview](https://platform.claude.com/docs/en/api/overview).

This document specifies implementation; it does not change application code.

## Reuse the current provider path

Follow the existing native Google adapter. Add `anthropic_api.h/.cpp` beside
`google_api.*`, with a request builder, streaming decoder, and complete-response
decoder. Add Anthropic branches to the existing protocol switches in
`ProviderClient` and the tool helpers.

Keep `Providers`, `ModelBackend`, `ApiKeyStore`, libcurl, `SseFramer`, generation
events, cancellation, timeouts, diagnostics, and the existing tool execution loop.
No SDK, provider registry, new transport class, background work, or persistent
state is needed. Decoder state lasts for one HTTP response. Tool continuation
uses the current request body and `GenerationResult::continuation`.

## Configuration and HTTP

Use the existing fields, with the user's model and saved key ID:

```toml
display_name = "Anthropic"
api = "anthropic"
mode = "net"
host = "api.anthropic.com"
port = 443
https = true
base_path = ""
model = "YOUR_CLAUDE_MODEL_ID"
api_key = "api_key_1"
stream = true
max_tokens = 64000
```

Append `/v1/messages` to the configured origin and base path, as the Responses
adapter appends `/v1/responses`. The provider form's Base URL is therefore
`https://api.anthropic.com`, without `/v1`. Preserve custom origins and local
HTTP test servers. Pass model IDs through without an allowlist or model catalog.

Resolve `api_key` through the existing native vault key store. Keep the legacy
`api_key_env` lookup by saved key name; do not read process environment variables.
A missing configured key uses the current credential error. With no key selected,
omit authentication and let the endpoint return its normal HTTP error.

Reuse `Authorization: Bearer <saved API key>`, which Anthropic now supports.
Add `anthropic-version: 2026-10-21` to every native Anthropic call. Keep the
current Content-Type and Accept headers. Use one version constant in code,
with no version selector or beta headers. Continue omitting credentials from
logs and redacting the active key from diagnostics.
[Authentication headers](https://platform.claude.com/docs/en/api/overview#authentication).

API version `2026-10-21` removes `temperature`, `top_p`, and `top_k`. Do not send
them. For Anthropic, ignore `temperature`, `reasoning_effort`, `reasoning_format`,
`cache_retention`, `openrouter_targets`, and provider-hosted `web_search`.
Ignore them before value validation, including obsolete invalid values. Retain
validation for fields that are used: endpoint, model, positive token limit,
timeouts, and credentials.
[API version history](https://platform.claude.com/docs/en/api/versioning).

Apply this rule in workspace loading, settings Test/Save parsing, character
overrides, and `provider_config_error()`, following the Google path. Workspace
loading and character resolution use their existing warning collector. The
settings parser currently normalizes Google's unused fields silently; for
Anthropic, use a plain `log_warn()` before normalization when supplied unused
values differ from defaults, including a non-null `temperature`. This follows
AGENTS.md's warning rule without a new diagnostics interface. Do not produce
warnings on every generation. Naming and note operations copy their
own effort settings; these have no effect for Anthropic and must not block the
operation. Extend the Google exception in `provider_supports_reasoning_effort()`
so the existing note-provider check accepts these unused settings. This does
not require a new capability system.

`auth = "none"` continues to mean no special OAuth flow; API keys still work.
`openai_subscription` remains valid only for the existing OpenAI configuration.
CHA's client-side `web_search` and `web_read` tools remain available independently
of provider-hosted search. Add Anthropic to the Google exclusion in
`provider_supports_web_search()`, including for custom OpenRouter origins, so
character overrides are ignored with the existing warning.

## Request encoding

Use `project_model_context()` so character instructions, shared history,
timestamps, notes, and compression keep their current behavior. Translate its
result into Anthropic fields:

| CHA input | Anthropic request |
| --- | --- |
| System instructions | Top-level `system` string |
| User text | `messages[]`, role `user`, text content blocks |
| Assistant text | `messages[]`, role `assistant`, text content blocks |
| Configured model | `model` |
| Streaming setting | `stream` |
| Output limit | Required `max_tokens` |

Combine adjacent messages with the same role into one message with ordered text
blocks. Use the projection's single system message directly. Omit empty text
blocks.
Preserve retained history, including a leading assistant reply after compression;
the current prompt remains the final user message. Never insert an OpenAI-style
system message into `messages`.

Always send `max_tokens`: use the configured positive value, or `64000` when unset.
This single encoder constant fits the documented 64K output ceilings of Claude
Opus/Sonnet/Haiku 4.5 and the 128K ceilings of current models. Thinking and answer
tokens share this budget. It leaves room for long note formatting/compression
and Assistant file writes, but cannot guarantee that every operation fits.
It is an upper bound, not a requested answer length. A truncated note must not replace
saved content; a truncated tool call must not run. Models with a lower ceiling
need an explicit lower `max_tokens` in TOML; larger tasks can use a higher value
within the model's limit. There is no per-model table or new form control.
[Messages request reference](https://platform.claude.com/docs/en/api/messages/create).
[Model output limits](https://platform.claude.com/docs/en/build-with-claude/thinking#output-limits).

Keep `stream = false` supported with the same limit. Anthropic documents the
SDK's large-limit streaming requirement as client-side validation, not an API
restriction; CHA uses libcurl. Long non-streaming requests still face existing
request and idle timeouts. Do not silently lower their limit or change transport.
[Long requests](https://platform.claude.com/docs/en/build-with-claude/thinking#long-requests).

Omit `thinking` and effort options. Anthropic's default thinking behavior varies
by model; some models think by default and may return no visible summary.
Do not promise a reasoning display or attempt to disable thinking by model name.
Preserve returned thinking blocks during a tool turn as described below.
[Thinking configuration](https://platform.claude.com/docs/en/build-with-claude/thinking).

Extend `update_tool_instructions()` to inspect Anthropic tool names and update
the top-level `system` string during preparation only. Include the instruction
for answering from collected results if further calls become unavailable in
that initial text. Preserve `include_tool_instructions` and request text-size
diagnostics. All requests, including auxiliary and continuation requests, must
contain only fields supported by the selected protocol.

## Tools and continuation

Reuse the existing schemas for search, page reading, and Assistant maintenance.
In `append_tool()`, translate the shared function definition to
`{name, description, input_schema}` by renaming `parameters` and omitting
OpenAI's `type`, `function` wrapper, and `strict` option. Do not duplicate the
tool catalog. Let Anthropic use its default automatic tool choice; never insert
the OpenAI string `tool_choice: "auto"` into an Anthropic request.
[Tool definitions and choice](https://platform.claude.com/docs/en/agents-and-tools/tool-use/define-tools).

Decode each complete `tool_use` block into the existing `ToolCall`: copy `id`
and `name`, and serialize the `input` object as `arguments`. Extend
`tool_call_result()` with this representation so call-ID validation, argument
limits, and call-count checks remain shared. Execute calls only after a complete,
valid response ending in `tool_use`. Never execute partial calls from a truncated,
refused, malformed, or interrupted response.

Store the complete ordered assistant content array in an assistant message in
`GenerationResult::continuation`. Append that message unchanged, then one user
message containing one `tool_result` block per call:

```json
{
  "role": "user",
  "content": [
    {"type": "tool_result", "tool_use_id": "toolu_example", "content": "TOOL_OUTPUT"}
  ]
}
```

Use each executor's existing output string directly. Keep result order aligned
with call order, including parallel web calls. Return a result for every call,
including the existing error outputs for rejected or over-limit calls. Keep
current execution rules, progress events, cancellation, and maintenance limits.
Tool results immediately follow the assistant message; no other message goes
between them.
[Tool result format](https://platform.claude.com/docs/en/agents-and-tools/tool-use/handle-tool-calls).

Retain all `thinking` and `redacted_thinking` blocks, signatures, and text from
the assistant response in their original order. Do not reconstruct this message
from displayed text. These blocks are required for thinking with tools. Keep
them across all rounds of the current request, then discard the request as
usual. Later user turns use CHA's existing text transcript; no second history
store is needed.
[Thinking with tools](https://platform.claude.com/docs/en/build-with-claude/thinking#thinking-with-tool-use).

At the existing tool limit, retain declarations and set
`tool_choice = {"type": "none"}`. Keep `system`, `tools`, and all earlier messages
unchanged; append only the assistant response and its user tool-result message.
Skip the shared loop's `update_tool_instructions()` and final-answer directive
mutation for Anthropic. The initial instruction and `tool_choice` handle the
limit without a new message type or model-specific branch. Editing the prefix
can invalidate thinking signatures; `tool_choice` is outside that prefix.
Reject further calls through the existing limit error. Text streams immediately
with the current blank line between rounds that contain answer text.
[Preserved-thinking rules](https://platform.claude.com/docs/en/build-with-claude/preserved-thinking#keeping-the-prefix-unchanged).

## Responses, streaming, and errors

Use `SseFramer` and dispatch on the JSON `type`; SSE data already contains the
event type, so the framer needs no new interface. Handle:

- `message_start`: initialize the message and reported usage.
- `content_block_start`, `content_block_delta`, `content_block_stop`: accumulate
  ordered content by block index. Emit text as answer deltas and any visible
  thinking summary as reasoning deltas. Retain signature deltas and opaque
  redacted blocks without displaying them.
- `input_json_delta`: accumulate partial JSON for that tool block, then parse
  its final input as an object at block completion. Record parse failure until
  the terminal reason is known, so truncation gets the correct error below.
  A no-argument call can have an empty object with no JSON deltas. Apply the
  existing argument byte limits while buffering; stop retaining excess bytes
  and return `Tool arguments too large`, rather than a later JSON parse error.
- `message_delta`: update the terminal reason and latest usage values.
- `message_stop`: finalize; successful results require complete supported blocks.
- `ping`: ignore. `error`: fail with the provider error. Ignore unknown event
  types for forward compatibility, without treating them as completion.

Require `message_stop` and a terminal reason for successful streaming completion.
There is no `[DONE]` marker. If a terminal length limit cuts a tool block, return
`maintenance_output_limit_message` for maintenance or `Tool response ended
incomplete` for web tools, even when the tool JSON cannot be parsed or the block
was not closed. Reuse those existing errors without fabricating valid tool input
or executing any call. Keep the current maintenance length-error precedence over
the argument-size error. Premature EOF without a terminal reason, malformed
known events, invalid block indexes, or invalid tool JSON in an otherwise
complete response remain protocol failures. Preserve text already delivered.
Apply the same terminal-reason precedence to full JSON responses, and share
completed-message parsing and result normalization between both paths.
[Streaming protocol](https://platform.claude.com/docs/en/build-with-claude/streaming).
[Incomplete tool calls](https://platform.claude.com/docs/en/build-with-claude/handling-stop-reasons#incomplete-tool-use-blocks).

Normalize terminal reasons for current consumers:

| Anthropic `stop_reason` | CHA result |
| --- | --- |
| `end_turn`, `stop_sequence` | `finish_reason = "stop"` |
| `tool_use` | Completed tool round, with validated calls |
| `max_tokens`, `model_context_window_exceeded` | `finish_reason = "length"` |
| `refusal` | `refused = true`, `finish_reason = "content_filter"` |
| `pause_turn` or unknown value | Protocol error with the reason |

Keep partial chat text on truncation. Note operations must reject truncated or
refused results through their existing checks. Empty responses without answer
text or valid calls use the current no-answer error. Do not automatically resume
paused turns, retry rate limits, or add a fallback policy. Use `error.message`
from HTTP and SSE errors with the current diagnostic limits and key redaction.
[Stop reasons](https://platform.claude.com/docs/en/build-with-claude/handling-stop-reasons).

For usage, retain the latest reported value of each field across stream events;
counts are cumulative, not per-chunk increments. Map output to `output_tokens`
without adding thinking tokens again. Map cache reads/writes to
`cache_read_input_tokens` and `cache_creation_input_tokens`. CHA input is the
sum of uncached input, cache reads, and cache writes, so its total includes the
full prompt. Missing cache counts contribute zero to that sum when input is
reported; missing input/output counts remain unknown. Let the existing loop sum
usage across HTTP calls.
[Usage fields](https://platform.claude.com/docs/en/api/typescript/messages),
[Input token totals](https://platform.claude.com/docs/en/api/rate-limits).

## Provider form

Add **Anthropic** to the existing **API format** selector. Reuse the existing
Model, Base URL, Credentials, Test, and Save controls. Keep endpoint and model
edits explicit; choosing a format does not overwrite them. Hide Reasoning effort
and OpenRouter inference targets, following Google. Offer saved keys and no
OpenAI OAuth option. Switching from OAuth clears that selection to no credentials;
switching from a saved key preserves the selection. Test uses the native API.

Proposed form fragment, with the current layout and styling:

```text
Model
[ YOUR_CLAUDE_MODEL_ID                                  ]

Base URL
[ https://api.anthropic.com                             ]

API format                    Credentials
[ Anthropic             v ]   [ My Anthropic API key  v ]

[ Test ]  [ Cancel ]  [ Save changes ]
```

Normalize unused provider fields in `candidateUpdate()` as the Google form does,
including `temperature = null` and `reasoning_effort = "none"`. Existing character,
naming, and note effort controls stay unchanged and have no effect for Anthropic.
Default thinking may exceed the current naming/Test timeout; keep the existing
failure and naming fallback behavior. Add no panel, legend, helper copy, or
setup flow. This sketch records the proposed visible change for design review.

## Implementation and validation

1. Add `ProviderApi::anthropic`, string parsing, endpoint construction, and the
   unused-setting rules in `src/characters/character_config.*`,
   `src/workspace/workspace.cpp`, `workspace_config_editor.cpp`, and
   `src/runtime/request_parser.cpp`. Preserve settings round trips and defaults
   for other formats.
2. Add `src/providers/anthropic_api.*` and register it in `CMakeLists.txt`.
   Integrate request preparation, version header, response decoders, tool
   definitions, and continuation handling in `provider_client.*` and
   `tool_calls.*`. Keep changes to the shared execution loop limited to protocol
   encoding and decoding.
3. Add `anthropic` to both provider API enums in `resources/dto.yaml`, regenerate
   `webapp/src/api/schema.d.ts` with `npm run api-types`, and update the client
   guard and `webapp/src/components/ProviderSettings.tsx`.
4. Update the embedded Assistant reference during implementation:
   `docs/MaintainerGuide.md` must describe the native API value, endpoint, key
   scope, required output limit, and unused settings. Clarify in
   `resources/assistant-maintenance.md` that Test adds no separate cap; Anthropic
   still requires the configured or default `max_tokens`. Keep the restriction
   against adding a cap as a repair shortcut. These are narrow integration
   updates in the implementation plan; this review edits only this design.
5. Add focused adapter tests in `tests/providers/unit_anthropic_api.cpp`, register
   that file in `CHA_TEST_SOURCES` in `CMakeLists.txt`, and extend existing
   transport, workspace, settings, and web tests. Cover default
   and explicit token limits, auth/version headers, ignored invalid settings,
   history projection, complete JSON and split SSE, cumulative usage, refusals,
   truncation, stream errors, cancellation, tool IDs and fragmented inputs,
   signature preservation, full maintenance schemas, parallel calls, and the
   final request with tools disabled and an unchanged system/tool/message prefix.
   Test an incomplete maintenance call at both length-limit reasons: it returns
   `maintenance_output_limit_message` and no tool runs. Also cover oversized
   fragmented arguments, a missing terminal event, the custom-OpenRouter search
   override, and ignored malformed fields in Test/Save. Confirm no OpenAI-only
   fields leak into any Anthropic round and existing formats still pass.
6. Run affected C++ tests and `npm run check` in `webapp`. With a saved Anthropic
   key, manually test streaming chat, non-streaming output, a multi-call tool
   round, the limit continuation, provider Test, naming, and note operations.
   Include compressed history that starts with an assistant reply, a long page
   format, large-note compression, and an Assistant write near the existing
   64 KiB file limit. Verify long non-streaming output and a deliberately cut
   tool call; capture its terminal event sequence. Keep live API checks manual
   so normal tests need no credentials.

For each model actually tested, record Test, naming, and long-thinking chat
outcomes, including observed idle gaps and any consistent timeout failure.
The documentation says ping events may occur, but gives no maximum interval;
do not assume they prevent idle timeouts. Keep existing timeouts and naming
fallback, and record demonstrated limits before calling implementation complete.
These checks have not run during this design review. Leading-assistant history,
the truncated-tool event sequence, and the complete tool-limit continuation
remain live checks; documentation alone does not establish their model-specific
behavior. No new timer, retry, model catalog, or timeout policy is added.

## Review decisions

The factual integration findings are incorporated above, including the output
limit and error handling. The unnecessary system-fragment join rule is removed.
The suggestion to normalize settings silently is not adopted: AGENTS.md requires
warnings for unused settings, which a direct log message satisfies. External
questions are either supported by the linked API references or included in the
manual validation above. The original CLI output is retained unchanged.

No existing configuration needs migration. Completion means a saved native
Anthropic provider works through current character and auxiliary-operation paths,
with the existing tool and cancellation behavior. The protocol details above
were checked against Anthropic documentation on the design date; live model
validation remains part of implementation.
