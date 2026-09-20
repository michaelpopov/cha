# Web API boundary

Generated DTO declarations and the hand-written application client live here.
`nativeClient.ts` maps the host request/reply bridge onto `ChaClient`, while
components only depend on that typed interface. `nativeEvents.ts` performs the
same job for scoped session subscriptions. Browser code fetches only temporary
local media resource URLs issued by the host.
