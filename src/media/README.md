# Media

`media/` owns downloaded entry audio and connection-scoped in-memory media
resources. It contains the queues, cancellation state, and resource lifetime
rules used by application media operations.

| Source | Responsibility |
| --- | --- |
| `audio_download.*` | Background FishAudio downloads, retries, cancellation, and cached-entry writes. |
| `audio_stream.h` | Growing audio bytes shared by download workers and native playback resources. |
| `media_resources.*` | Opaque connection-scoped media handles and byte storage. |
| `pending_media_registry.*` | Cancellation and cleanup associations for pending media replies. |
| `xai_transcript.*` | xAI word-time normalizer, STT URL, and PCM base64 decoding. |
| `xai_socket.h`, `xai_curl_socket.cpp` | One libcurl WebSocket connection for an xAI dictation. |
| `xai_voice_session.*` | xAI dictation registry, worker, and bridge reply lifecycle. |

Application-facing request handling remains in `app/media_operations.*`.
FishAudio request construction and transport remain in `providers/fish_audio.*`.

Fresh speech can be played while its download grows; complete validated audio
is still cached in SQLite. Speech uses text derived from the stored entry,
without an intermediate model request. xAI capture
uses 16 kHz mono PCM16 and native WebSocket transport, with credentials kept
outside the renderer.

Tests live in `tests/media/`; application-level media flows are covered by
`tests/app/unit_media_operations.cpp`.
