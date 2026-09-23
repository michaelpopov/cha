This is the shared implementation contract for Sessions 3 and 4 of selectable xAI Grok Voice Transcribe support in CHA. Run [block3.md](block3.md) first and [block4.md](block4.md) second; do not attempt both as one session.

Follow the Keep it simple rule in AGENTS.md. If a simpler design meets a requirement, use it.

Session 1 should already have introduced the provider distinction (openai | xai), persisted it through settings/configuration, preserved the existing OpenAI WebRTC implementation, and left an explicit extension point for xAI.
Your task is to implement the actual xAI streaming transcription path with append-only finalized text while keeping OpenAI fully functional.
The protocol and design decisions below were settled in the 2026-09-23 review, including authenticated xAI tests. Implement this contract and replay the checked-in fixtures. Do not assign protocol research or normalization choices to this or the next session.

## Fixed division of work

- **Session 3 owns native work:** preserve the existing curl fix, implement the WebSocket worker, timestamp deduplication, four bridge methods and their typed client calls, native/fake-server tests, and the test-only fake-server executable for Session 4's packaged check. The normalizer stays native because it consumes provider word timestamps. Keep the browser xAI unavailable stub in place. No AudioWorklet or packaged capture work belongs in Session 3.
- **Session 4 owns browser integration:** implement AudioWorklet capture, bounded audio sending, per-piece dictation-command formatting, composer integration, frontend tests, and the one packaged macOS capture/stop check. Reuse Session 3's tested native normalizer and bridge; do not build another normalizer in TypeScript.
- **Session 5 owns remaining regression fixes:** reuse passing evidence from Sessions 3/4 and rerun affected checks only after relevant changes. Run Windows acceptance on a Windows host when available; do not require both Sessions 3 and 4 to reproduce every platform check.

Each entry prompt lists its completion boundary and commit handoff. Read the preceding session's commit body and the referenced repository files. No handoff may depend only on chat. A new worktree must contain all prerequisite commits, including the committed curl prerequisite 5ed0898.

## Goal

When Voice Input provider is configured as xai, CHA should:

```text
microphone
   ↓
capture audio continuously
   ↓
native bridge
   ↓
C++ xAI streaming transport
   ↓
Grok Voice Transcribe 2.0
   ↓
partial/final transcription events
   ↓
normalized CHA transcription callback
   ↓
prompt editor
```

Capture and send continuously, and append locked words as soon as xAI returns them. Finalized-text latency is provider-controlled: the real long-utterance fixture first produced locked words after about 26.9 seconds. Do not promise a three-second update cadence, append mutable hypotheses, or add periodic forced finalization to mask that limitation.
Do not replace or alter the working OpenAI WebRTC transport except where necessary to share provider-neutral interfaces.

## First: inspect Session 1

Before changing anything:

1. Inspect the current branch and understand what Session 1 implemented.
2. Find the provider-neutral interface/extension point it established.
3. Preserve that design unless there is a concrete correctness problem.
4. Do not independently redesign voice input from scratch.

Relevant implementation areas:

- webapp/src/voiceInput.ts
- webapp/src/voiceInput.test.ts
- Chat composer integration
- native bridge request/event plumbing
- src/app/media_operations.cpp
- application/bridge DTOs
- voice-input settings
- CMake/libcurl configuration

## Fixed xAI connection contract

