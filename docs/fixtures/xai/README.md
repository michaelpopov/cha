# Verified xAI transcript contract

These are seven authenticated recordings made on 2026-09-23 against
`wss://api.x.ai/v1/stt`, using `grok-voice-transcribe-2.0`. They contain only
speech synthesized for this review. They are real provider responses, not
invented examples. The session identifier in `transcript.created` is redacted;
transcript text, flags, word times, event order, and elapsed receive times are
unchanged. No key, authorization header, or personal recording is included.

The implementation decision is in [block2.md](../../block2.md). Replay these
files in tests; implementation sessions do not need credentials or another
protocol investigation.

## Capture configuration

Native libcurl 8.14.1 from the rebuilt macOS `libChaRuntime.dylib`, with
certificate and hostname verification enabled, opened an authenticated
WebSocket. Configuration was `encoding=pcm`, `sample_rate=16000`, `channels=1`,
`endpointing=400`, the model above, and `language=en` (`ru` for the Russian
case). `interim_results=false` except for the explicit comparison case.
No Smart Turn, diarization, keyterms, or periodic finalize messages were used.

The probe waited for `transcript.created`, sent signed PCM16 little-endian in
real-time-paced 100 ms binary messages, sent a final short message if needed,
then sent the text message `{"type":"audio.done"}`. It kept receiving through
`transcript.done`. JSONL rows contain `event` (the provider payload) and
`received_ms` (time since the WebSocket handshake completed). Receive times
are observations, not test deadlines.

[Capture metadata](capture-metadata.json) records audio durations, raw PCM
SHA-256 hashes, language, interim setting, and event counts.
[Expected results](expected.json) lists one expected raw native delta per
JSONL row, final text, and final end-time cursor. Empty additions are included.
Native bridge replies collect the nonempty additions as ordered `pieces`;
they must not merge their boundaries. Session 4 separately applies dictation
commands and editor spacing to each piece. These saved expectations cover
timestamp deduplication before that frontend formatting, not command behavior.
The concatenated result was independently compared with the disjoint
chunk-final `text` fields in these recordings; repeated word spans in the
utterance finals were checked for identical times and text.

## Recordings

| Fixture | Case and observed behavior |
| --- | --- |
| [chunk-overlap](chunk-overlap.jsonl) | Two nonempty chunk finals followed by one stitched utterance final. The second chunk has only four new words although its top-level `start` is still `0.001`. The final repeats all 35 words, then `transcript.done` is empty. |
| [multiple-utterances](multiple-utterances.jsonl) | Four seconds of silence separate speech segments. Separate utterances repeat “Try again. Try again.” Word times continue past 12 and 18 seconds; they do not reset at the boundary. Empty chunk finals during silence contribute nothing. |
| [stop-during-speech](stop-during-speech.jsonl) | Audio stops after 7.0625 seconds, without trailing silence. Final words arrive in chunk/utterance-final events before an empty `transcript.done`. |
| [silence](silence.jsonl) | Two seconds of zeros produce readiness and an empty successful completion. |
| [long-utterance](long-utterance.jsonl) | Approximately 25 seconds of continuous synthesized speech. The first locked result arrives at 26.878 seconds; repeated phrases within it survive. No three-second locked-result cadence was observed. |
| [interim-comparison](interim-comparison.jsonl) | The same long audio with interim results enabled. Mutable text arrives with empty word arrays; enabling interims does not make locked text arrive during the long utterance. Ignore all those interim events when replaying the normalizer. |
| [russian](russian.jsonl) | Russian text and punctuation, including the provider's hyphenated “очень-очень”. Word timestamps and final overlap follow the same rule. |

## Decisions supported by these observations

- Use word `start`/`end` directly as seconds on the connection's audio clock.
  Never add top-level event `start` or use `start + duration` as a word cursor.
- Keep one end-time cursor across every utterance. Select words with
  `end > previous_cursor`, then advance it. The same words in overlapping
  finals have exactly equal times in these recordings; do not add tolerance
  or text-based deduplication.
- Process final partials before completion. An empty `transcript.done` is
  normal; it is not evidence that no speech was recognized.
- Keep interim results disabled and append only locked words. This preserves
  append-only editing, with a real latency cost during continuous speech.
  Do not promise subsecond or three-second committed-text updates.
- All nonempty final events in these captures contain word timings. A
  nonempty final without timings is an unsupported response: fail explicitly.
  Do not invent a text-prefix fallback. Missing-timing tests must be labeled
  synthetic error cases, not observations of the live service.

These measurements establish the implementation contract for the tested
model/settings. They do not guarantee every possible provider response or
future model behavior. Silence and completion can legitimately have no words.

## Controlled audio recipe

macOS `say` generated AIFF, and `afconvert -f WAVE -d LEI16@16000 -c 1`
converted it to 16 kHz mono PCM. The WAV header was removed before transmission.
Both English clips used voice Daniel at rate 165:

- Clip A: “I want to build a small application that records my ideas and keeps
  every word in the right order because this is very very useful and I want
  to try again” (8.4106875 seconds).
- Clip B: “Try again. Try again.” (1.9135625 seconds).

The Russian clip used Milena at rate 165: “Я хочу создать небольшое приложение.
Это очень очень полезно. Попробуй ещё раз.” (5.6161875 seconds).

Audio layouts: chunk-overlap = A, 1 s silence, B, 1 s silence;
multiple-utterances = A, 4 s silence, B, 4 s silence, B, 4 s silence;
stop-during-speech = the first 7.0625 s of A;
long-utterance/interim-comparison = A three times, 4 s silence, B, 4 s silence;
Russian = its clip plus 3 s silence. The silence case contains only zeros.
A future speech-engine version may synthesize different bytes; the saved
provider events, not regenerated TTS recognition, are the regression oracle.

## Other verification completed in this review

- The rebuilt macOS runtime advertises `ws` and `wss`, passed a local
  certificate-verified TLS WebSocket binary echo, and completed all seven
  authenticated xAI sessions. The SDK curl does not support these protocols.
- A separate WKWebView probe used CHA's existing asset scheme handler,
  `cha://app`, and production CSP. A same-origin AudioWorklet loaded and
  received nonzero synthetic audio at 16 kHz through an AudioContext requested
  at that rate. No CSP relaxation or custom resampler was needed.
- Windows execution and the future feature's actual packaged microphone path
  were not exercised here. Session 4 owns macOS capture acceptance; Session 5
  reuses that evidence and covers remaining available platforms. They do not
  need to select a transport or infer semantics.
