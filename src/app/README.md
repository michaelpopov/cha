# Application layer

`app/` is the native composition root and the public application API. It owns
process-wide components, coordinates operations that cross domain boundaries,
and translates domain failures into application results. Lower layers do not
depend on it.

| Source | Responsibility |
| --- | --- |
| `application.*` | Public application API, construction, ownership, shutdown, and small session operations. |
| `application_internal.h` | Private process-wide ownership shared by application operation files. |
| `application_config.*` | External application and vault configuration loading and validation. |
| `current_vault.h` | Immutable active-vault identity and paths. |
| `background_jobs.*` | Cancellation and bounded shutdown for application-owned background work. |
| `settings_operations.*` | Provider, credential, voice, and appearance settings operations. |
| `workspace_operations.*` | Workspace, character, forum, session-list, and configuration-file operations. |
| `vault_operations.*` | Vault lifecycle, maintenance, switching, merge, protection, and backup coordination. |
| `media_operations.*` | Application-facing speech, voice-input, and media-resource operations. |
| `r2_database_transfer.*` | R2 listing, upload, download, signing, validation, and local replacement. |

The application may depend on every domain directory. `bridge/` and native
hosts call into it; domain directories must not include application internals.

Tests live in `tests/app/`, with lower-level media, runtime, and provider tests
kept beside their corresponding source areas.
