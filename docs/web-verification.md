# chaweb verification record

`web-design.md` is authoritative. This file records how its Section 20 testing
strategy is discharged: which bullets an automated test covers, which are
verified another way, and which platforms and sanitizers were actually
exercised. Nothing here claims coverage that was not run.

## Suites

The ordinary suite excludes the socket/process and long-concurrency groups, so
a full check is three commands:

```bash
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure -LE "web_process|web_stress"
ctest --test-dir build/console --output-on-failure -L web_process
ctest --test-dir build/console --output-on-failure -L web_stress
```

`web_process` binds real sockets and spawns the built `chaweb`; `web_stress`
runs the long concurrency cases. Both are separated from the fast suite on
purpose rather than folded into it.

## Design Section 20 coverage

| Design section | Automated by | Tests |
| --- | --- | --- |
| 20.1 Session lease | `tests/session/unit_session_lease.cpp`, `tests/integration/console_process_test.cpp`, `tests/support/lease_test_helper.cpp` | 7 unit, plus `ConsoleProcess.ReportsSessionLeaseContentionClearly` |
| 20.2 Session registry | `tests/ui/web/unit_session_registry.cpp` | 15 |
| 20.3 Session runtime | `tests/ui/web/unit_web_session_runtime.cpp`, `unit_sse_mailbox.cpp`, `unit_sse_stream.cpp`, `unit_browser_connection_state.cpp` | 53 |
| 20.4 Concurrency and containment | `tests/session/unit_concurrent_controllers.cpp`, `tests/ui/web/stress_web_sessions.cpp` | 4 + 5 (`web_stress`) |
| 20.5 HTTP/SSE contract | `tests/ui/web/unit_lobby_routes.cpp`, `unit_session_routes.cpp`, `unit_protocol.cpp`, `process_web_server.cpp` | 40 + socket-limit cases |
| 20.6 Server lifecycle | `tests/ui/web/process_web_server.cpp` | 9 (`web_process`) |
| 20.7 Browser lifecycle | **Not automated.** | — |

Section 20.7 describes behavior of a browser page. Section 18 defers the
browser implementation to a separate design and plan, so there is no page to
drive and no automated coverage to claim. The server-side halves of those
bullets — same-origin open paths, the not-open page, `browser_stream_in_use`,
snapshot-on-every-stream, target and `seq` continuity, absence of `id:` fields
and of `/api/v1/close` — are covered under 20.5. What remains unverified is the
page's own conduct, and it stays that way until the browser block runs.

The 20.4 bullet "sanitizer builds cover concurrent sessions under load" is
discharged by running the `web_stress` group under both sanitizer presets; see
below.

## Sanitizers

Both presets are opt-in and require a GNU or Clang-family toolchain.

```bash
cmake --preset console-asan-ubsan
cmake --build --preset console-asan-ubsan
ctest --test-dir build/console-asan-ubsan --output-on-failure -LE "web_process|web_stress"
ctest --test-dir build/console-asan-ubsan --output-on-failure -L web_process
ctest --test-dir build/console-asan-ubsan --output-on-failure -L web_stress

cmake --preset console-tsan
cmake --build --preset console-tsan
ctest --test-dir build/console-tsan --output-on-failure -LE "web_process|web_stress"
ctest --test-dir build/console-tsan --output-on-failure -L web_process
ctest --test-dir build/console-tsan --output-on-failure -L web_stress
```

Instrumentation is applied before the dependencies are fetched, so libuv, the
HTTP transport, spdlog, and the SQLite amalgamation are instrumented too.
ThreadSanitizer reports false races against synchronization it cannot see, so a
partially instrumented build would be worse than none.

Two deliberate differences apply to instrumented builds only:

- The cpp-httplib non-blocking resolver is disabled
  (`HTTPLIB_USE_NON_BLOCKING_GETADDRINFO=OFF`). It calls glibc `getaddrinfo_a`,
  which resolves on threads created inside libanl rather than through
  `pthread_create`; ThreadSanitizer cannot intercept those and every test that
  issues an HTTP request dies before running. `chaweb` resolves one bind
  address at startup and no production path uses the cpp-httplib client, so
  the blocking resolver is equivalent here.
- `tests/ui/web/process_web_server.cpp` scales its absolute socket timings.
  The ratios each test asserts on are unchanged, and the bounds stay below
  cpp-httplib's 5s default write timeout, which is what those tests exist to
  distinguish the configured timeout from.

`tests/support/sanitizer_suppressions.cpp` is compiled into every test binary
and is empty unless the build is instrumented. It suppresses one library race:
libstdc++ fills the `std::ctype<char>` narrow/widen cache lazily and without
synchronization, which any test making concurrent HTTP requests hits. Nothing
in that file can hide a race in cha's own code.

## What was exercised

| Check | Result |
| --- | --- |
| Linux `console`, all three groups | 443 / 9 / 5 pass |
| Linux `console-asan-ubsan`, all three groups | 443 / 9 / 5 pass |
| Linux `console-tsan`, all three groups | 443 / 9 / 5 pass |
| macOS build and tests | **Not run** — no runner available |
| Windows build and tests | **Not run** — no runner available |
| MemorySanitizer | **Not run** — needs an instrumented libc++ |

Recorded environment: Linux 6.17.0 aarch64, GCC 15.2.0, CMake 3.31.6.

The native companion-file lease backend is exercised by the portable unit tests
on every supported platform, and the POSIX process harness adds cross-process
crash-release coverage. The Windows path of that backend has portable
lifecycle coverage but has not been run on Windows.
