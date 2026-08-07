# Browser API boundary

Generated OpenAPI declarations and the hand-written HTTP client live here. The
client owns every JSON URL, request header, request body, success parse, and API
error conversion; browser components do not call `fetch` or construct API URLs.
`events.ts` provides the typed native `EventSource` boundary and reports only a
generic stream failure so later recovery can probe the ordinary snapshot API.
