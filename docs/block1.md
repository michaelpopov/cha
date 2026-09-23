Implement Session 1 of adding selectable xAI Grok Voice Transcribe support to CHA. This session is about architecture, configuration, and preserving the existing OpenAI path. Do not implement the full xAI streaming transport yet.

Follow the Keep it simple rule in AGENTS.md. If a simpler design meets a requirement, use it.

## Session sequence and handoff

Work in this order on the same working branch: Session 1, [Session 2a](block2a.md), [Session 2b](block2b.md), [Session 3](block3.md). [Block 2](block2.md) is the shared contract for 2a and 2b, not one large implementation session. Read those repository files and AGENTS.md; no previous chat summary is required. Preserve the curl prerequisite changes already prepared in the working tree. Do not start from a clean worktree that omits them.

Commit each completed session before starting the next. Its commit body must record the concrete interface/file locations, relevant tests and results, and known limitations. Session 1 must include these plan files and the checked-in xAI fixtures if they are not yet tracked. Do not include unrelated user changes. Later sessions read the preceding commit messages as well as the repository instructions; do not leave the only handoff in chat.

## Decisions

The provider defaults, URL rules, and interface below are fixed. Implement them; do not research alternatives or introduce another transport. Session 2 contains the xAI wire contract. Implementation tests are required, but protocol/design decisions are not assigned to this session.

## Goal

Prepare CHA so voice input can support multiple transcription providers:

- existing OpenAI Realtime transcription
- future xAI / Grok Voice Transcribe 2.0 streaming transcription

The user must eventually be able to choose either provider in Voice Settings.

## Constraints

Preserve OpenAI's WebRTC transport and transcript behavior. The no-reload and fresh-start-settings changes below deliberately update settings/startup behavior for both providers. Avoid broad refactoring outside voice input. Do not implement the actual xAI WebSocket/audio transport in this session; leave the explicit stub described below.

## Required work

1. Inspect the current voice-input path, especially:
   - webapp/src/voiceInput.ts
   - webapp/src/voiceInput.test.ts
   - Voice Settings in webapp/src/components/Settings.tsx
   - voice-input settings DTO/schema
   - workspace/config storage for voice input
   - native/application voice-input entry points in src/app/media_operations.cpp
   - bridge/API types around connect_voice_input
2. Add the field named provider with exactly two values: "openai" and "xai". Missing provider means "openai" in stored configuration and legacy save requests; an explicitly invalid value is rejected. Emit provider explicitly in all settings/runtime responses and newly saved configuration.
3. Persist this field through the complete existing settings path:
   - workspace/config representation
   - src/runtime/protocol.h settings/runtime structs and their JSON serialization
   - DTO/schema
   - resources/dto.yaml, webapp/src/api/schema.d.ts, client guards, and native client types
   - src/runtime/request_parser.cpp: replace the existing exact-five-field check with an explicit allowed-field check accepting the original five fields plus optional provider
   - Settings UI
   - tests
4. Existing configurations without this field must continue to work and must default to OpenAI, so this change is backward compatible.
5. Update Voice Settings so the user can choose:
   - OpenAI
   - xAI

When the provider changes, replace URL and model with the selected provider defaults and clear the API-key selection. Preserve delay and prompt in the form so switching back restores them. Do not add per-provider profile storage. Hide delay and prompt while xAI is selected; show them for OpenAI.
Allow voice-input settings to be saved independently of voice-output settings. The current form requires complete output settings and saves both together; split the existing form into input and output forms, each with its own Save/Reset buttons, dirty state, validation, and submission state, so a user with only an xAI API key can configure transcription without an output provider or voice. Saving input alone must not validate or write unrelated output settings. Preserve existing voice-output functionality.
Use these defaults:

```text
OpenAI:
  provider: openai
  url: https://api.openai.com/v1/realtime/calls
  model: gpt-live-transcribe

xAI:
  provider: xai
  url: wss://api.x.ai/v1/stt
  model: grok-voice-transcribe-2.0
```

The xAI endpoint and model were checked against the official xAI speech-to-text documentation on 2026-09-23; use them as written. Retain the existing OpenAI model default unchanged.
Make URL validation provider-specific in both configuration loading and saving. The current valid_voice_input_url() in src/workspace/workspace.cpp accepts only HTTP/HTTPS and is used by load_voice_input() and WorkspaceConfigEditor::write_voice_input(). Do not simply allow all four schemes for every provider:

- OpenAI accepts absolute https:// URLs and http:// URLs for local test servers; reject ws:// and wss://.
- xAI accepts absolute wss:// URLs and ws:// URLs for local fake servers; reject http:// and https://.

Resolve the provider first, defaulting a missing provider to OpenAI, then apply the appropriate URL check. Preserve the existing authority validation and keep any UI/client URL validation consistent. Replace the load error "requires an absolute HTTP or HTTPS URL" with an error that names the selected provider and its accepted schemes; save validation must report "OpenAI voice input requires an absolute HTTP or HTTPS URL" or "xAI voice input requires an absolute WS or WSS URL" as applicable. Update the generic-error catch in save_voice_input_settings() so it does not hide this known validation error. Keep the existing authority validation; this change selects schemes by provider and does not introduce a new host allowlist.
Do not reload the page after voice settings saves or because a vault merge changes a voice endpoint. This decision was checked against both native hosts: CSP is fixed at connect-src 'self', the OpenAI SDP POST is native, and xAI connects natively. The Settings.tsx comment about a direct browser transcription connection is obsolete. Remove voiceInputOrigin(), its before/after merge reads, the voice-origin reload state/prompt, and the save-time origin comparison. Keep the existing successful-merge bootstrap refresh and unrelated recovery/navigation behavior. Remove the now-unused state/voiceSettingsReload.ts helper, consumeVoiceSettingsRestore() import/branches in useLiveSession.ts, and obsolete reload-specific tests. Saving stays on the current Settings screen; returning to the composer remounts it and loads current availability.

