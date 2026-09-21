# Storage layer

`storage/` owns SQLite access and the durable representation of workspace and
session data. It contains no controller, browser, or external-service policy.

All persistent sessions share the SQLite file selected by the external
application config's `data` setting. Public `(forum_id, session_id)` identities
resolve to internal `session_key` values, and every restore and journal query is
scoped by that key.

The top-level workspace store owns the database lease and private root. The
session repository receives explicit database, materialized-workspace, and
Welcome paths and owns none of them. Repository operations use short-lived
connections, while each live controller owns a separate journal connection.

| Source | Responsibility |
| --- | --- |
| `sqlite_storage.*` | SQLite connection, encryption setup, and common helpers. |
| `workspace_session_database.*` | Workspace schema, validation, WAL initialization, and checkpointing. |
| `session_database.*` | Session-key-scoped restore and journal operations. |
| `session_repository.*` | Listing, creation, rename, deletion, history, and preparation. |
| `session_storage_layout.*` | Import-only detection of legacy per-session databases. |
| `session_lease.*` | Portable companion-file lease for the runtime store and offline transfers. |
| `stored_session.h` | Stored-session listing value. |
| `session_label.*` | Shared validation for persisted session labels. |
| `session_timestamp.h` | Unix-time helper used by storage writes. |
| `not_found_error.h` | Stable forum/session absence errors exposed by storage operations. |

This directory may depend on `chat/`, `workspace/`, and `util/`. It contains no
application, bridge, runtime, or external-service policy.

Tests live in `tests/storage/`. Database-backed transcript constraints are also
exercised beside transcript validation in `tests/chat/unit_transcript.cpp`.
