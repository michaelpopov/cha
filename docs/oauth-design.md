# OpenAI subscription support in CHA

Status: proposed design. This is a small personal-app feature.

## Approach

Use C++ for OpenAI login, token storage, renewal, and model requests. Use the
existing TypeScript UI to display a connection page. Both Linux `chaweb` and
the macOS application use the same device-code login flow.

There is one OpenAI account per CHA workspace. CHA obtains its own login and
stores its own credentials; Codex and Node do not need to be installed.
Characters using subscription providers share that connection. Existing
API-key providers continue to work, with no automatic switch between billing
methods.

Device login lets the user approve a code in a browser on any machine. It
avoids a localhost callback, which would be awkward when the browser and Linux
server are on different computers.

Use the device flow in the inspected Pi 0.85.1 implementation as the concrete
reference, including its Codex public client ID. The exchange is recorded in
[OAuth protocol notes](oauth-protocol.md); no client-registration investigation
or general OAuth library is needed. Port the small wire functions to C++, then
verify a fresh login, refresh, and streamed request. Pi is a reference, not a
runtime dependency.

OpenAI explicitly names Pi among developers' preferred tools in its
[Codex for Open Source program](https://developers.openai.com/community/codex-for-oss).
That is not a versioned specification for CHA's raw endpoints, so retain a
compatibility smoke test rather than a gate about official client registration.

## One small C++ component

Add `OpenAiOAuth` in `src/providers/openai_oauth.h/.cpp`. It owns:

- The current access token, refresh token, expiry, and account ID.
- One pending device login, when the user is signing in.
- The credential-file path.
- One mutex protecting all of this state.

The shared [ApplicationRuntime](../src/web/application_runtime.cpp) creates it
and supplies it to provider clients through the existing
`ProviderClientFactory`. Keep it alive until HTTP handlers and provider workers
have finished. `Providers` keeps its existing request supervision role.

Use libcurl through a small file-local blocking POST helper in
`openai_oauth.cpp`; leave the provider's private streaming wrapper alone. Reuse
JSON and private-file helpers. Authentication runs on existing HTTP/provider
workers. Hold the mutex through an exchange and its file write, with a
15-second overall network deadline including any follow-up exchange.

That deliberately allows connection controls or a request waiting for renewal
to pause briefly. It avoids a dedicated login worker, condition variables,
refresh ownership flags, and credential generation counters. Release the mutex
before streaming a model response, so ordinary generations still run
independently.

## Sign-in page and polling

Add an **OpenAI** page to the existing sidebar. It has three states:

- **Signed out:** show **Connect ChatGPT**.
- **Waiting:** show the verification link, code, and **Cancel**.
- **Connected:** show **Connected to ChatGPT** and **Disconnect**.

Errors use the existing error display. There are no separate recovery or
storage-error states.

Use these small root-scoped endpoints:

| Endpoint | Action |
| --- | --- |
| `GET /api/v1/openai/auth` | Read displayable connection state |
| `POST /api/v1/openai/auth/login` | Start a device login and return its code and link |
| `POST /api/v1/openai/auth/poll` | Check the active login once and finish it if approved |
| `POST /api/v1/openai/auth/disconnect` | Cancel a pending login or remove the connected account |

POST bodies are empty JSON objects. The pending login belongs to the workspace,
so there is no per-browser login session or public attempt ID. Starting again
while waiting returns the existing code. Connecting another account requires
disconnecting first.

The browser drives polling:

1. Click **Connect ChatGPT**. C++ requests a device code and returns the
   verification URL, code, expiry, and when to poll next.
2. Open the verification page and approve the code.
3. While the connection page is open, TypeScript calls `/poll` at the requested
   interval, waiting for each call to finish before scheduling another.
4. Each call performs at most one upstream poll. Approval returns an
   authorization code and verifier; C++ exchanges these for tokens and saves
   them before returning **Connected**.
5. Stop polling after success, error, cancellation, or leaving the page.

Keep the next allowed poll time on the server. Follow Pi's pending/slowdown
mapping and 15-minute login deadline from the protocol notes. Early polls
return state without calling OpenAI, so two tabs cannot double upstream polls.
Discard expired or failed attempts; the user can start again. Do not copy Pi's
long-running login loop into one blocking C++ HTTP request.

Closing the page pauses polling. Returning can resume the pending attempt if
it has not expired; restarting CHA requires starting that login again. All
browsers see and control the same connection. Independent tab coordination is
unnecessary for this application.

Because poll, disconnect, and file writes use the same mutex, disconnect runs
before or after an exchange rather than racing its completion. An already
running exchange may finish before Cancel takes effect.

Expose only the display state and pending code/link/timing. No email lookup or
ID-token storage is needed.
Tokens and private device polling credentials stay in C++. Preserve existing
JSON and same-origin checks and macOS's `CHA_RUNTIME` cookie protection. Send
`Cache-Control: no-store` on connection responses. Describe the routes in
`resources/cha.yaml` and regenerate the existing TypeScript API types.

## Local credential file

Derive the filename from the normalized workspace database path:

```text
<database path>.openai-auth.json
```

For example, `/srv/cha/cha.sqlite3.openai-auth.json`. On the Mac it normally
lives beside the database in `~/Library/Application Support/CHA/`. This keeps
it outside the replaceable application bundle and temporary workspace tree.

A plain JSON object is enough:

```json
{
  "access_token": "<secret>",
  "refresh_token": "<secret>",
  "expires_at": 1800000000,
  "account_id": "<account>"
}
```

Use Unix seconds for expiry, calculated from the response's `expires_in`.
Extract the account ID from the access-token payload as Pi does. Require a
complete token response on login and refresh; do not add speculative fallbacks
for omitted refresh tokens or alternate expiry fields.

Reuse [create_private_file()](../src/util/private_filesystem.cpp) for private
atomic replacement. Keep mode `0600` and the helper's checks for regular files.
Never log tokens or raw authentication responses. Keep the file out of SQLite,
configuration exports, R2 database transfers, and transcript mirrors.

Load it once when opening the runtime. A missing or invalid file leaves the
application signed out; an invalid file also produces a short diagnostic.
The existing database lease already prevents two CHA runtimes from owning the
same workspace. No additional lock file is needed.

On failed renewal or failure to save new credentials, clear the in-memory
login, attempt to remove the old file, and ask the user to connect again.
Report file-removal errors rather than claiming success; if removal failed,
a restart might reload that file. Fixing file permissions and signing in again
is an acceptable recovery procedure. Do not keep pending-write bundles,
backups, or an automatic repair system.

Each installation signs in separately. Transferring a database to another
machine does not transfer this credential file.

## Renewal and model requests

Resolve credentials in `ProviderClient::perform()`, where cancellation is
available. The normal path is:

1. Check cancellation and lock the authentication mutex.
2. Fail with a sign-in message if disconnected.
3. If the access token expires within five minutes, refresh it and save the
   complete returned bundle, including its refresh token.
4. Copy the access token and account ID into this request and unlock.
5. Check cancellation again, then send the model request normally.

Concurrent requests naturally wait for the same mutex. After the first
successful renewal, they use the fresh token. A failed renewal clears the
login, so waiting requests do not each repeat the failed exchange. Following
Pi's parser, require the returned refresh token and replace the complete bundle.

There is no background renewal timer. Once a refresh is sent, finish and save
its result even if the triggering generation was canceled; then honor that
cancellation. Accept that stopping during authentication may wait for its
timeout. Normal streaming retains its existing cancellation behavior.

An unexpected model `401` ends that request with a reconnect message. It does
not retry the request or change shared credentials: an older request must not
erase a newer login. Quota, network, and model errors likewise end the request
with an appropriate message. The user retries or reconnects explicitly.

Disconnect clears the pending login and stored credentials under the mutex.
Requests that already obtained an access token may finish. No automatic replay,
remote session revocation, or special disconnect protocol is required.

## Subscription provider

Add `auth = "openai_subscription"` to the existing provider format. Omission
keeps today's environment-key or anonymous-provider behavior.

```toml
auth = "openai_subscription"
host = "chatgpt.com"
port = 443
https = true
base_path = "/backend-api/codex"
mode = "net"
api = "responses"
model = "gpt-5.6-terra"
stream = true
web_search = "off"
cache_retention = "off"
```

The model is an example; verify the selected model's availability. Keep the
existing explicit connection fields rather than adding another configuration
format. Require this fixed HTTPS destination and Responses streaming, with no
`api_key_env`. For this first version, reject explicit temperature and
maximum-output-token settings, enabled web search, and non-off cache settings,
including effective character overrides. These are CHA scope limits, not a
claim that the backend universally rejects them; Pi implements more options.

Reuse the current transport and Responses decoder with a small subscription
branch:

- Send to `https://chatgpt.com/backend-api/codex/responses`; the ordinary
  `/v1/responses` suffix is wrong for this endpoint.
- Add Bearer/account headers and the Pi SSE compatibility headers recorded in
  the protocol notes, identifying the caller as CHA rather than Pi.
- Send CHA's full conversation, nonempty instructions, `stream: true`, and
  `store: false`; use a plain default instruction when CHA supplies none.
- Omit the deferred options and `max_output_tokens`. Verify this reduced request
  with the selected model before UI integration.

Do not port Pi's WebSockets, fallback/retry machinery, compression, model
catalog, or multi-provider credential framework. Keep CHA's existing atomic
private-file writes and sanitized errors rather than Pi's direct file writes
and raw response text in some error messages.

Keep TLS verification and send subscription credentials only to that fixed
destination. Continue using existing generation events, reasoning display, and
reported token counts. Model discovery, images, tools, quota dashboards, and
advanced caching can wait until there is a concrete need.

## Linux and macOS startup

Both entry points already use `ApplicationRuntime`, so the authentication
component and UI are shared.

For Linux, run `chaweb` as usual and connect through the browser. Nothing waits
on stdin or launches a browser on the server. The browser can be on another
computer because device login requires no incoming callback. Use the existing
private deployment arrangement, such as loopback with SSH forwarding.
Connecting OpenAI does not add browser authentication to `chaweb`.

For macOS, remove the mandatory API-key prompt before startup in
`prepareApplicationData()` and `importInitialDatabase(apiKey:)`. Start the
runtime and let the user connect through the common page. Preserve existing
API keys and **Change API Key…**. Open the verification link in the system
browser with `NSWorkspace`, keeping the `WKWebView` on CHA. No OAuth callback
or token-bearing Swift bridge is needed.

Also remove the environment-key availability check from workspace loading.
Missing credentials should fail when a provider is used, so the user can still
open the UI and connect. Keep structural configuration validation.

Add the subscription provider to the shared package seed. New seed characters
can use it; existing databases and provider selections stay untouched. Existing
users add/select it through the current configuration import and character
settings workflows.

Shutdown uses the existing HTTP and provider-worker shutdown. Keep the
authentication owner alive until those operations finish. There are no
additional authentication threads or timers to stop. Database maintenance
leaves the local connection alone.

## Implementation and checks

Follow the six blocks in the [implementation plan](oauth-plan.md): port the
small C++ owner and verify login/refresh, add and verify the provider request,
then wire the shared runtime, UI, and startup. Protocol discovery is already
captured; there is no separate research session or authentication framework.

Keep verification focused:

- With a mock service, test login pending/success/expiry and disconnect.
- Test that two requests near expiry perform one renewal and save the new
  refresh token; failed renewal leaves subsequent requests signed out.
- Test that serialized disconnect cannot be undone by an in-flight poll or
  refresh, and that cancellation does not discard a successful renewal.
- Check private-file writes and failures, token-free browser responses, and
  absence of credentials from database transfers.
- Try startup, login, generation, restart, and disconnect in Linux with a
  remote browser and in the packaged Mac app, without an API key or Codex.
- Confirm an existing API-key provider still works.

Occasional sign-in retries, brief waits during authentication, and manual
reconnection after failures are acceptable. Keeping those cases simple is more
valuable here than making every interrupted operation recover automatically.
