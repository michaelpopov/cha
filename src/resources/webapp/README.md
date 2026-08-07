# CHA browser application

The browser source is built with the pinned Node.js version in `.node-version`.
Dependencies and browsers are development tools only; the customer receives
the generated files under `web/`.

From this directory:

```sh
npm ci
npm run api-types
npm run check
npm run build
npm run stage
npm run e2e
```

`npm run stage` builds the browser files and safely replaces the repository's
`bin/web/`. Pass `--root <application-directory>` after `--` to stage another
application root. `chaweb` reads `web/index.html` once at startup, so a staged
shell reaches the browser only after the server is restarted; the hashed asset
names under `assets/` make that the correct behavior for a release, but it does
mean `npm run stage` alone will not refresh a running server.

## Two development loops, two ports

There are two ways to run the application while working on it, and they
deliberately listen on different ports so that neither blocks the other.

| Loop | Command | Serves | Port |
| --- | --- | --- | --- |
| Staged | `make run-web` | `bin/web/`, exactly as a release does | 8888 |
| Editable | `make run-web-dev` plus `npm run dev` | Vite, with hot reloading | 8080 and 5173 |

`npm run dev` serves the editable shell on `http://127.0.0.1:5173/` and proxies
API requests to a `chaweb` listening on `127.0.0.1:8080`; it does not start that
server. `make run-web-dev` is the one that starts it, on the port the proxy
expects. Browse the editable shell at `http://127.0.0.1:5173/`, not at 8080.

The two loops keep separate ports so that a staged server and an editable one
can run at the same time. The browser suite is independent of both: it chooses
its own ports, so neither loop needs to be stopped before `npm run e2e`.

## Browser tests

`npm run e2e` builds the browser files first, then runs two Playwright
projects against one deterministic `chaweb`. It picks its own pair of ports
rather than taking a fixed one, so a staged server, a container, or a previous
run that has not exited yet cannot make the suite fail; set `CHA_E2E_PORT` to
pin the API port deliberately. The two projects are:

- `chromium` drives the application through the development server, which is
  where the API proxy, `Host`/`Origin` rewriting, and hot reloading are proved.
- `served` loads the production build straight from `chaweb`, with no
  development server in the path. It is what proves the shipped application
  runs: the built bundle, the asset routes, the cache headers, and the Content
  Security Policy together. A policy that blocked the bundle would show up here
  as a console error rather than as a blank page in front of a customer.

The suite starts a copied deterministic workspace and a real
`chaweb`; it never reads an API key or contacts a model provider. Install the
Playwright Chromium browser and its host libraries once on a development
machine with:

```sh
npx playwright install chromium
sudo npx playwright install-deps chromium
```
