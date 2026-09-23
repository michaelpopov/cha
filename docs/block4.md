Implement **Session 4: xAI capture and composer integration** in CHA.

Follow the Keep it simple rule in AGENTS.md. If a simpler design meets a requirement, use it.

Read AGENTS.md, [block1.md](block1.md), [block3.md](block3.md), the completed Session 1 and Session 3 commit bodies, and the full shared contract in [block2.md](block2.md). The native worker, timestamp deduplication, and typed bridge calls must already be present. Use them; do not redesign the transport, move the timestamp algorithm into TypeScript, or obtain new live fixtures.

## Scope

1. Replace the xAI stub with the 16 kHz AudioContext/AudioWorklet path, the separately emitted same-origin worklet asset, and block 2's 100 ms base64 batches and bounded credits. Keep one audio bridge request outstanding and fail on overflow.
2. Connect the typed start/audio/stop/cancel calls from Session 3. Wait for native readiness before sending audio; drain capture and format all final reply pieces before stop resolves. Keep startup/stop cancellation and stale-attempt checks.
3. Share the existing dictation-command helper and normalize each finalized piece in the xAI adapter. Preserve newline-only pieces, recompute spacing, and use the first-addition helper specified in block 2. Keep the accepted split-command limitation and unchanged OpenAI behavior.
4. Use Session 1's fresh runtime settings read at dictation start and the current composer language. Do not restore provider/origin page reloads or hardcode English.
5. Add focused frontend/composer tests for capture, queue bounds, reply-piece order, commands/newlines, languages, final stop delivery, errors, and cancellation. Fix only native integration defects uncovered by these tests; retain Session 3's contract.

## Validation and completion

Run `npm run check` and `npm run build` in `webapp`. Reuse Session 3's native results unless native code changed; rerun affected tests when it did.

Build/stage the current native runtime and production assets and perform one macOS packaged check under `cha://app` and the existing CSP. Use synthetic audio and a local fake xAI provider for capture/bridge/stop ordering, then check actual microphone permission and cleanup. Include a roughly one-minute responsiveness check while typing or scrolling; temporary observation is enough. Do not commit instrumentation or repeat a live protocol investigation. Record unrun Windows acceptance for Session 5, rather than pretending it passed.

At this boundary xAI is selectable and usable, with documented finalized-text latency and split-command limitations. Commit with subject `voice input: connect xAI capture and composer (session 4)`. The body must list adapter/worklet/text-helper/composer files, the callback and reply-piece contract, test commands/results, packaged-check evidence, and any unrun platform checks. Commit only feature changes. Session 5 reads that committed handoff and [block5.md](block5.md), not this chat.
