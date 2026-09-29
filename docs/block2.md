# Block 2: ChaWeb nginx integration

Status: complete. See "Handoff to block 3" at the end.
This block implements plan step 5 and the API integration portion of step 9.
It requires the six-operation daemon API from block 1, described below. It does
not require a browser build. Block 3 later adds production-asset checks and the
frontend dependency to `make itest-daemon`.

This document contains the requirements for this block. Background:
[chaweb.md](chaweb.md) and [chaweb-plan.md](chaweb-plan.md).

## Result and files

Deliver a shipped nginx example and automated tests proving that the text API
works through nginx, SCGI, and a Unix daemon socket. Each production user gets
one private HTTPS listening port and a fixed daemon socket. All ports serve the
same static application. Port selection is routing, not authentication: anyone
who can reach a user's port can use that user's conversations.

| File | Work |
| --- | --- |
| `packaging/linux/nginx-chaweb.conf.example` | Add the separate ChaWeb server block. |
| `tests/integration/daemon_integration_test.py` | Add a ChaWeb fixture and API cases using the existing harness. |
| `tests/native/prepare_test_vault.cpp` | Reuse the `cha_prepare_test_vault` helper; extend only if a required fixture needs it. |
| `scripts/run_daemon.py` | Reuse for local inherited-socket activation; no new launcher. |

Keep existing `nginx.conf.example`, `nginx.conf.install`, bearer-key maps, and
OpenAI integration cases unchanged. Do not add a second test runner, TCP port
allocator, Playwright suite, or `itest-chaweb` target.

## API available from block 1

Base: `/api/cha/v1`. `S` below means
`/forums/{forum_id}/sessions/{session_id}` under that base.

| Request | Body | Success |
| --- | --- | --- |
| `GET /bootstrap` | None | `200`, existing `Bootstrap` directly. |
| `GET /forums/{forum_id}/sessions` | None | `200`, `SessionListing[]`. |
| `POST /forums/{forum_id}/sessions` | `{ "text": "..." }` | `201`, `{ "id": "...", "label": "..." }`. |
| `GET S` | None | `200`, existing `SessionSnapshot`. |
| `POST S/input` | `{ "text": "..." }` | `204`, empty body. |
| `POST S/stop` | `{}` | `204`, empty body. |

All POSTs require `Content-Type: application/json`; missing/unsupported types
return `415` before application operations. Unknown method/path pairs, including
`OPTIONS`, return `404` without CORS grants. Creation validates before storing,
accepts raw first input, and deletes the new session on definitive rejection.
Every named-session operation opens the session first, including Stop.

Errors use `{"error":{"code":"...","message":"..."}}`. Adapter statuses
are `400` invalid request/prompt size, `404` unknown route/resource, `415` media
type, `422 invalid_argument` rejected input, `500` other failure, and `503`
shutdown. The global SCGI 16 MiB bound can return `413`. A timeout or lost write
response has an unknown outcome and must not cause automatic input replay.

The daemon handles requests serially. Input returns after acceptance; existing
workers continue generation. Snapshots return the full transcript and
`generation.active`; there is no subscription. Request closure must not stop
accepted ChaWeb generation. A stored session and its IDs survive daemon restart.

## nginx example

Ship this server-block structure for inclusion in the nginx `http` context.
The enclosing configuration must include nginx's `mime.types`. Replace private
address, hostname, static root, and TLS paths during deployment. Duplicate the
block with another port and a literal different socket for another user.

```nginx
server {
    listen 192.168.1.10:8443 ssl;
    server_name cha.example.test;
    # Set ssl_certificate and ssl_certificate_key for this deployment.
    root /srv/cha/chaweb;

    location = / {
        add_header Cache-Control "no-cache";
        try_files /index.html =404;
    }
    location = /index.html {
        add_header Cache-Control "no-cache";
    }
    location /assets/ {
        add_header Cache-Control "public, max-age=31536000, immutable";
        try_files $uri =404;
    }
    location /api/cha/v1/ {
        add_header Cache-Control "no-store" always;
        client_max_body_size      256k;
        include                   scgi_params;
        scgi_pass                 unix:/run/cha/alice.sock;
        scgi_pass_request_headers off;
        scgi_request_buffering    on;
        scgi_buffering            on;
        scgi_cache                off;
        scgi_ignore_client_abort  off;
        scgi_read_timeout         75s;
        scgi_send_timeout         30s;
        gzip                      on;
        gzip_types                application/json;
        gzip_vary                 on;
    }
    location / {
        try_files $uri =404;
    }
}
```

The standard `scgi_params` already sends `CONTENT_TYPE`, even with request-header
forwarding disabled. Do not add `CHA_CONTEXT`, user IDs, bearer routing, or
custom header plumbing. Do not grant CORS or special-case `OPTIONS` in nginx.
Let the daemon return its finite API error. `/v1/` is absent on these listeners.

Keep both buffering directions enabled. nginx's timeouts also bound time spent
waiting behind another daemon request; they do not require daemon I/O deadlines.
JSON is explicitly included for gzip; test with `Accept-Encoding: gzip`. If a
deployment has an outer proxy adding `Via`, document the need to configure
`gzip_proxied` there. API cache headers apply to successes and errors, and stay
out of shared `write_cgi()` and OpenAI responses.

## Extend the existing harness

Use `tests/integration/daemon_integration_test.py`, which already supplies
`UnixHTTPConnection`, `FakeProvider`, temporary paths, process logs, cleanup,
test-vault creation, and `scripts/run_daemon.py`. Reuse these small helpers
without turning the test file into a general test framework.

