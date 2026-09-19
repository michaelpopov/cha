import { expect, test } from '@playwright/test';
import { chromium } from '@playwright/test';
import { spawn, type ChildProcess } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const cdpPort = Number(process.env.CHA_WEBVIEW2_CDP_PORT ?? '9222');
const executable = process.env.CHA_WEBVIEW2_EXECUTABLE;
const assets = process.env.CHA_NATIVE_ASSETS;

async function launchHost(): Promise<ChildProcess> {
  if (!executable || !assets) {
    throw new Error('CHA_WEBVIEW2_EXECUTABLE and CHA_NATIVE_ASSETS are required');
  }
  const userData = mkdtempSync(join(tmpdir(), 'cha-webview2-'));
  const child = spawn(
    executable,
    [
      '--feasibility',
      '--assets', assets,
      '--cdp-port', String(cdpPort),
      '--user-data', userData,
    ],
    { stdio: 'ignore' },
  );
  for (let attempt = 0; attempt < 40; attempt += 1) {
    try {
      const browser = await chromium.connectOverCDP(`http://127.0.0.1:${cdpPort}`);
      await browser.close();
      return child;
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  }
  child.kill();
  throw new Error('WebView2 CDP port did not become ready');
}

test.describe('WebView2 native host', () => {
  test.skip(!executable || !assets, 'Windows WebView2 host is not available');

  let host: ChildProcess | undefined;

  test.beforeAll(async () => {
    host = await launchHost();
  });

  test.afterAll(() => {
    host?.kill();
  });

  test('connects over CDP and proves a passing assertion', async () => {
    const browser = await chromium.connectOverCDP(`http://127.0.0.1:${cdpPort}`);
    const page = browser.contexts()[0]?.pages()[0];
    expect(page).toBeDefined();
    const origin = await page!.evaluate(() => window.location.origin);
    expect(origin).toBe('https://app.cha.local');
    const secure = await page!.evaluate(() => window.isSecureContext);
    expect(secure).toBe(true);
    await browser.close();
  });

  test('exits the assertion as failed when asked', async () => {
    const browser = await chromium.connectOverCDP(`http://127.0.0.1:${cdpPort}`);
    const page = browser.contexts()[0]?.pages()[0];
    expect(page).toBeDefined();
    const ok = await page!.evaluate(() => 1 === 0);
    expect(ok).toBe(true);
    await browser.close();
  });
});
