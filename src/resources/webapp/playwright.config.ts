import { defineConfig, devices } from '@playwright/test';

// Both ports are chosen here rather than written into the suite, because a
// fixed port makes the whole suite fail whenever anything else on the machine
// happens to hold it — a staged server, a colleague's container, a previous run
// that has not exited yet. The default derives from the process, so concurrent
// runs do not collide either; set CHA_E2E_PORT to pin one deliberately.
// Chosen once and published to the environment: this file is re-evaluated in
// every worker, and a port derived per process would differ between the worker
// and the server the runner started. Workers inherit what the runner set.
process.env.CHA_E2E_PORT ??= String(20000 + (process.pid % 20000));

const apiPort = Number(process.env.CHA_E2E_PORT);
const devPort = apiPort + 1;
const api = `http://127.0.0.1:${apiPort}`;
const dev = `http://127.0.0.1:${devPort}`;

process.env.CHA_API_TARGET ??= api;

export default defineConfig({
  testDir: './e2e',
  outputDir: './test-results',
  // Every project shares the one deterministic chaweb process below, and a
  // live session intentionally permits one event stream. Serial workers keep
  // unrelated root-page tests from becoming accidental second viewers of the
  // built-in Welcome session; the dedicated two-page test covers that case.
  workers: 1,
  fullyParallel: false,
  retries: 0,
  reporter: 'list',
  use: {
    baseURL: dev,
    trace: 'retain-on-failure',
  },
  projects: [
    // Run the direct production-shell checks before development-server tests
    // begin opening live sessions through the proxy.
    {
      name: 'served',
      // The production server runs the complete customer flow, not only the
      // three header checks. CHA_E2E_APPLICATION_ROOT makes this the assembled
      // package verification used by the Linux packaging command.
      testMatch: /.*\.spec\.ts/,
      use: { ...devices['Desktop Chrome'], baseURL: api },
    },
    // Through the development server, which proxies the API to chaweb.
    {
      name: 'chromium',
      testIgnore: /served\.spec\.ts/,
      use: { ...devices['Desktop Chrome'] },
    },
  ],
  webServer: [
    {
      command: 'node e2e/start-cha.mjs',
      url: `${api}/health`,
      reuseExistingServer: false,
      timeout: 30_000,
    },
    {
      // The proxy target has to be the server started above, not the port a
      // developer's own `npm run dev` would assume.
      command: `npm run dev -- --port ${devPort}`,
      url: `${dev}/`,
      reuseExistingServer: false,
      timeout: 30_000,
    },
  ],
});
