import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: '.',
  testMatch: /webview2\.spec\.ts/,
  retries: 0,
  workers: 1,
  reporter: 'list',
  timeout: 30000,
});