Reference: [official xAI speech-to-text documentation](https://docs.x.ai/developers/model-capabilities/audio/speech-to-text), checked 2026-09-23. The following is the selected CHA configuration; do not choose a different encoding, transport, or set of optional features.

- Default endpoint: wss://api.x.ai/v1/stt; use the saved endpoint for local fake-server tests.
- Model: the saved model, default grok-voice-transcribe-2.0.
- Native authentication: Authorization: Bearer <resolved vault secret>. Never put the key in the URL.
- Query parameters: model, encoding=pcm, sample_rate=16000, channels=1, interim_results=false, endpointing=400. Forward the composer's language array through voiceInput.xai.start and set language to its first nonempty entry (currently en or ru); omit it if none is supplied. This is a formatting hint, not a restriction on the language recognized. Add request-construction assertions for ['en'], ['ru'], and an empty list; never hardcode English or send the array as the query value.
- Construct/escape the query natively, only from the parameters above. If the saved endpoint already contains a query string, ignore it and log a warning without the URL or query. Do not merge, retain, or filter saved query parameters. Do not log the full URL. Do not add a setup message or map OpenAI delay/prompt to xAI parameters.
- Wait for transcript.created before sending PCM. Send raw binary WebSocket messages; base64 is used only across CHA's JSON bridge.
- Ignore unexpected interim transcript.partial events with is_final=false. They must never affect committed text.
- At stop, send exactly {"type":"audio.done"} as a text WebSocket message after all PCM. Wait for transcript.done before successful completion. Do not use finalize/Finalize or invent another stop message.
- Any provider error is terminal for this dictation, even if xAI leaves its socket open. A socket close before transcript.done is failure. A transcript.done that arrives before CHA sends audio.done is also failure. No automatic reconnection or audio replay.

## Native transport and curl

Use libcurl's WebSocket API with CURLOPT_CONNECT_ONLY=2L; do not introduce another networking library. Use one native worker as the sole owner of each CURL handle. In all deadline formulas below, D is the effective bridge request deadline in milliseconds, including any router/runtime override. The router does not pass D to native operations today: add that argument to the xAI start, audio, and stop operations. Preserve TLS peer/hostname verification and disable redirects on this authenticated connection.
The curl build correction is committed in this branch as 5ed0898:

- Apple skips find_package(CURL) and statically links the bundled curl 8.14.1 into libChaRuntime.dylib.
- CURL_DISABLE_WEBSOCKETS=OFF; Apple uses SecureTransport with OpenSSL and nghttp2 disabled for curl. All macOS HTTP traffic uses this bundled curl; HTTP/2 is unavailable in that build.
- Preserve the HAVE_PIPE2=0 deployment compatibility workaround.
- Do not upgrade curl in these sessions. SecureTransport was removed in curl 8.15; upgrading would require a separate TLS-backend change.

Before opening xAI, inspect curl_version_info(CURLVERSION_NOW)->protocols for the endpoint's scheme (wss in production, ws for local fakes). A compiled/linked curl_ws_send symbol is not a capability check. On macOS, missing wss is a build regression to fix, not an accepted platform limitation. Elsewhere, reject startup with "xAI voice input requires a curl build with WebSocket support" if that runtime lacks the scheme; OpenAI remains usable.
Handle CURLE_AGAIN, partial sends, receive buffer splits, fragmented messages, and ping/pong/close frames. Finish a partial binary message before starting another message or audio.done. Limit an assembled provider JSON message to 1 MiB and pending transcript additions to 64 KiB; overflow terminates dictation with an error. Use nonblocking I/O and at most 100 ms waits so cancellation is bounded. Do not call a blocking connection attempt with an uninterruptible long timeout: adapt the cancellable curl-multi handshake loop in src/app/media_operations.cpp, but keep the easy handle attached to its multi handle for the entire WebSocket lifetime. Removing the easy handle after the handshake makes CONNECT_ONLY send/receive unusable. Remove it only during final transport cleanup.

## API key security

Do not expose the xAI API key to JavaScript.
The browser should communicate audio with CHA's native/application layer, and native C++ should authenticate to xAI using the configured API key.
Preserve the same vault/API-key ownership model already used by voice settings.

## Audio capture

Use getUserMedia({audio: {channelCount: 1}}), AudioContext({sampleRate: 16000}), and an AudioWorklet module shipped as a same-origin JavaScript asset. Route microphone -> worklet -> zero-gain node -> context destination, keeping the graph active without microphone playback. Explicitly downmix the worklet input to mono, then convert clamped Float32 samples to signed PCM16 little-endian. Require context.sampleRate === 16000; report an unsupported-audio-format error if the browser cannot create that context. Do not implement a custom resampler, MediaRecorder upload path, ScriptProcessor fallback, or an Opus encoder.
Produce 1,600-sample (100 ms) batches: 3,200 PCM bytes and 4,268 base64 characters. Send each batch once as base64 in JSON, decode once natively, then send binary PCM to xAI. Bound the full serialized request below 65,536 bytes. The final batch may contain fewer samples but must have an even nonzero byte count.
The worklet posts each completed batch to the main thread and holds at most one partial batch. It keeps no credit state. The main thread counts the batches that wait for a native audio reply, including the one in-flight request, and removes a batch from the count only after its native audio reply. At most 19 completed batches may wait. If another completed batch arrives while 19 wait, report overflow and stop the session; never drop audio silently. During graceful stop, the final short flush batch may use one additional twentieth slot. This bounds unsent/in-flight audio to about two seconds, including the final flush. Native holds at most the one outstanding batch and replies only after it has been fully written to the WebSocket, so no second native audio queue is needed. Bound each audio send to min(2000, floor(2*D/3)) milliseconds, including socket stalls. Fail the dictation when that bound expires.
Create the microphone stream/context during startup, but connect/start capture only after native start succeeds. Native start resolves only after transcript.created, not just the WebSocket handshake; browser start resolves after that readiness and capture setup. Cancellation during startup must also stop a getUserMedia stream that resolves late.
On graceful stop, send a worklet flush command, receive its last partial batch and flush acknowledgment, then disconnect capture and stop microphone tracks. Accept those final worklet messages while stopping, drain all batches, and only then invoke native stop. On cancel or failure, discard pending audio and release the graph immediately.
Load the worklet with a bundled asset URL (import captureUrl from './voiceInputCapture.worklet.js?url&no-inline', then audioWorklet.addModule(captureUrl)), not blob:, data:, a CDN, or inline code. Vite must emit a separate file served under cha://app; do not let a small module become an inline data: URL. Keep the existing CSP. In this review, a macOS WKWebView probe using CHA's asset handler, cha://app origin, and current CSP loaded a same-origin worklet and processed synthetic audio at 16 kHz. Keep this verified fixed sample rate; do not add rate negotiation or a manual resampler. Session 4 performs the actual packaged implementation and microphone permission/cleanup check once.

## Fixed transcript algorithm

Evidence and replay data: [xAI fixtures](fixtures/xai/README.md), captured with grok-voice-transcribe-2.0 on 2026-09-23. The real runs establish the contract used here:

- Final words use seconds from the beginning of the connection's audio stream, including silence. Later utterances do not reset that clock.
- Chunk-final text can contain only new words while top-level start/duration still describe a broader window. Utterance finals repeat the earlier words with identical word timestamps. Do not add event.start to a word timestamp or advance a cursor from event.duration.
- In these recordings, transcript.done has empty text and words; the last words arrive in transcript.partial events before it. Treat done as the completion signal, not a guaranteed full-session transcript.
- Interim text is mutable and may have an empty words array. Disable it in production and ignore it if received.

Implement one native C++ normalizer, tested independently of networking. Its transcript state is last_committed_end, initially -1, plus the last emitted character needed for spacing. It has no per-utterance transcript strings, prefix matching, or audio-coverage interval collection.
For each complete decoded event:

1. transcript.created changes readiness only. An unknown event type is ignored. An error event fails the session. Malformed JSON or malformed fields of a recognized final event fail the session.
2. Ignore transcript.partial with is_final=false before examining words. Process transcript.partial with is_final=true (either speech_final value), and process any words in transcript.done by the same rule. Require boolean flags for partial events.
3. A missing/empty words array is a no-op only when text is missing or an empty/whitespace-only string. If a final event has nonempty text but no word timings, terminate dictation with "xAI returned a transcript without word timings." Do not append text as a fallback, reinterpret a prefix, or silently lose it. A non-array words field, non-string text, invalid word object, or non-finite/negative timestamp is a protocol error. Each word requires string text and finite 0 <= start <= end; require nondecreasing end times within the array.
4. Snapshot the old cursor before processing the event. In the original array order, select all words with end > old_cursor. Comparing against the snapshot retains multiple new words with an equal end time in the same event. Do not compare word strings, round times, introduce an epsilon, or reset the cursor on speech_final.
5. Append the selected word texts in order. Before each nonempty trimmed word text, insert one ASCII space if text was already emitted, the previous emitted character is not whitespace, and the new text does not begin with one of , . ; : ! ? ) } ]. Otherwise append directly. Preserve the provider's capitalization and punctuation; do not use top-level text to replace previously emitted words. The first emitted word has no leading separator. Track spacing across bridge replies as well as within an event.
6. Advance last_committed_end to the maximum end among the selected words. Do not change it for an all-overlapping or empty event. Repeated words at different times are distinct and must survive. A repeated final with the same times contributes nothing.
7. After processing transcript.done, complete native stop only after transport cleanup. Empty text/words is a valid successful completion. If the connection closes before done, fail even when some text was already delivered.

