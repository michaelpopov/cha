Implement **Session 3: native xAI transport and bridge** in CHA.

Follow the Keep it simple rule in AGENTS.md. If a simpler design meets a requirement, use it.

Read AGENTS.md, [block1.md](block1.md), the Session 1 commit body, and the full shared contract in [block2.md](block2.md). Read the [real fixture notes](fixtures/xai/README.md). The shared contract fixes the protocol, native word-time normalizer, reply shapes, queue bounds, cancellation, and deadlines. Do not infer them from a previous chat or research another design.

## Scope

1. Preserve and validate the prepared bundled-curl macOS fix. Include its existing changes in this session's commit if they remain uncommitted; do not repeat the curl upgrade or switch TLS backends.
2. Implement the application-owned native WebSocket worker, native timestamp deduplication, and start/audio/stop/cancel lifecycle exactly as block 2 specifies.
3. Register the four bridge methods and add DTO/client types and typed client calls. Audio and stop replies carry ordered `pieces: string[]`; they must preserve finalized-piece boundaries for Session 4's command handling.
4. Resolve credentials and provider settings natively, forward the first supplied language, and preserve the ordinary/control bridge limits. Do not introduce an event subscription or binary bridge.
5. Add focused native/bridge tests and replay all saved xAI fixtures against the raw expected results. Use a fake server for readiness, send ordering, partial I/O, cancellation, deadlines, and malformed/missing-timing failures. No live xAI account is required.

Keep the browser's xAI unavailable-transport stub. Do not implement AudioWorklet capture, command formatting, composer wiring, or packaged microphone checks in this session. Those belong to [Session 4](block4.md). Native timestamp deduplication is complete here; Session 4 does not duplicate it in TypeScript.

## Validation and completion

- Build the native target and run the relevant application, bridge, and normalizer tests.
- Run `NativeRuntime.SupportsSecureWebSockets`, the existing macOS runtime smoke, and the local certificate-verified wss binary echo against the rebuilt runtime. The existing curl correction remains a prerequisite, not a reason to tolerate broken macOS xAI support.
- Regenerate API types and run `npm run api-types:check`, `npm run typecheck`, and the affected native-client/wire tests in `webapp`.
- Do not run a packaged AudioWorklet test or add performance instrumentation. Record Windows checks as unrun when there is no Windows host.

Complete this boundary and stop. Commit with subject `voice input: add native xAI streaming (session 3)`. The commit body must list the concrete worker/normalizer files, all four bridge method signatures, typed-client entry points, test commands/results, and any unrun platform checks. Explicitly state that browser capture remains the intentional stub for Session 4. Commit only feature/prerequisite changes, preserving unrelated user work. Session 4 must be able to continue from this commit and the repository documents without this chat.
