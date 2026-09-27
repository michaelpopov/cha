# Web API boundary

Generated DTO declarations and the hand-written application client live here.
`nativeClient.ts` maps the host request/reply bridge onto `ChaClient`, while
components only depend on that typed interface. `nativeEvents.ts` performs the
same job for scoped session subscriptions. Browser code fetches only temporary
local media resource URLs issued by the host, including growing audio streams.

Voice input exposes OpenAI connection setup and the scoped xAI
start/append/stop/cancel lifecycle. Session APIs include safe unused-session
discard and error deletion. Settings include Jev, Search API, hands-free voice
input, and voice output. DTOs come from `resources/dto.yaml`; run
`npm run api-types` from `webapp/` after changing that schema, and keep wire
fixtures and native method policies in sync.