Use strict word timings for this release. There is no text-only fallback and no live algorithm selection. The fixtures support this decision, not a guarantee that every future provider/model version will behave identically. Keep malformed/missing-timing behavior deterministic and do not silently switch algorithms.
The output above is the native raw-text contract used by expected.json. Preserve each nonempty event addition as a separate piece in bridge replies; do not concatenate pieces before frontend command formatting. Timestamp deduplication stays native. The xAI browser adapter prepares each piece for the editor using the following fixed rule.

## Dictation commands and editor formatting (Session 4)

The current appendTranscription() calls normalizeDictationCommands() only for the first composer addition. Later deltas bypass it. Fix this for xAI without changing OpenAI's existing callback behavior:

1. Export the existing normalizeDictationCommands() implementation from a small shared text-helper module used by both voice implementations. Move its command table unchanged; do not copy it into C++ or create a second command vocabulary. Keep appendTranscription()'s OpenAI behavior unchanged.
2. For each raw finalized piece from native, call that helper once. Remove only leading/trailing spaces and tabs from the result, never line breaks. Ignore an empty result, but emit a newline-only result. Do not call appendTranscription() here: its trim() would erase a standalone or trailing newline.
3. Recompute the separator from the last character actually emitted to the editor, not from the native raw transcript. Add one space only when the preceding character exists and is not whitespace, and the new piece starts with neither whitespace nor one of , . ; : ! ? ) } ]. This prevents a leading space after a normalized newline. Keep only that last-character state; do not accumulate and re-normalize the whole dictation.
4. For the first xAI callback, the composer uses a small appendPreparedTranscription(currentDraft, preparedDelta) helper with the same separator rule and no normalization or trim. Later xAI callbacks concatenate directly. Preserve the existing first/later OpenAI branches exactly. Choose this first-addition behavior from the fresh provider captured at startup; it is a formatting choice, not xAI protocol parsing in ChatScreen. This also preserves a newline-only first addition to an existing draft.
5. Normalize only within each piece. A command split between finalized pieces, such as "new" then "line", remains literal text. This is an accepted limitation. Do not buffer words across pieces or revise earlier text. Keep piece boundaries even when one audio/stop reply contains several additions.

