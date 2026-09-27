# CHA browser application

The browser source is built with the pinned Node.js version in `.node-version`.
The packaged desktop hosts load the generated files. Domain calls use the
native bridge; Vite never proxies application RPC.

From `webapp/`:

```sh
npm ci
npm run api-types
npm run check
npm run build
```

From the repository root, native development serves only frontend assets/HMR at `http://127.0.0.1:5173/`:

```sh
make run-native-dev CONFIG=/path/to/cha-config
```

## Runtime and voice

`useLiveSession.ts` owns subscription recovery, navigation epochs, delayed
Recent visibility, and unused-session cleanup. `App.tsx` composes screens and
the resizable sidebar; detail screens own their mutations. Chat sends with
Enter and inserts a newline with Ctrl+Enter. Reasoning events are ignored;
answer entries show usage and search indicators.

`voiceInput.ts` selects OpenAI WebRTC or xAI native WebSocket dictation.
`voiceInputCapture.worklet.js` converts capture to 16 kHz mono PCM16 for xAI.
`dictationText.ts` handles spoken punctuation; `ChatScreen.tsx` handles the
configurable send phrase.
`speechPlayback.ts` pauses microphone capture during playback. `textToSpeech.ts`
uses growing native resources for MP3 playback when MediaSource is available,
and complete clips otherwise. Keys stay in native code.

## Browser tests

`npm run check` runs schema generation checks, TypeScript, and Vitest. Native
host automation lives under `tests/native/`. The old HTTP Playwright suite
that launched `chaweb` has been removed.

Presentation tests use a fake `ChaClient` or fake native bridge. They do not
start an application listener. Component tests verify that Stop and draft
editing remain available while a live stream is replaced.
