# Web frontend boundary

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination. It may depend on `session/` presentation values but does
not put web types in `cha_core`. A future session runtime is the sole owner of a
`SessionController`; HTTP workers exchange only owning web values with it.

`WebSessionRuntime` owns one permanent thread, a condition-variable wake
notifier, and a bounded multi-producer command queue. HTTP-facing callers get
only owning command results; the owner thread alone reaches a controller and
continues draining agent notifications without a browser connection. Routes,
the registry, snapshots, and SSE writing remain later blocks. `web_main.cpp`
is only the composition root for the one server listener.
