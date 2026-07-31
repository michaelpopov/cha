# Web frontend boundary

`cha_web` owns HTTP/SSE transport, web protocol values, serialization, and web
runtime coordination. It may depend on `session/` presentation values but does
not put web types in `cha_core`. A future session runtime is the sole owner of a
`SessionController`; HTTP workers exchange only owning web values with it.

`WebSessionRuntime` owns one permanent thread, a condition-variable wake
notifier, and a bounded multi-producer command queue. HTTP-facing callers get
only owning command results; the owner thread alone reaches a controller and
continues draining agent notifications without a browser connection. It copies
controller state into owning, presentation-neutral snapshots and publishes
them through a small abstract sink, so a future SSE writer never borrows
controller state or blocks the owner. Append candidates cross that seam without
a sequence number; the future mailbox assigns sequence values only to payloads
it actually stores. Its idempotent owner-thread teardown
uses registry hooks only for lifecycle notifications, drains a final snapshot
for a bounded interval, and contains controller failures to that runtime.
`SessionRegistry` is the sole process-local liveness authority. It serializes
open requests by validated forum/session identity, counts starting and stopping
entries against the configured bound, and owns the owner threads. It publishes
only running runtimes through owning `SessionHandle` values, and sweeps finished
entries in two phases so joins and runtime destruction occur outside its mutex.
Routes and actual SSE writing remain later blocks.
`web_main.cpp` is only the composition root for the one server listener.
