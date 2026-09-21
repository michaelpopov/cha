# Utilities

`util/` holds domain-neutral helpers. It must not know about transcripts,
agents, sessions, or UI policy.

| Source | Responsibility |
| --- | --- |
| `text.*` | Byte-oriented whitespace, trimming, and ASCII case folding. |
| `text_source.*` | Immutable text-file maps and filesystem-backed text reads. |
| `text_template.*` | Prompt includes, variables, scopes, containment, and resource limits. |
| `path_name.*` | UTF-8 path conversion, safe path-component, and URL-identifier validation. |
| `public_name.*` | Validation for user-visible single-line names and descriptions. |
| `json_serialization.h` | JSON dumping with caller-supplied invalid-UTF-8 diagnostic context. |
| `private_filesystem.*` | Owner-private directories/files and validated regular-file replacement for databases, secrets, and materialization. |
| `crypto.*` | SHA-256 and HMAC-SHA256 helpers. |
| `curl.*` | RAII wrappers for curl handles and header lists. |
| `logging.*` | Synchronous rotating diagnostic-file logging. |
| `toml_file.*` | Read and atomic mutation helpers for TOML files. |
| `concurrent_queue.h` | Portable typed thread-safe queue with reserved final delivery. |
| `wake_notifier.h` | Narrow producer-to-owner wake interface. |
| `owner_wake_signal.*` | Portable coalescing wake source for one owner loop. |

`require_path_component()` protects workspace-controlled path components.
Forum and session IDs use the stricter URL-safe identifier rule. Prompt
templates additionally canonicalize included files and require them to remain
inside the containment root supplied by the caller.

`expand_template_file()` expands `$$(relative/path)` includes and
`$${variable}` substitutions. It rejects malformed macros, cycles, containment
escapes, non-scalar scope values, and resource-limit violations, and reports
file/include-chain context.

`ConcurrentQueue<T>` drains accepted values after close. `close_with(value)`
adds one allocation-independent final value before consumers observe closure.
Notification is separate from queue storage: producers use an injected
`WakeNotifier` only when their owner loop requires one.

The utility layer depends only on the standard library, nlohmann-json, spdlog,
and toml++ where required. Libuv remains a core dependency because the
unified database uses it for portable exclusive temporary-file creation.
