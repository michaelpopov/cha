# Native bridge

`bridge/` adapts native host requests and deliveries to the owning application
API. It validates request envelopes, dispatches methods, serializes replies and
events, and tracks delivery acknowledgements. It contains no session,
workspace, or persistence implementation.

| Source | Responsibility |
| --- | --- |
| `bridge_protocol.*` | Native request envelopes, delivery values, parsing, and serialization. |
| `bridge_router.*` | Per-connection routing, asynchronous replies, subscriptions, acknowledgements, and shutdown. |
| `operation_dispatch.h` | Shared dispatch context and operation registration seam. |
| `request_params.h` | Small typed request-parameter readers. |
| `settings_dispatch.cpp` | Settings and credential method dispatch. |
| `workspace_dispatch.cpp` | Workspace, character, forum, and file method dispatch. |

The bridge depends on `app/` and the DTOs needed to cross the native boundary.
Nothing below the application boundary depends on `bridge/`.

Tests live in `tests/bridge/`. Native C API ownership and host integration are
tested separately under `tests/native/`.
