Implement **Session 5** of selectable xAI Grok Voice Transcribe support in CHA.

Follow the Keep it simple rule in AGENTS.md. If a simpler design meets a requirement, use it.

Read [block1.md](block1.md), the shared contract in [block2.md](block2.md), and the commit bodies from [Session 3](block3.md) and [Session 4](block4.md) before starting. These are the durable handoff; previous chats are not required. Work on the branch containing those completed commits.

The protocol reference is the [official xAI speech-to-text documentation](https://docs.x.ai/developers/model-capabilities/audio/speech-to-text). Its relevant behavior was already checked against the real recordings linked below; do not assign another protocol investigation to this session.

Sessions 1, 3, and 4 should already have:
- introduced `openai | xai` voice-input provider selection,
- preserved the OpenAI WebRTC path,
- implemented xAI streaming transport with append-only finalized text,
- normalized xAI transcription into the common CHA voice-input callback,
- added the required bridge/native plumbing.

This session is for **integration hardening, regression testing, and cleanup**. The configuration, transport, buffering, bridge, and transcript decisions are fixed in blocks 1 and 2. The [recorded xAI fixtures](fixtures/xai/README.md) resolve the provider semantics. Test the implementation against that contract; do not research alternatives or select a different normalization algorithm. Fix concrete implementation defects without redesigning the feature.

## Primary goals

1. Verify both OpenAI and xAI voice input work end-to-end.
2. Make provider switching reliable and understandable in Settings.
3. Harden cancellation, failure, shutdown, and stale-event behavior.
4. Validate packaging/build behavior on supported native platforms.
5. Remove temporary scaffolding from earlier sessions, TODOs, or duplicate abstractions.
6. Keep scope limited to voice input.

## First: inspect current implementation

Before changing code:

- read the committed interface/file-location and test summaries from Sessions 1, 3, and 4,
- trace the complete OpenAI path,
- trace the complete xAI path,
- identify tests already present,
- inspect any TODOs, temporary stubs, compatibility shims, or duplicated logic.

Do not assume the implementation matches the earlier plan; treat the current branch as authoritative.

## End-to-end behavior to verify

### OpenAI

Existing behavior must remain intact:

```text
microphone
  → WebRTC
  → OpenAI Realtime transcription
  → delta events
  → append into prompt
```

Check:
- start
- continuous dictation
- stop
- send while dictation is stopping
- cancel
- provider/API failures
- switching sessions while dictating
- vault/application shutdown
- Russian/English language behavior already supported by CHA

### xAI

Verify:

```text
microphone
  → browser audio capture
  → native bridge
  → native WebSocket
  → xAI STT
  → transcript normalization
  → prompt
```

Check:
- startup latency
- continuous streaming
- unexpected interim events are ignored; final words follow block 2's single-cursor rule
- long dictation
- silence
- punctuation
- Russian and English dictation: the composer sends ['ru'] or ['en'], native startup sets the matching language query parameter, and the Russian fixture's text/timings survive
- dictation commands in later finalized pieces, including English/Russian punctuation and newline commands
- rapid stop/start
- cancellation during connection establishment
- cancellation while streaming
- final transcript arriving after stop
- stop flushes the last partial capture frame and sends all queued audio before audio.done
- stop resolves only after final transcription reaches the composer and resources are released
- send while dictation is stopping includes the final words
- finalization timeout or cancellation during stop leaves no pending promise or finishing state
- unexpected WebSocket close
- authentication failure
- malformed provider response
- stale events from a previous dictation

## UX requirement: append-only prompt behavior

This is important.

The prompt editor must not behave like the previous ElevenLabs experiment where the whole text was repeatedly replaced.

For xAI:

- committed prompt text must never be rewritten,
- finalized transcript pieces should append exactly once,
- overlapping locked chunks, utterance finals, and any words in transcript.done must not append the same audio coverage twice,
- matching text from distinct utterances, or intentional repetition within one utterance, must be preserved,
- interim_results is false; ignore mutable events without adding a provisional presentation layer.

Replay every JSONL file in [docs/fixtures/xai](fixtures/xai/README.md) in its recorded event order and assert the per-event deltas and full text in [expected.json](fixtures/xai/expected.json). These are captured provider events, not illustrative guesses. Do not regenerate or adjust their expected outputs to accommodate a different algorithm.

The fixtures settle the important cases:

- chunk-only additions with a broader top-level start/duration window, followed by the stitched utterance final;
- session-wide word times across separate utterances;
- repeated phrases within and across utterances, including "very very useful";
- empty chunk finals during silence;
- words delivered after audio.done in final partials, followed by an empty transcript.done;
- silence-only completion;
- Russian punctuation and word text;
- mutable interim events with empty word arrays, which must be ignored.

Add focused synthetic error/edge cases for malformed times, a nonempty final without words, repeated delivery of the same final, equal end times among new words in one event, and a transcript.done containing timed words not previously delivered. Clearly label these as synthetic cases. For a nonempty final without timings, assert a terminal error and resource cleanup; there is no text-prefix fallback. Do not assume transcript.done must contain the full dictation.

Check finalized-text latency against the documented behavior, not a made-up target: the real long-utterance fixture has no locked text until roughly 26.9 seconds. Audio must keep flowing and the UI must stay responsive throughout. Enabling interim results did not improve locked-result latency in the comparison capture. Keep interim_results=false and do not add periodic finalize messages or mutable editor replacements.

Exercise these sequences through the actual composer integration. Session 4 applies the shared dictation-command helper to each xAI piece, preserves line breaks, and recomputes separators from the emitted text. Check later "new line"/"новая строка" and "comma"/"точка", newline-only first and later additions, a word after a newline, and an existing draft. Check multiple pieces in one audio/stop reply. A command split between pieces is deliberately not recognized; do not add cross-piece buffering. Use appendPreparedTranscription() for the first xAI addition so its line breaks are not trimmed. Keep OpenAI's existing first-delta appendTranscription() and later concatenation unchanged.

## Settings UX

Review the Voice Input settings screen.

The user should be able to switch cleanly between:

- OpenAI
- xAI

Provider switching should:
- set the exact endpoint/model defaults from Session 1 and clear the API-key selection when the provider changes,
- retain delay/prompt values for switching back to OpenAI but hide them for xAI,
- not expose irrelevant OpenAI-only settings for xAI,
- not expose xAI-only settings for OpenAI,
- clearly identify which API key is selected,
- produce clear validation errors.

Verify that URL validation agrees between load, save, and the UI: OpenAI accepts HTTPS (HTTP for local test servers), while xAI accepts WSS (WS for local fake servers). Reject the other provider's schemes and malformed authorities. A missing provider must still use the OpenAI rules, and errors must name the appropriate accepted schemes.

Avoid making users manually understand transport-level concepts such as SDP or WebSocket framing.

Apply Session 1's no-reload rule: input/output saves, provider/origin changes, and merged voice settings stay in the current page without an obsolete reload prompt. Both native hosts have a fixed CSP and native provider networking; remove the old browser-connection rationale rather than testing for a changing CSP. Returning to the composer refreshes availability, and every dictation start reads fresh runtime settings before selecting the transport. Test an old cached OpenAI configuration followed by fresh xAI configuration, and the reverse; also test cancellation/navigation while that fresh read is pending. A failed read must not fall back to the cached provider.

Verify independent input setup introduced in Session 1: a user with an xAI key and no voice-output configuration or voices must be able to save and use transcription. Saving input alone must not require or write output settings. Keep any related Settings changes limited to this independent save behavior.

## Backward compatibility

Verify old workspaces/configurations that predate the provider field still load as OpenAI.

Test:
- missing provider field,
- existing OpenAI URL/model,
- saved/reloaded configuration,
- preserve provider in the existing workspace/vault configuration round trip; do not introduce a separate migration.

Do not introduce a configuration migration; missing provider defaults to OpenAI.

## Security review

Verify:

- xAI and OpenAI API keys are not exposed to browser JavaScript where they should remain native/server-side,
- API keys are not logged,
- audio contents are not logged,
- transcript contents are not added to diagnostic logs unless existing policy explicitly allows it,
- error messages returned to the UI do not leak credentials or raw provider responses unnecessarily.

## Resource ownership

Review for leaks and lifetime bugs.

Ensure all paths release:
- microphone tracks,
- AudioContext / AudioWorklet resources,
- pending bridge requests and session identity,
- WebSocket handles,
- background jobs,
- pending request state.

Test:
- repeated start/stop cycles,
- cancel during startup,
- application shutdown during xAI streaming,
- vault switch during streaming,
- provider switch while idle,
- provider switch after a failed session.

There must be no permanent worker/thread left after voice input completes.

## Backpressure and buffering

Inspect the xAI audio path for unbounded queues.

Requirements:
- use block 2's 19 completed-batch credits (including the one native in-flight batch) plus one reserved partial/final-flush batch,
- if native/transport cannot keep up, behavior must be explicit,
- do not silently accumulate arbitrary amounts of audio in JavaScript or C++ memory,
- preserve audio frame order,
- keep at most one audio request outstanding and use the existing reply/delivery-acknowledgment flow before submitting the next,
- keep complete JSON requests below 65,536 bytes,
- leave capacity for other UI operations within the shared limit of 16 ordinary requests; submitting another request when those slots are full invalidates the whole bridge connection,
- finish or fail stop within its documented budget below the effective bridge deadline (30 seconds by default), including cleanup and reply-delivery margin.

Follow block 2's overflow policy: fail dictation with a concise error and release resources when the bounded capture, JavaScript, or native queue fills. Do not silently drop audio, keep sending after failure, raise global bridge limits, or exempt voice requests from deadlines. Test delayed replies and stalled WebSocket sends with concurrent UI activity; audio backpressure must not flood or invalidate the shared connection. Test a shorter bridge deadline as well as the default.

## One manual responsiveness check

During the existing packaged smoke check, dictate continuously for about one minute while typing or scrolling, then stop. Confirm the UI stays responsive, audio does not accumulate without bound, and stop finishes or reports its bounded error. Reuse Session 4's result if the relevant code/package has not changed. Temporary debugger observations are enough; do not add or commit telemetry, performance counters, a benchmark harness, or instrumentation. Keep the already-decided 100 ms batches and fail-on-overflow policy; this session does not study alternative drop policies.

The existing bridge carries JSON strings only. Keep PCM audio base64-encoded inside those messages; do not add binary transport to the Swift host, WebView2 host, or C++ router. Raw binary audio belongs on the native-to-xAI WebSocket connection after decoding.

## Platform/build validation

Use Session 3's runtime/curl evidence and Session 4's macOS packaged capture evidence. Repeat only checks affected by fixes in this session. Complete the listed Windows checks on a Windows host if available; do not make every session rerun both platforms. These are acceptance checks for the actual package, not another transport feasibility investigation.

Check:
- macOS build/link
- Windows build/link if available in the current environment/configuration
- runtime curl_version_info()->protocols contains wss (and ws for local fake servers)
- macOS uses the pinned bundled curl 8.14.1/SecureTransport build, with WebSockets enabled, nghttp2 disabled, and the pipe2 compatibility workaround intact
- production frontend assets include the capture module and load it under the packaged cha://app origin and existing content security policy
- the specified 16 kHz AudioContext/AudioWorklet produces ordered PCM frames through the JSON bridge in the packaged webview, using synthetic audio and a local fake provider
- microphone permission, stop, cancellation, and cleanup with an actual microphone
- no new runtime dependency accidentally introduced

A macOS runtime without wss is a regression: fix its build/package and rerun the smoke test. Keep the existing cha_runtime_supports_secure_websockets/runtime-smoke checks; a successful link is insufficient. On a non-Apple build whose selected curl lacks the needed ws/wss protocol, assert block 2's explicit startup error and keep OpenAI usable. Do not add another WebSocket library. Windows acceptance checks run on Windows; if this session has no Windows host, record that check as unrun, not passed.

Report unavailable platform checks explicitly. Development-server and unit-test success alone do not establish that packaged audio capture works.

## Tests

Expand tests only where meaningful gaps remain.

### Frontend

Cover:
- provider switching UI
- provider-specific URL validation on load/save, including local test endpoints and cross-provider scheme rejection
- OpenAI regression
- xAI start/stop/cancel
- append-only normalization
- intentional repetition within and across utterances
- punctuation/spacing
- actual composer output across multiple finalized utterances and an existing draft
- final audio flush, final transcription delivery before stop resolves, and cancellation/finalization timeout
- serialized request sizes, one outstanding audio request under delayed replies, and explicit queue-overflow failure with the shared UI bridge still usable
- finalization completes or fails below the default and shortened bridge deadlines
- voice-input setup without voice-output configuration
- no reload on provider/origin changes or merged voice settings; fresh provider selection at startup and cancellation of a pending settings read
- per-piece dictation commands, preserved newlines, and the documented split-command limitation
- stale-event rejection
- resource cleanup
- failure followed by an explicit new dictation attempt (no automatic audio replay)

### C++

Cover:
- WebSocket lifecycle
- auth headers
- xAI message parser
- send ordering
- bounded buffering
- base64 decoding and rejection of malformed/oversized audio payloads
- a stalled send times out with only one native batch; reject a second simultaneous audio request
- cancellation
- shutdown
- malformed input
- connection failures

### Integration

Use the checked-in real event recordings as the network-independent transcript integration oracle. Drive a local fake WebSocket provider through the actual native request/reply path and assert final composer text, stop ordering, queue bounds, failure, and cancellation. The fake provider sends the recorded xAI events; it does not return already-normalized text in place of exercising the native parser.

Use a local TLS WebSocket echo for packaged curl transport acceptance and the synthetic AudioWorklet source for capture acceptance. Do not make CI depend on xAI availability, credentials, speech-recognition accuracy, or another protocol investigation. Live contract verification was completed in the planning review and is recorded with the fixtures.

## Validation

Run the full relevant checks:

```sh
cd webapp
npm run check
npm run build
```

Run the relevant C++ unit/component tests and a native build.

Reuse the passing bridge/native lifecycle and runtime smoke results from Sessions 3/4 unless this session changes the relevant native code or package. Run the checks affected by each fix. Do not broaden into unrelated suites without a failure or changed dependency that warrants it.

Fix regressions caused by Sessions 1, 3, and 4.

## Cleanup

Remove:
- temporary debug logging,
- dead compatibility code introduced only during development,
- unused DTO fields,
- duplicated provider-dispatch logic,
- stale TODO comments,
- abandoned experimental APIs.

Do not perform unrelated refactors.

## Documentation

Update concise voice-input documentation to reflect the fixed implementation:

- supported voice-input providers,
- OpenAI vs xAI configuration,
- API key setup,
- provider-specific defaults,
- any known platform limitation,
- the single word-time cursor, explicit missing-timing error, provider-controlled finalized-text latency, and commands split between pieces remaining literal.

Keep documentation proportional to the feature.

## Deliverable

Commit the fixes with subject `voice input: finish integration checks (session 5)` and a body containing the following summary. If no fixes are needed, do not manufacture a cleanup change or empty commit; add a concise validation record to this document and commit that. Repeat the summary in chat:

1. issues found and fixed,
2. files changed,
3. final OpenAI/xAI architecture,
4. final prompt-update semantics,
5. resource/cancellation behavior,
6. platform/build results,
7. tests run and results,
8. any remaining known limitations.

The final state must meet the concrete acceptance checks so the user can choose OpenAI or xAI and dictate in this personal application. Do not expand this into production infrastructure or unrelated hardening.