Test the actual composer, not just the helper: later "comma" and "точка" become punctuation; later "new line" and "новая строка" become line breaks; newline-only first and later additions survive; a following word has no unwanted leading space after a newline. For example, pieces "Hello", " new line", " world" must yield "Hello\nworld". Include an existing draft, a stop-reply piece, multiple pieces in one reply, and the accepted split-command limitation. The checked-in provider fixtures and expected.json remain raw native expectations; add these command cases as separate frontend fixtures.

## Provider-specific session implementation

Use this architecture:

```text
VoiceInputSession
       │
       ├── OpenAI realtime session
       │      WebRTC
       │
       └── xAI realtime session
              audio stream
              native bridge
              WebSocket
```

Both expose the same high-level operations to the composer:

```text
start()
stop(): Promise<void>
cancel()
onTranscription(delta)
onFailure(error)
```

Do not leak xAI protocol details into ChatScreen.
For xAI, stop() is a completion barrier:

- stop accepting new microphone audio and flush the remaining captured samples, including a partially filled worklet frame
- drain queued bridge/audio frames to the WebSocket in order, then send audio.done after the last audio frame
- process the final transcript.done and deliver any remaining transcription callback before resolving
- release microphone, capture, pending bridge requests, and WebSocket resources before resolving

Use a 20-second maximum budget for the entire frontend stop, starting when stop() is called and including worklet flush, audio drain, provider finalization, and cleanup. Let D be the effective bridge request deadline; native start returns stop_budget_ms = min(20000, floor(2*D/3)). Use that shorter budget when D is overridden. The frontend passes the remaining budget to native stop, which caps it again against the effective deadline. Do not reset the budget at each stage. Abort on expiration, reject stop(), and release resources. Stop completion is a final result, not an acknowledgment that stopping began.
Cancel is immediate from the composer's perspective: stop capture, discard queued audio, suppress later callbacks, abort pending invokes, send the control cancel request, and settle pending start/stop promises. The worker must close its transport promptly without waiting for transcript.done. Make repeated stop/cancel calls idempotent. Keep existing OpenAI behavior unchanged.

