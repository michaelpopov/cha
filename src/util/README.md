# Utilities

`util/` holds domain-neutral helpers. It must not know about transcripts,
agents, sessions, or UI policy.

| Source | Responsibility |
| --- | --- |
| `text.*` | Byte-oriented whitespace, trimming, and ASCII case folding. |
| `text_template.*` | Prompt includes, variables, scopes, containment, and resource limits. |
| `path_name.*` | Safe path-component and URL-identifier validation. |
| `utf8_path.*` | UTF-8 application text and native filesystem path conversion. |
| `environment.*` | Optional `.env` loading without overriding the process environment. |
| `logging.*` | Synchronous rotating diagnostic-file logging. |
| `concurrent_queue.h` | Portable typed thread-safe queue with reserved final delivery. |
| `thread_pool.*` | Fixed-size executor for session-scoped blocking work. |
| `wake_notifier.h` | Narrow producer-to-owner wake interface. |

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

`ThreadPool::submit()` is thread-safe. `stop()` closes admission, drains
accepted tasks, and joins workers. Domain callers own cancellation and convert
task failures at their own boundaries.

The utility layer depends only on the standard library, process environment,
spdlog, and toml++ where required. Libuv remains a core dependency because the
session database uses it for portable exclusive temporary-file creation.
