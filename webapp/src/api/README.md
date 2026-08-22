# Browser API boundary

Generated OpenAPI declarations and the hand-written HTTP client live here. The
client owns every JSON URL, request header, request body, success parse, and API
error conversion; browser components do not call `fetch` or construct API URLs.
Stored-session creation, rename, recoverable deletion, and open are all exposed
through this boundary; deletion treats its successful `204` as an empty response.
`events.ts` provides the typed native `EventSource` boundary. It reports a
generic stream failure so later recovery can probe the ordinary snapshot API,
and separately reports the `superseded` record the server writes when the
reader opened this session on another device, which parks the page instead of
reconnecting.