## Native bridge protocol

Add the four request/reply methods below. No new transcription event channel is needed.
Inspect and preserve the existing bridge limits in src/bridge/bridge_router.h, src/bridge/bridge_router.cpp, src/runtime/runtime_settings.h, and both native hosts:

- Each connection has 16 ordinary request slots shared with the rest of the UI. Submitting another request when they are full invalidates the entire connection, not just the audio request. Slots are released when the frontend acknowledges delivery of replies.
- Each serialized request is limited to 65,536 bytes (64 KiB), including the JSON envelope.
- Normal requests have a default 30-second deadline, with runtime/test overrides. Do not exempt voice requests or increase the global deadline to keep dictation alive.
- The Swift and WebView2 hosts accept JSON strings; there is no binary bridge path.

Keep at most one audio request outstanding per dictation. Wait for its reply using the existing bridge invocation/delivery-acknowledgment flow before submitting the next batch, leaving request capacity for other UI work. Do not send audio requests without waiting for replies, bypass delivery acknowledgments, or raise bridge limits.
Use the exact 19-batch main-thread bound and one native in-flight batch specified above. Reply only after a batch has been fully sent; do not acknowledge mere acceptance into another queue. If a queue fills or an audio request fails, terminate dictation with a concise error, release its resources, and stop sending queued batches. Do not silently drop audio or retry into the same backlog. Keep cancellation responsive while audio is stalled, following the existing control-request conventions.
Use these exact new methods and JSON payloads, registered through the existing bridge protocol and native client:

- voiceInput.xai.start: {session_id, languages}; returns {session_id, stop_budget_ms} after transcript.created. Bound connection/readiness startup to min(15000, floor(2*D/3)) milliseconds. The frontend creates a fresh session_id before invoking start so cancellation can target a startup attempt.
- voiceInput.xai.audio: {session_id, pcm_base64}; returns {session_id, pieces: string[]} after the whole binary audio message is sent. pieces contains the ordered nonempty event additions not previously delivered, or an empty array. Preserve piece boundaries for command normalization; keep the existing 64 KiB total pending-text bound. Keep exactly one audio request outstanding. Do not retry audio requests.
- voiceInput.xai.stop: {session_id, remaining_ms}; returns {session_id, pieces: string[]} only after audio.done, transcript.done, and native cleanup. The frontend formats/delivers every remaining piece before resolving its stop() promise.
- voiceInput.xai.cancel: {session_id}; returns {} promptly and signals worker cancellation. Classify it as a control request like existing voiceInput.cancel. Cancellation of an absent/already-finished session is a successful no-op. Explicitly invoke this method with session_id when cancelling; do not use NativeBridge.invoke cancelMethod, which sends only request_id. AbortSignal may still settle frontend invokes locally. Retain a cancellation tombstone for a queued start with that session_id until the start is rejected, so the priority control queue cannot cancel first and then allow startup to resurrect the session.

