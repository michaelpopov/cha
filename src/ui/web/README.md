# Web frontend boundary

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination. It may depend on `session/` presentation values but does
not put web types in `cha_core`. A future session runtime is the sole owner of a
`SessionController`; HTTP workers exchange only owning web values with it.

This initial block intentionally contains no routes, registry, live runtime, or
browser assets. `web_main.cpp` is only the composition root for the one server
listener.
