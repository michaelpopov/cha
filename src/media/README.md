# Media

`media/` owns downloaded entry audio and connection-scoped in-memory media
resources. It contains the queues, cancellation state, and resource lifetime
rules used by application media operations.

| Source | Responsibility |
| --- | --- |
| `audio_download.*` | Bounded background FishAudio downloads, retries, cancellation, and cached-entry writes. |
| `media_resources.*` | Opaque connection-scoped media handles and byte storage. |
| `pending_media_registry.*` | Cancellation and cleanup associations for pending media replies. |

Application-facing request handling remains in `app/media_operations.*`.
FishAudio request construction and transport remain in `providers/fish_audio.*`.

Tests live in `tests/media/`; application-level media flows are covered by
`tests/app/unit_media_operations.cpp`.