Session identity is scoped to bridge connection and context epoch. Reject duplicate starts or overlapping audio calls for an active session. Accept at most one xAI dictation per connection. Resolve endpoint, model, and secret from native settings at start, not from browser-supplied credentials or a replacement endpoint. Reject an xAI start if the current native provider is no longer xai; do not silently dispatch another transport.
Transcript delivery uses audio/stop replies, not a new event subscription or a polling request. Send silent PCM while capture is active so audio replies continue delivering transcripts during pauses. A worker error is returned by the current/next audio or stop request. On failure, discard pieces that were not yet delivered; do not return them with the error. Text already delivered to the composer stays. Preserve a small terminal result for that session until consumed/cancelled or superseded; do not leave a worker running to remember it. Replies from a cancelled/older session must be ignored by the frontend.
Avoid sending one bridge request that blocks for the entire dictation session.
Design ownership and cancellation carefully:

- one xAI transcription connection per active dictation
- stable request/session identity
- stale events from an old dictation must not affect a new one
- closing/cancelling must release WebSocket and audio resources
- application shutdown/vault switch must terminate outstanding transcription cleanly

## Threading and ownership

Launch the worker through Application::Impl::background_jobs, as the existing OpenAI native connect operation does. Store a small xAI session registry in Application::Impl, keyed by connection and session_id, with the originating context epoch. The worker owns CURL/CURLM and its normalizer; bridge operations only exchange a pending operation/batch, cancellation state, and replies under a mutex. Do not create a generic streaming framework or a second thread pool.
Start/audio/stop return OperationReply objects through the router's existing start_background path. Complete those replies from the owning worker; never run socket receive or readiness loops on the router/UI thread and never hold the session/lifecycle mutex during network waits. Native cancel signals the session without waiting for network completion. Clear/suppress delivery on connection close, vault/context change, renderer replacement, and application shutdown. Cancel the worker before application-owned state is destroyed, and use BackgroundJobs' existing shutdown/join ownership rather than detached threads. Preserve the host's existing bounded shutdown behavior.

## Error handling

Handle at least:

- authentication failure
- unsupported model/configuration
- WebSocket connection failure
- unexpected close
- malformed xAI message
- microphone/audio capture failure
- native bridge failure
- cancellation during startup
- cancellation while actively streaming
- stop while a final transcript is still pending

User-facing failures should remain concise; detailed provider diagnostics may go to logs.
Never log API keys or audio/transcript contents unless existing CHA policy explicitly allows that.

## Tests

Add focused tests for both sides.
### TypeScript/browser

Cover at least:

- provider dispatch selects xAI correctly
- OpenAI still selects existing WebRTC path
- xAI audio capture starts/stops correctly
- audio frames are forwarded in order
- base64 batches and their complete JSON envelopes remain below the request-size limit
- delayed replies allow only one outstanding audio request while other UI requests remain usable
- queue overflow stops capture and reports an error without silently dropping audio or invalidating the shared bridge by flooding it
- cancellation releases microphone resources
- mutable interim xAI transcripts do not rewrite committed prompt text
- replies preserve native fixture piece boundaries and the frontend formats each piece exactly once; no assumptions that transcript.done repeats the full transcript
- dictation commands in later pieces, newline-only pieces, punctuation, and the accepted split-command limitation follow the formatting contract
- identical phrases in distinct utterances and intentionally repeated words within an utterance are preserved
- multiple finalized chunks append correctly
- actual composer output has correct spacing/punctuation across utterances and when appending to an existing draft
- stop flushes a partial capture frame and waits for final transcription delivery before resolving, including send-while-stopping
- finalization timeout and cancellation during stop settle the promise and release resources
- stop finishes or fails within min(20000, floor(2*D/3)) milliseconds, including a shorter deadline override
- stale events from an old dictation are ignored
- failure terminates the session cleanly