Each new ChaWeb fixture runs one daemon, one isolated vault, and one nginx
HTTP listener on a Unix socket. The fake provider can continue using its
existing localhost TCP socket. Keep existing OpenAI fixtures and their
multi-user coverage intact; no new two-user routing fixture is needed.

Render the actual shipped ChaWeb example with test substitutions for the
listener, daemon socket, root, and TLS. Replace the HTTPS listener with a Unix
HTTP listener and use a temporary static directory, which can remain empty in
this block. Include real nginx `scgi_params` and `mime.types`; locate the latter
beside the nginx configuration or accept an explicit path if necessary. Keep
all nginx PID, log, body, and SCGI temporary paths inside the test directory.
Validate the rendered configuration with `nginx -t` before starting it.

Use bootstrap for readiness and derive forum/session IDs from responses. Send
no bearer key in ChaWeb requests. Extend request helpers only as needed to
control media types and inspect status, headers, and raw bodies. Do not force
JSON parsing for `204`, gzip bytes, or nginx error pages.

`FakeProvider` records prompts and supports holding/releasing replies and
provider failures. Use its synchronization to test active generation and Stop;
avoid timing assertions based on a fast provider or arbitrary long sleeps.
Ensure cleanup releases held replies and stops every child process after
failures. Keep readiness and completion waits bounded and report process logs.

## Required integration cases

| Case | Evidence |
| --- | --- |
| Complete API path | Bootstrap/list/create/snapshot/input/Stop work through the shipped nginx configuration and real SCGI daemon; later input and Stop have bodyless `204` responses. |
| First-input lifecycle | Creation returns `201` after acceptance while a held reply remains active; rejected first input returns `422` and leaves no stored row; self-notes keep their sessions. |
| Continuation and Stop | Snapshots show progress and final state; Stop cancels a held reply; loading an older/retired session works; idle Stop succeeds and unknown Stop returns `404`. |
| Request lifetime | An accepted turn survives completion or loss of its ChaWeb request; the next read finds it. Do not retry a mutation to recover a lost response. |
| Persistence | Restart the daemon with the same test vault, then reopen the same session ID with its history. |
| Content-type regression | Send valid creation JSON as `text/plain`; assert `415`, unchanged session list, and no provider request. Include missing type and unsupported preflight coverage. |
| Validation | Malformed JSON, unknown fields, invalid method/path pairs, missing/mismatched identities, and oversized prompts return the documented statuses without unwanted mutations. |
| nginx behavior | API successes and errors have `Cache-Control: no-store`; the 256 KiB bound returns `413`; an unavailable socket gives a gateway error; sufficiently large JSON is gzip-compressed and decompresses to a valid snapshot. |

Focused media-type variations and unusual cleanup failures remain covered by
block 1's unit tests. Browser stale responses and automatic-replay prevention
belong to block 4. Real static-file status/MIME/cache checks are added in block
3, once production assets exist. Do not fake assets to declare those checks done.

## Local browser-development handoff

Provide a usable local HTTP nginx listener for the next block's Vite proxy.
Use the shipped example with `listen 127.0.0.1:8087` without `ssl`, an absolute
path to `webapp/dist-chaweb` as root, and a fixed development daemon socket.
The root can be absent until block 3. Use an isolated nginx prefix/configuration
and process, not changes to an installed personal service.

A development daemon can be started in a terminal with:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha-daemon cha_prepare_test_vault
build/ninja/cha_prepare_test_vault /tmp/chaweb-dev-config
python3 scripts/run_daemon.py /tmp/chaweb-dev.sock build/ninja/cha-daemon --config /tmp/chaweb-dev-config
```

Choose unused temporary paths. Set the local nginx upstream to
`unix:/tmp/chaweb-dev.sock`. Put the server block inside a complete nginx
configuration with `events {}` and `http {}`, absolute include paths, and
writable PID/log/temp paths. Check and run it with:

```sh
nginx -t -p /absolute/dev-nginx/ -c /absolute/dev-nginx/nginx.conf
nginx -p /absolute/dev-nginx/ -c /absolute/dev-nginx/nginx.conf -g 'daemon off;'
```

The prepared vault is enough for navigation and self-notes. For real replies,
use an isolated vault with working provider settings or the existing fake
provider configured through `cha_prepare_test_vault --provider-port PORT`.
Do not open a vault simultaneously in the native app and daemon.

## Verification and completion

Prerequisites are the normal C++ build tools, Python 3, and nginx on PATH with
its parameter/MIME files. Run from the repository root:

```sh
make test
make itest-daemon
```

For a nonstandard nginx installation, use the existing Python arguments
`--nginx` and `--scgi-params` after building the targets. Keep
`make itest-daemon` free of a ChaWeb frontend-build dependency until block 3.

Completion requires passing new ChaWeb API cases and existing OpenAI cases,
plus a working local listener for browser development. Record its URL and
configuration/socket paths for the next block. Check another user's production
port manually during deployment; do not automate nginx's fixed-port routing
with extra daemons or request-header/body redirection tests.

## Handoff to block 3

The nginx example and ChaWeb integration cases are done. `make test` and
`make itest-daemon` pass. The local development listener was checked by hand:

- URL: `http://127.0.0.1:8087` (API at `/api/cha/v1/`; `/v1/` returns `404`).
- nginx prefix and configuration: `/tmp/chaweb-dev-nginx/nginx.conf`.
  It is the shipped example with `listen 127.0.0.1:8087;`, root
  `/Users/mpopov/projects/cha/webapp/dist-chaweb` and upstream
  `unix:/tmp/chaweb-dev.sock`.
- Daemon socket: `/tmp/chaweb-dev.sock`. Vault: `/tmp/chaweb-dev-config`.
- Both `/tmp` paths are temporary; make them again with the commands above
  if they are lost.

COMPLETED
