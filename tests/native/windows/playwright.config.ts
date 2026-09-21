import { defineConfig } from '@playwright/test';
import { resolve } from 'node:path';

export default defineConfig({
  testDir: '.',
  testMatch: /webview2\.spec\.ts/,
  retries: 0,
  workers: 1,
  reporter: 'list',
  timeout: 30000,
  webServer: process.env.CHA_NATIVE_DEV_ORIGIN ? {
    command: 'npm run dev:native',
    cwd: resolve(__dirname, '../../../webapp'),
    url: process.env.CHA_NATIVE_DEV_ORIGIN,
  } : undefined,
});
