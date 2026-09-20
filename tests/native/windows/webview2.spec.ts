import { expect, test, type Browser, type Page } from '@playwright/test';
import { chromium } from '@playwright/test';
import { spawn, spawnSync, type ChildProcess } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const cdpPort = Number(process.env.CHA_WEBVIEW2_CDP_PORT ?? '9222');
const executable = process.env.CHA_WEBVIEW2_EXECUTABLE;
const assets = process.env.CHA_NATIVE_ASSETS;
const prepareVault = process.env.CHA_PREPARE_TEST_VAULT;

let userData = '';

async function launchHost(): Promise<{ child: ChildProcess; browser: Browser }> {
  if (!executable || !assets || !prepareVault) {
    throw new Error(
      'CHA_WEBVIEW2_EXECUTABLE, CHA_NATIVE_ASSETS, and '
      + 'CHA_PREPARE_TEST_VAULT are required',
    );
  }
  userData = mkdtempSync(join(tmpdir(), 'cha-webview2-'));
  const config = join(userData, 'config');
  mkdirSync(config, { recursive: true });
  const prepared = spawnSync(prepareVault, [config], { stdio: 'inherit' });
  if (prepared.status !== 0) {
    throw new Error(`test vault preparation failed with exit code ${prepared.status}`);
  }
  const child = spawn(
    executable,
    [
      '--assets', assets,
      '--cdp-port', String(cdpPort),
      '--user-data', userData,
    ],
    { stdio: 'ignore' },
  );
  for (let attempt = 0; attempt < 80; attempt += 1) {
    try {
      const browser = await chromium.connectOverCDP(`http://127.0.0.1:${cdpPort}`);
      return { child, browser };
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  }
  child.kill();
  throw new Error('WebView2 CDP port did not become ready');
}

async function invoke(
  page: Page,
  method: string,
  params: Record<string, unknown>,
  contextEpoch: number,
): Promise<any> {
  return page.evaluate(({ callMethod, callParams, epoch }) => new Promise((resolve, reject) => {
    const nativeWindow = window as any;
    nativeWindow.__CHA_TEST_NEXT_REQUEST_ID__ ??= 9_000_000_000;
    const id = nativeWindow.__CHA_TEST_NEXT_REQUEST_ID__++;
    const previous = nativeWindow.__CHA_NATIVE_RECEIVE__;
    const timer = window.setTimeout(() => {
      nativeWindow.__CHA_NATIVE_RECEIVE__ = previous;
      reject(new Error(`${callMethod} timed out`));
    }, 15_000);
    nativeWindow.__CHA_NATIVE_RECEIVE__ = (batch: any) => {
      if (typeof previous === 'function') previous(batch);
      for (const message of batch?.messages ?? []) {
        if (message?.id !== id) continue;
        window.clearTimeout(timer);
        nativeWindow.__CHA_NATIVE_RECEIVE__ = previous;
        if (message.ok) resolve(message.result);
        else reject(new Error(message.error?.message ?? callMethod));
      }
    };
    nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({
      connection_id: nativeWindow.__CHA_NATIVE_CONNECTION_ID__,
      id,
      context_epoch: epoch,
      method: callMethod,
      params: callParams,
    }));
  }), { callMethod: method, callParams: params, epoch: contextEpoch });
}

async function waitForApplication(page: Page): Promise<void> {
  await page.waitForFunction(() => {
    const nativeWindow = window as any;
    return typeof nativeWindow.__CHA_NATIVE_POST__ === 'function'
      && typeof nativeWindow.__CHA_NATIVE_CONNECTION_ID__ === 'string'
      && document.querySelector('textarea[aria-label="Message"]');
  }, undefined, { timeout: 20_000 });
}

