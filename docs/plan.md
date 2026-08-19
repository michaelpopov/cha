# Plan: Provider-Side Context Caching

This is the first-priority implementation milestone from `docs/design.md`. It
compacts Blocks 1–7 of `docs/implementation-plan.md` into four ordered slices.
The goal is to make CHA's stable prompt prefix cacheable, send a stable cache
identity where the provider supports it, and measure real cache usage.

Do not begin the reliability and diagnostics work from later blocks until this
milestone is complete and a live-provider check has shown whether caching is
effective. Keep this implementation small: no cache dashboard, cost estimator,
local prompt cache, or conversation-object state.

## Slice 1 — Stabilize the prompt prefix

Files:

- `src/agents/model_context.cpp`
- `tests/agents/unit_model_context.cpp`

Remove the current-submission-time sentence from the system message in
`project_model_context()`. Do not add a replacement: the final user/persona
message already receives `run.created_at` through `prefixed_human_message()`.
Historical message timestamps remain unchanged.

Tests must establish that:

- two otherwise identical requests with different `run.created_at` values have
  byte-identical system messages;
- each final user/persona message still contains its own submission timestamp;
- historical messages and their timestamps are unchanged.

Acceptance: changing only the current submission time changes the request tail,
not the system prefix.

## Slice 2 — Parse and log cache usage

Files:

- `src/agents/model_backend.h`
- `src/agents/provider_response.cpp`
- `src/agents/responses_api.cpp`
- `src/agents/provider_client.cpp`
- `tests/agents/unit_provider_response.cpp`
- `tests/agents/unit_responses_api.cpp`
- `tests/agents/unit_provider_client.cpp`

Add this optional field to `GenerationTokenUsage`:

```cpp
std::optional<std::size_t> cache_read_tokens;
```

Populate it from the provider's usage object:

- Responses API: `usage.input_tokens_details.cached_tokens`;
- Chat Completions: `usage.prompt_tokens_details.cached_tokens`;
- Chat compatibility fallback: `usage.prompt_cache_hit_tokens`.

Missing or malformed optional cache details must leave the field unset without
changing existing input/output token parsing. Keep provider-reported
`input_tokens` unchanged: cached-read tokens are normally already included in
that total.

Add `cache_read_tokens=<value-or-unreported>` to the existing sanitized HTTP
completion log. Do not derive cost or a cache percentage.

Tests must cover each supported JSON path, missing cache details, precedence of
the primary Chat field over the fallback, and the resulting HTTP log field.

Acceptance: a provider-reported cache count reaches `GenerationResult::usage`
and the normal completion log without changing the public generation outcome.

## Slice 3 — Build and send cache identity

Files likely involved:

- `src/session/session_controller.h/.cpp`
- `src/workspace/session_open.cpp`
- `src/agents/model_context.h`
- `src/agents/model_backend.h`
- `src/agents/character_config.h/.cpp`
- `src/agents/provider_client.h/.cpp`
- `src/agents/responses_api.cpp`
- corresponding controller, session-open, config, request-builder, and
  provider-client tests
- build configuration if SHA-256 needs an explicit crypto dependency

### Session key

Pass the production `SessionIdentity` from `open_session()` into
`SessionController` and retain it for request construction. Test-only
constructors may use an empty identity, which disables explicit cache identity.

When `SessionController::start_batch()` creates each `GenerationRequest`, set a
cache key on its `RunSpec` using:

```text
<forum-id>/<session-id>/<target-character-id>
```

This intentionally produces a different key for each multicast target. If the
plain key is at most 64 characters, use it unchanged. Otherwise use the
64-character lowercase hexadecimal SHA-256 digest of the plain key, as required
by `docs/design.md`. Do not use `std::hash`. If OpenSSL is used, link the crypto
dependency explicitly rather than relying on libcurl's transitive linkage.

### Configuration

Add one provider setting:

```toml
cache_retention = "short" # off | short | long
```

Represent it with a small enum in both `ProviderConfig` and
`ModelBackendConfig`, add it to `provider_setting_names`, propagate it through
`make_backend_config()`, and validate it while loading provider TOML. The
default is `short`; invalid spellings fail with an error naming
`cache_retention`.

Semantics:

- `off`: send no cache key, affinity header, or retention field;
- `short`: send the supported key/header and use provider-default retention;
- `long`: behave as `short`, and additionally send
  `prompt_cache_retention = "24h"` for direct OpenAI Responses requests.

### Provider wire behavior

Use an exact normalized host match for `api.openai.com`; do not infer support
from `base_path` or a host substring.

| Provider path | JSON body | HTTP header |
| --- | --- | --- |
| Direct OpenAI Responses | `prompt_cache_key`; plus `prompt_cache_retention` for `long` | `session_id: <key>` |
| Direct OpenAI Chat Completions | `prompt_cache_key` | none |
| Any other host | none | none |

The Chat request builder is in `provider_client.cpp`; the Responses builder is
in `responses_api.cpp`.

`ProviderClient::prepare()` must carry any per-request `session_id` safely into
`perform()`. Extend `RequestPayload` with narrow optional metadata for this
purpose. Do not store the current request's cache key in mutable
`ProviderClient` state: a backend may be exercised concurrently or cancelled
between preparation and transport.

Tests must cover:

- stable short and long-key construction;
- identity flow from session open through the generated request;
- one key per target character;
- all three retention modes;
- both direct OpenAI APIs;
- exact-host rejection of lookalike and gateway hosts;
- body and header behavior from the matrix above;
- `store` remains false and neither `previous_response_id` nor `conversation`
  is introduced.

Acceptance: repeated turns for the same forum/session/character carry the same
key, while unsupported hosts and `cache_retention = "off"` carry no cache
metadata.

## Slice 4 — Verify the milestone

Keep automated coverage in the existing test files unless a new fixture is
clearly simpler. CMake lists test sources explicitly, so any new test file must
also be added to `CMakeLists.txt`.

The mock transport test verifies plumbing, not provider-side cache
effectiveness. It should make two requests whose current timestamps differ and
verify:

- the system instructions are byte-identical;
- both requests carry the same cache key;
- the expected body/header matrix is respected;
- a mocked cached-token value is parsed and logged.

Then run:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

Finally, perform a manual check against direct OpenAI using a conversation with
a sufficiently long stable prefix:

1. submit multiple turns to the same character in the same session;
2. confirm the same `prompt_cache_key` is sent each time;
3. confirm a later completion logs non-zero `cache_read_tokens`;
4. compare with the provider's usage or billing view when available.

A zero cache count on a short, cold, or recently evicted prompt is not by itself
an implementation failure. If sufficiently long repeated prefixes still never
produce a hit, investigate the actual serialized requests before starting the
lower-priority timeout, retry, error-classification, and context-warning work.

## Completion checkpoint

This milestone is complete when all automated tests pass and the live check has
either demonstrated a cache hit or produced enough request evidence to explain
why the provider did not cache the prefix. Record that result before deciding
whether to proceed with Phase 2 from `docs/implementation-plan.md`.