### C++

Cover at least:

- xAI request construction/authentication
- WebSocket initialization
- audio-frame sending
- base64 decoding preserves PCM bytes; malformed or oversized audio payloads are rejected
- a stalled WebSocket holds at most one native audio batch and fails within its send deadline
- queued audio is sent before audio.done, including the final short frame
- xAI transcript message parsing and the exact normalizer above
- replay every fixture and compare every event addition and final text with expected.json
- empty completion, missing/invalid timings, duplicate finals, unexpected interim events, and a transcript.done before audio.done
- malformed messages
- provider errors
- cancellation
- connection shutdown
- finalization timeout releases the connection and completes the pending operation with an error
- application shutdown with an active xAI request
- API key is resolved server-side and never included in browser-facing configuration/events

Use a fake WebSocket boundary for parser/lifecycle tests and a plain ws:// local fake server for handshake/framing tests. Reuse the loopback socket code in tests/support/mock_http_server.h where it fits. Do not build a TLS test server. Session 3 also builds the fake server as a small test-only executable that replays one fixture JSONL file on ws://127.0.0.1; Session 4 uses it for the packaged check.
Normal tests replay the approved real fixture and fake WebSocket failures without network access. Do not make obtaining credentials, researching provider behavior, or selecting a normalization algorithm part of implementation.

## Validation by session

Session 3 runs the native normalizer/bridge/fake-server tests and the macOS runtime capability check: the existing runtime-smoke and NativeRuntime.SupportsSecureWebSockets tests. A successful link alone is insufficient. There is no TLS echo test: these checks and the seven live wss captures in the fixtures prove the wss path. Regenerate API types and run frontend type/client checks for its typed bridge additions; no packaged capture check is assigned to Session 3.

Session 4 runs the frontend checks:

```sh
cd webapp
npm run check
npm run build
```

It then builds/stages the current native runtime and production assets and runs one macOS packaged capture/stop/cancel check with the real microphone and Session 3's fake-server executable, plus microphone permission/cleanup. The fake provider ignores audio content, so no synthetic audio source is needed. An older packages/CHA.app is not evidence about the new implementation. Reuse Session 3's native results unless Session 4 changes that code. Record Windows as unrun if unavailable; Session 5 owns remaining available-platform acceptance. No new live-provider test or permanent performance instrumentation is required.

## Scope control

Do not:

- redesign unrelated voice-output code
- modify provider/model inference infrastructure
- refactor unrelated application networking
- introduce binary bridge transport or raise bridge capacity/deadlines for audio streaming
- remove OpenAI transcription
- expose provider credentials to the browser
- add a second WebSocket library or a browser-to-provider credential path
- perform unrelated cleanup

## Deliverable

At the end of Session 3, the native/fake-server path and typed bridge contract are complete and committed; browser xAI remains an explicit stub. At the end of Session 4, xAI is selectable and usable with the documented latency and split-command limitations. Keep the two session scopes separate and do not reopen native protocol decisions in Session 4.

Commit each phase with its entry prompt's subject and a body summarizing:

1. files changed
2. final OpenAI vs xAI transport architecture
3. audio format and streaming mechanism used
4. xAI event → CHA transcription normalization behavior
5. cancellation/shutdown ownership
6. tests run and results
7. any platform-specific libcurl/WebSocket limitations
8. actual failed/unavailable implementation checks; do not hand unresolved protocol choices to Session 5

COMPLETED
