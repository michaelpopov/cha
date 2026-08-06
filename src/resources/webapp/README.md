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

`npm run dev` serves the editable shell on `http://127.0.0.1:5173/` and proxies
API requests to a `chaweb` listening on `127.0.0.1:8080`. `npm run stage`
builds the browser files and safely replaces the repository's `bin/web/`.
Pass `--root <application-directory>` after `--` to stage another application
root.

The browser smoke suite starts a copied deterministic workspace and a real
`chaweb`; it never reads an API key or contacts a model provider. Install the
Playwright Chromium browser and its host libraries once on a development
machine with:

```sh
npx playwright install chromium
sudo npx playwright install-deps chromium
```