test.describe('WebView2 native host', () => {
  test.skip(
    !executable || !assets || !prepareVault,
    'Windows WebView2 host and native test-vault helper are not available',
  );

  let host: ChildProcess | undefined;
  let browser: Browser | undefined;
  let page: Page;

  test.beforeAll(async () => {
    const launched = await launchHost();
    host = launched.child;
    browser = launched.browser;
    const openedPage = browser.contexts()[0]?.pages()[0];
    expect(openedPage).toBeDefined();
    if (!openedPage) throw new Error('WebView2 did not expose an application page');
    page = openedPage;
  });

  test.afterAll(async () => {
    if (host && host.exitCode === null) {
      const exited = new Promise<void>((resolve) => host!.once('exit', () => resolve()));
      host.kill();
      await Promise.race([
        exited,
        new Promise<void>((resolve) => setTimeout(resolve, 2_000)),
      ]);
    }
    await browser?.close().catch(() => undefined);
    if (userData) {
      rmSync(userData, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
    }
  });

  test('runs the packaged application through the real native runtime', async () => {
    await waitForApplication(page);
    expect(await page.evaluate(() => window.location.origin)).toBe('https://app.cha.local');
    expect(await page.evaluate(() => window.isSecureContext)).toBe(true);

    const composer = page.locator('textarea[aria-label="Message"]');
    await expect(composer).toBeEnabled();
    await composer.fill('Hello from the Windows native host');
    await page.locator('button[aria-label="Send message"]').click();

    const transcript = page.locator('[aria-label="Conversation transcript"]');
    await expect(transcript).toContainText('Hello from the Windows native host');
    const stop = page.locator('button[aria-label="Stop generation"]');
    if (await stop.isVisible()) await stop.click();

    const firstConnection = await page.evaluate(
      () => (window as any).__CHA_NATIVE_CONNECTION_ID__ as string,
    );
    await page.reload();
    await waitForApplication(page);
    await expect(transcript).toContainText('Hello from the Windows native host');
    expect(await page.evaluate(
      () => (window as any).__CHA_NATIVE_CONNECTION_ID__ as string,
    )).not.toBe(firstConnection);

    const restored = await invoke(page, 'app.bootstrap', {}, 0);
    expect(restored.state).toBe('running');
    const originalVault = restored.bootstrap.vault_name as string;
    const copied = await invoke(page, 'vault.create', {
      display_name: 'Windows native copy',
      copy_from: originalVault,
      password: null,
    }, restored.context_epoch);
    const beforeSwitch = await page.evaluate(
      () => (window as any).__CHA_NATIVE_CONNECTION_ID__ as string,
    );
    await invoke(page, 'vault.switch', {
      vault_name: copied.display_name,
      password: null,
    }, restored.context_epoch).catch(() => undefined);
    await page.waitForFunction(
      (oldConnection) => (window as any).__CHA_NATIVE_CONNECTION_ID__ !== oldConnection,
      beforeSwitch,
      { timeout: 20_000 },
    );
    await waitForApplication(page);
    const switched = await invoke(page, 'app.bootstrap', {}, 0);
    expect(switched.bootstrap.vault_name).toBe(copied.display_name);

    const copiedConnection = await page.evaluate(
      () => (window as any).__CHA_NATIVE_CONNECTION_ID__ as string,
    );
    await invoke(page, 'vault.switch', {
      vault_name: originalVault,
      password: null,
    }, switched.context_epoch).catch(() => undefined);
    await page.waitForFunction(
      (oldConnection) => (window as any).__CHA_NATIVE_CONNECTION_ID__ !== oldConnection,
      copiedConnection,
      { timeout: 20_000 },
    );
    await waitForApplication(page);
    const finalBootstrap = await invoke(page, 'app.bootstrap', {}, 0);
    expect(finalBootstrap.bootstrap.vault_name).toBe(originalVault);
  });

  test('enforces CSP and retains session, file, navigation and vault behavior', async () => {
    test.setTimeout(60000);
    await waitForApplication(page);
    const source = readFileSync(join(__dirname, '..', 'parity.js'), 'utf8');
    const result = await page.evaluate(source + '\nnativeParity();');
    expect(result).toMatchObject({ok: true});
    await page.reload();
    await waitForApplication(page);
  });

  test('blocks Blob documents from replacing the privileged shell', async () => {
    await waitForApplication(page);
    const connection = await page.evaluate(() => (window as any).__CHA_NATIVE_CONNECTION_ID__);
    await page.evaluate(() => {
      const blob = new Blob(['<script>window.__BLOB_DOCUMENT__ = true;</script>'], {type: 'text/html'});
      const url = URL.createObjectURL(blob);
      location.assign(url);
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    });
    await page.waitForTimeout(500);
    expect(page.url()).toMatch(/^https:\/\/app\.cha\.local\//);
    expect(await page.evaluate(() => (window as any).__CHA_NATIVE_CONNECTION_ID__)).toBe(connection);
    expect(await page.evaluate(() => (window as any).__BLOB_DOCUMENT__)).toBeUndefined();
  });

  test('exits the assertion as failed when asked', async () => {
    await waitForApplication(page);
    const ok = await page.evaluate(() => 1 === 0);
    expect(ok).toBe(true);
  });
});