6. Keep VoiceInputSession as the provider-neutral entry point in webapp/src/voiceInput.ts. Move the existing implementation, unchanged, into OpenAiVoiceInputSession in webapp/src/openAiVoiceInput.ts. Define a shared session interface with stop(): Promise<void> and cancel(): void. VoiceInputSession.start() dispatches by configuration.provider and returns that interface. Keep onTranscription(delta) and onFailure(error) callback signatures. Preserve OpenAI's nativeConnect dependency and supply xAI bridge access as a separate dependency; ChatScreen must not parse provider events.

Copy provider from getVoiceInputRuntime() into ChatScreen's VoiceInputConfiguration (that mapping currently copies only model, delay, and prompt), and pass it to supported(provider). Retain the mount-time read only for microphone-button availability, and refresh it on vault/context changes. On every new dictation attempt, enter starting state and create the existing attempt token/AbortController, then await a fresh getVoiceInputRuntime() before provider dispatch or microphone capture. Check cancellation and the attempt token immediately after that await. Use the returned provider/model/delay/prompt for the attempt and the composer's current ['ru'] or ['en'] language. Null, failed, or unsupported fresh configuration ends startup with an error; never fall back to the cached provider. Prevent late reads from starting capture after cancel or navigation. Each native start must also reject a provider mismatch with its current settings, rather than using the wrong transport if settings change during startup.

VoiceInputSession.supported(provider) checks getUserMedia for both providers, RTCPeerConnection for OpenAI, and AudioContext/AudioWorkletNode for xAI. Do not require WebRTC support to enable xAI. Keep the xAI branch as an explicit unavailable-transport stub until Session 2b. Do not add a settings subscription service or a second configuration cache.

7. Retain delay and prompt in the shared settings for backward compatibility. They apply only to OpenAI. For xAI, do not send either field to the provider, do not reinterpret prompt as keyterms, and do not reject otherwise valid configuration because an unused delay is obsolete. Normalize an invalid unused xAI delay to "low" and log a warning without its value; retain the existing OpenAI delay validation. Do not add xAI-specific controls in this release. Session 2 fixes endpointing at 400 ms and disables interim results.
8. The runtime must reject or clearly report unsupported xAI execution for now rather than accidentally trying to use the OpenAI WebRTC path with an xAI URL.

Until Session 2b replaces the browser stub, selecting xAI fails with "xAI voice input transport is not implemented". Also reject xAI in the native OpenAI connect operation before any HTTP request.
Do not silently route xAI through OpenAI code.

## Important design objective

At the end of this session, the code should make this distinction clear:

```text
configuration/provider selection
            ↓
provider-specific transport
            ↓
common transcription callback/interface
```

The editor/composer must not interpret provider events or word timestamps. Provider adapters prepare the text; the first-addition formatting choice described below does not change that separation.
Define the callback contract for Session 2b: onTranscription(delta) emits text ready to append, including required separators. Preserve OpenAI's existing first-delta appendTranscription() and later raw concatenation. Session 2b adds the xAI adapter's per-piece dictation-command normalization and a newline-preserving first-addition helper, as specified in block 2; do not implement that xAI behavior in this session.

## Tests

Add/update tests for at least:

- old voice-input config without provider loads as OpenAI
- saving/loading provider openai
- saving/loading provider xai
- both load and save accept the default OpenAI HTTPS and xAI WSS endpoints, plus their HTTP/WS local test endpoints
- both load and save reject cross-provider schemes, unsupported schemes, and missing/malformed authorities
- missing provider still uses OpenAI URL validation; an invalid endpoint reports the provider's accepted schemes
- DTO/schema/client validation includes provider
- Voice Settings can switch between OpenAI and xAI
- provider switch uses the exact defaults, clears API-key selection, and retains hidden OpenAI delay/prompt values
- ChatScreen retains provider from the runtime DTO and uses provider-specific availability checks
- xAI ignores an obsolete unused delay with a warning while OpenAI still rejects an invalid delay
- input/provider/origin saves and voice-origin changes after a vault merge do not reload or offer an obsolete reload prompt
- returning from Settings refreshes microphone availability; a new dictation attempt re-reads provider and configuration rather than using the mount-time snapshot
- cancellation, navigation, or a vault/context change while that read is pending prevents late startup
- a provider mismatch at native startup fails without contacting the wrong provider
- an xAI key and valid input settings can be saved/reloaded with no voice-output configuration or voices
- saving input alone neither validates nor writes voice-output settings
- OpenAI current path still behaves exactly as before
- selecting xAI does not accidentally invoke the OpenAI WebRTC implementation
- invalid provider values are rejected

Run the relevant test suites and normal validation, including:

```sh
cd webapp
npm run api-types
npm run check
```

and the relevant C++ tests/build for changed workspace/settings/application code.

## Deliverable

Implement and test Session 1 completely.
Commit it with subject `voice input: prepare provider selection (session 1)` and a body containing the handoff below. Repeat the concise summary in chat, but the commit is the durable handoff:

- files changed
- resulting provider/configuration model
- how backward compatibility works
- exact interface/extension point Sessions 2a and 2b should use to implement xAI streaming
- validation failures, if any; do not leave provider or transport decisions to later sessions

Do not proceed into the full xAI WebSocket transport implementation in this session.
