import { expect, test, type Browser, type Page } from '@playwright/test';
import { chromium } from '@playwright/test';
import { spawn, spawnSync, type ChildProcess } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';

const cdpPort = Number(process.env.CHA_WEBVIEW2_CDP_PORT ?? '9222');
const executable = process.env.CHA_WEBVIEW2_EXECUTABLE;
const assets = process.env.CHA_NATIVE_ASSETS;
const prepareVault = process.env.CHA_PREPARE_TEST_VAULT;
const devOrigin = process.env.CHA_NATIVE_DEV_ORIGIN;

let userData = '';

async function historyAction(page: Page, name: 'Undo' | 'Redo'): Promise<void> {
  await page.getByRole('button', { name: 'History', exact: true }).click();
  await page.getByRole('menuitem', { name, exact: true }).click();
}

function removeTestData(root: string): void {
  if (dirname(resolve(root)) !== resolve(tmpdir()) || !basename(root).startsWith('cha-webview2-')) {
    throw new Error('Refusing to remove a directory outside the test temp root');
  }
  // WebView2 subprocesses can briefly outlive the host and lock their profile.
  // A temp-cleanup failure must not mask an assertion or fail a passing probe.
  try {
    rmSync(root, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 });
  } catch {
    // Left for the system temp cleaner.
  }
}

async function launchHost(existingRoot?: string, assetRoot = assets): Promise<{ child: ChildProcess; browser: Browser }> {
  if (!executable || !assets || !prepareVault) {
    throw new Error(
      'CHA_WEBVIEW2_EXECUTABLE, CHA_NATIVE_ASSETS, and '
      + 'CHA_PREPARE_TEST_VAULT are required',
    );
  }
  userData = existingRoot ?? mkdtempSync(join(tmpdir(), 'cha-webview2-'));
  if (!existingRoot) {
    const config = join(userData, 'config');
    mkdirSync(config, { recursive: true });
    const prepared = spawnSync(prepareVault, [config], { stdio: 'inherit' });
    if (prepared.status !== 0) {
      throw new Error(`test vault preparation failed with exit code ${prepared.status}`);
    }
  }
  const child = spawn(
    executable,
    [
      '--assets', assetRoot!,
      '--cdp-port', String(cdpPort),
      '--user-data', userData,
      ...(devOrigin ? ['--dev-origin', devOrigin] : []),
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

async function newNote(page: Page, name: string): Promise<void> {
  await waitForApplication(page);
  await page.getByRole('tab', { name: 'Notes', exact: true }).click();
  await page.getByRole('treeitem').first().getByRole('button').click();
  await page.getByRole('menuitem', { name: 'New note', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'New note', exact: true });
  await dialog.getByLabel('Name', { exact: true }).fill(name);
  await dialog.getByRole('button', { name: 'Create', exact: true }).click();
  await expect(page.locator('.tiptap[contenteditable="true"]')).toBeVisible();
}

async function pasteNote(page: Page, html: string, text = ''): Promise<void> {
  await page.locator('.tiptap').evaluate((element, content) => {
    const data = new DataTransfer();
    data.setData('text/html', content.html);
    data.setData('text/plain', content.text);
    element.dispatchEvent(new ClipboardEvent('paste', { clipboardData: data, bubbles: true }));
  }, { html, text });
}

async function noteJSON(page: Page): Promise<any> {
  return page.locator('.tiptap').evaluate((element) => (element as any).chaEditor.getJSON());
}

// Withhold a real host reply, rather than adding a production delay setting.
// Restore it through the same receiver so the admitted request settles normally.
async function holdReply(page: Page, method: string, failFirst = false): Promise<void> {
  await page.evaluate(({ heldMethod, fail }) => {
    const nativeWindow = window as any;
    const post = nativeWindow.chrome.webview.postMessage.bind(nativeWindow.chrome.webview);
    const receive = nativeWindow.__CHA_NATIVE_RECEIVE__;
    const ids = new Set<number>();
    nativeWindow.__CHA_TEST_HELD_REPLIES__ = [];
    nativeWindow.chrome.webview.postMessage = (raw: string) => {
      const message = JSON.parse(raw);
      if (message.method === heldMethod) {
        if (fail) {
          fail = false;
          // Valid, nonexistent item: induce a real runtime failure.
          message.params.id = Number.MAX_SAFE_INTEGER;
        } else ids.add(message.id);
      }
      post(JSON.stringify(message));
    };
    nativeWindow.__CHA_NATIVE_RECEIVE__ = (batch: any) => {
      const messages = batch.messages.filter((message: any) => {
        if (!ids.has(message.id)) return true;
        nativeWindow.__CHA_TEST_HELD_REPLIES__.push(message);
        return false;
      });
      receive({ ...batch, messages });
    };
    nativeWindow.__CHA_TEST_RELEASE_REPLIES__ = () => {
      nativeWindow.chrome.webview.postMessage = post;
      nativeWindow.__CHA_NATIVE_RECEIVE__ = receive;
      receive({
        connection_id: nativeWindow.__CHA_NATIVE_CONNECTION_ID__,
        delivery_id: 0,
        messages: nativeWindow.__CHA_TEST_HELD_REPLIES__,
      });
    };
  }, { heldMethod: method, fail: failFirst });
}

// Drive the production Win32 entry points and the real timeout dialog. These
// automated checks are separate from the operator's packaged-app checks.
function nativeWindow(child: ChildProcess, action: 'close' | 'exit' | 'cancel' | 'anyway' | 'fatal' | 'fatal-ok' | 'inspect') {
  const script = `
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CHAWindow {
  public delegate bool Visit(IntPtr window, IntPtr data);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Visit visit, IntPtr data);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
  public static IntPtr Find(uint process, string name) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((window, data) => {
      uint owner; GetWindowThreadProcessId(window, out owner);
      var text = new StringBuilder(256); GetClassName(window, text, text.Capacity);
      if (owner == process && text.ToString() == name && IsWindowVisible(window)) found = window;
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static string Title(IntPtr window) {
    var text = new StringBuilder(512); GetWindowText(window, text, text.Capacity);
    return text.ToString();
  }
}
'@
$main = [CHAWindow]::Find(${child.pid}, 'CHA.MainWindow')
$dialog = [CHAWindow]::Find(${child.pid}, '#32770')
${action === 'close' ? '[CHAWindow]::PostMessage($main, 16, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null' : ''}
${action === 'exit' ? '[CHAWindow]::PostMessage($main, 273, [IntPtr]1001, [IntPtr]::Zero) | Out-Null' : ''}
${action === 'fatal' ? '[CHAWindow]::PostMessage($main, 32770, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null' : ''}
${action === 'cancel' || action === 'anyway' ? `if ($dialog -eq [IntPtr]::Zero) { throw 'Native close dialog was not found' }
[CHAWindow]::PostMessage($dialog, 1126, [IntPtr]${action === 'cancel' ? 2 : 1}, [IntPtr]::Zero) | Out-Null` : ''}
${action === 'fatal-ok' ? '[CHAWindow]::PostMessage($dialog, 256, [IntPtr]13, [IntPtr]1) | Out-Null' : ''}
@{ title = [CHAWindow]::Title($main); dialog = ($dialog -ne [IntPtr]::Zero) } | ConvertTo-Json -Compress
`;
  const result = spawnSync('powershell.exe', ['-NoProfile', '-Command', script], { encoding: 'utf8' });
  if (result.status !== 0) throw new Error(result.stderr || 'Win32 close probe failed');
  return JSON.parse(result.stdout.trim()) as { title: string; dialog: boolean };
}

async function trackClose(page: Page, withhold = false): Promise<void> {
  await page.evaluate((hold) => {
    const nativeWindow = window as any;
    const prepare = nativeWindow.__CHA_NATIVE_PREPARE_CLOSE__;
    const abort = nativeWindow.__CHA_NATIVE_CLOSE_ABORTED__;
    nativeWindow.__CHA_TEST_CLOSE_ATTEMPTS__ = [];
    nativeWindow.__CHA_TEST_CLOSE_ABORTS__ = [];
    nativeWindow.__CHA_NATIVE_PREPARE_CLOSE__ = (attempt: number) => {
      nativeWindow.__CHA_TEST_CLOSE_ATTEMPTS__.push({ attempt, time: Date.now() });
      if (!hold) prepare(attempt);
    };
    nativeWindow.__CHA_NATIVE_CLOSE_ABORTED__ = (attempt: number) => {
      nativeWindow.__CHA_TEST_CLOSE_ABORTS__.push({ attempt, time: Date.now() });
      abort(attempt);
    };
  }, withhold);
}

test('note audio parity', async () => {
  test.skip(!process.env.CHA_NATIVE_PARITY_SCRIPT, 'Run through the note audio fixture service.');
  test.setTimeout(90_000);
  const launched = await launchHost(process.env.CHA_NATIVE_TEST_CONFIG);
  try {
    const page = launched.browser.contexts()[0].pages()[0];
    await page.waitForSelector('textarea[aria-label="Message"]:not(:disabled)');
    const script = readFileSync(process.env.CHA_NATIVE_PARITY_SCRIPT!, 'utf8');
    const result = await page.evaluate(`${script}\nnativeParity()`);
    expect(result, JSON.stringify(result)).toMatchObject({ ok: true });
    nativeWindow(launched.child, 'close');
    await expect.poll(() => launched.child.exitCode).toBe(0);
  } finally {
    await launched.browser.close().catch(() => undefined);
    if (launched.child.exitCode === null) launched.child.kill();
    await expect.poll(() => launched.child.exitCode).not.toBeNull();
  }
});

test.describe('WebView2 native host', () => {
  test.skip(
    !executable || !assets || !prepareVault,
    'Windows WebView2 host and native test-vault helper are not available',
  );

  let host: ChildProcess | undefined;
  let browser: Browser | undefined;
  let page: Page;

  test.beforeEach(async () => {
    const launched = await launchHost();
    host = launched.child;
    browser = launched.browser;
    const openedPage = browser.contexts()[0]?.pages()[0];
    expect(openedPage).toBeDefined();
    if (!openedPage) throw new Error('WebView2 did not expose an application page');
    page = openedPage;
    await waitForApplication(page);
    await page.waitForLoadState('load');
    console.log(`WebView2 ${browser.version()}, Node ${process.versions.node}`);
  });

  test.afterEach(async () => {
    await browser?.close().catch(() => undefined);
    if (host && host.exitCode === null) {
      const exited = new Promise<void>((resolve) => host!.once('exit', () => resolve()));
      host.kill();
      await Promise.race([
        exited,
        new Promise<void>((resolve) => setTimeout(resolve, 5_000)),
      ]);
    }
    if (userData) removeTestData(userData);
  });

  test('runs the packaged application through the real native runtime', async () => {
    await waitForApplication(page);
    expect(await page.evaluate(() => window.location.origin)).toBe(devOrigin ?? 'cha://app');
    expect(await page.evaluate(() => window.isSecureContext)).toBe(true);

    const composer = page.locator('textarea[aria-label="Message"]');
    await expect(composer).toBeEnabled();
    await composer.fill('Hello from the Windows native host');
    await page.locator('button[aria-label="Send message"]').click();

    const transcript = page.locator('[aria-label="Conversation transcript"]');
    await expect(transcript).toContainText('Hello from the Windows native host');

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
    await invoke(page, 'vault.switch', {
      vault_name: copied.display_name,
      password: null,
    }, restored.context_epoch);
    // Successful requested switches no longer trigger fault-recovery reloads.
    // This direct RPC probe bypasses App's switch action; reload to bootstrap
    // the new vault before exercising its UI.
    await page.evaluate(() => history.replaceState(null, '', '#/'));
    await page.reload();
    await waitForApplication(page);
    const switched = await invoke(page, 'app.bootstrap', {}, 0);
    expect(switched.bootstrap.vault_name).toBe(copied.display_name);

    await invoke(page, 'vault.switch', {
      vault_name: originalVault,
      password: null,
    }, switched.context_epoch);
    await page.evaluate(() => history.replaceState(null, '', '#/'));
    await page.reload();
    await waitForApplication(page);
    const finalBootstrap = await invoke(page, 'app.bootstrap', {}, 0);
    expect(finalBootstrap.bootstrap.vault_name).toBe(originalVault);
    await page.reload();
    await waitForApplication(page);
  });

  test('enforces CSP and retains session, file, navigation and vault behavior', async () => {
    test.skip(!!devOrigin, 'The packaged host supplies the production CSP.');
    test.setTimeout(60000);
    await waitForApplication(page);
    const source = readFileSync(process.env.CHA_NATIVE_PARITY_SCRIPT ?? join(__dirname, '..', 'parity.js'), 'utf8');
    const result = await page.evaluate(source + '\nnativeParity();');
    expect(result).toMatchObject({ok: true});
    await page.reload();
    await waitForApplication(page);
  });

  test('blocks Blob documents from replacing the privileged shell', async () => {
    await waitForApplication(page);
    const initialUrl = page.url();
    const connection = await page.evaluate(() => (window as any).__CHA_NATIVE_CONNECTION_ID__);
    await page.evaluate(() => {
      const blob = new Blob(['<script>window.__BLOB_DOCUMENT__ = true;</script>'], {type: 'text/html'});
      const url = URL.createObjectURL(blob);
      location.assign(url);
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    });
    await page.waitForTimeout(500);
    expect(page.url()).toBe(initialUrl);
    expect(await page.evaluate(() => (window as any).__CHA_NATIVE_CONNECTION_ID__)).toBe(connection);
    expect(await page.evaluate(() => (window as any).__BLOB_DOCUMENT__)).toBeUndefined();
  });

  test('exits the assertion as failed when asked', async () => {
    await waitForApplication(page);
    const ok = await page.evaluate(() => 1 === 0);
    expect(ok).toBe(true);
  });

  test('routes media requests to the native host', async () => {
    await waitForApplication(page);
    const result = await page.evaluate(async () => {
      const response = await fetch('/media/r999999999999', { cache: 'no-store' });
      return { status: response.status, body: await response.text() };
    });
    expect(result).toEqual({ status: 404, body: 'not found' });
  });

  test('validates native audio chunk requests', async () => {
    await waitForApplication(page);
    const statuses = await page.evaluate(async () => {
      const results: number[] = [];
      for (const offset of ['0', '-1', 'invalid']) {
        const response = await fetch('/media/r999999999999', { cache: 'no-store',
          headers: { 'X-CHA-Audio-Offset': offset } });
        results.push(response.status);
      }
      return results;
    });
    expect(statuses).toEqual([404, 400, 400]);
  });

  test('Notes packaged CSP preserves formatting, whitespace, tree keyboard and prompt insertion', async () => {
    test.skip(!!devOrigin, 'Requires the production CSP and bundled styles.');
    await newNote(page, 'Windows authoring');
    const violations: string[] = [];
    page.on('console', (message) => {
      if (message.text().includes('Content Security Policy')) violations.push(message.text());
    });
    await pasteNote(page, '<p>A  <strong>bold</strong> <mark>highlight</mark></p>');
    await expect(page.locator('.tiptap strong')).toHaveText('bold');
    await expect(page.locator('.tiptap mark')).toHaveText('highlight');
    expect(await page.locator('.tiptap').evaluate((element) => getComputedStyle(element).whiteSpace)).toBe('break-spaces');
    expect(await page.locator('.tiptap mark').evaluate((element) => getComputedStyle(element).backgroundColor)).not.toBe('rgba(0, 0, 0, 0)');
    await page.locator('.tiptap').evaluate((element) => {
      const editor = (element as any).chaEditor;
      editor.commands.setTextSelection(2);
    });
    const prompt = page.getByRole('textbox', { name: 'Prompt', exact: true });
    await prompt.fill('literal /clear\nnext line');
    await prompt.press('Enter');
    await expect(prompt).toHaveValue('');
    await expect(page.locator('.tiptap')).toContainText('literal /clear');
    expect(await noteJSON(page)).toMatchObject({ content: [
      { type: 'paragraph' },
      { type: 'paragraph', content: [
        { type: 'text', text: 'literal /clear' }, { type: 'hardBreak' }, { type: 'text', text: 'next line' },
      ] },
      { type: 'paragraph' },
    ] });
    await historyAction(page, 'Undo');
    await expect(page.locator('.tiptap')).not.toContainText('literal /clear');
    await historyAction(page, 'Redo');
    await expect(page.locator('.tiptap')).toContainText('literal /clear');
    const row = page.getByRole('treeitem').filter({ hasText: 'Windows authoring' });
    await row.focus();
    await row.press('Shift+F10');
    await expect(page.getByRole('menuitem', { name: 'Rename', exact: true })).toBeVisible();
    await page.waitForTimeout(100);
    await expect(page.getByRole('menuitem', { name: 'Rename', exact: true })).toBeFocused();
    await page.keyboard.press('Escape');
    expect(violations).toEqual([]);
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes comment highlighting follows runtime appearance changes', async () => {
    test.skip(!!devOrigin, 'Requires the bundled styles.');
    const preference = spawnSync('powershell.exe', ['-NoProfile', '-Command',
      '$value = (Get-ItemProperty -Path HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize -ErrorAction SilentlyContinue).AppsUseLightTheme; '
      + 'if ($null -eq $value -or $value -ne 0) { "light" } else { "dark" }',
    ], { encoding: 'utf8' });
    expect(preference.status).toBe(0);
    const osScheme = preference.stdout.trim();
    expect(['light', 'dark']).toContain(osScheme);
    expect(await page.evaluate(() => matchMedia('(prefers-color-scheme: dark)').matches)).toBe(osScheme === 'dark');
    await newNote(page, 'Adaptive comments');
    await page.locator('.tiptap').evaluate((element) => {
      (element as any).chaEditor.commands.setContent({ type: 'doc', content: [
        { type: 'paragraph', content: [{ type: 'text', text: 'Existing comment',
          marks: [{ type: 'highlight', attrs: { color: '#b3d9ff' } }] }] },
        { type: 'paragraph', content: [{ type: 'text', text: 'Ordinary highlight',
          marks: [{ type: 'highlight' }] }] },
      ] });
    });
    const before = await noteJSON(page);
    const comment = page.locator('.tiptap mark[data-color="#b3d9ff"]');
    await comment.evaluate((element) => { (window as any).__commentAppearanceNode = element; });
    for (const colorScheme of ['light', 'dark', 'light'] as const) {
      await page.emulateMedia({ colorScheme });
      expect(await page.evaluate(() => matchMedia('(prefers-color-scheme: dark)').matches)).toBe(colorScheme === 'dark');
      await expect(comment).toHaveCSS('background-color', colorScheme === 'dark' ? 'rgb(59, 83, 110)' : 'rgb(211, 227, 245)');
      await expect(comment).toHaveCSS('color', colorScheme === 'dark' ? 'rgb(245, 245, 245)' : 'rgb(26, 28, 31)');
      await expect(page.locator('.tiptap mark:not([data-color])')).toHaveCSS('background-color', 'rgb(255, 230, 120)');
      expect(await comment.evaluate((element) => element === (window as any).__commentAppearanceNode)).toBe(true);
      expect(await noteJSON(page)).toEqual(before);
    }
    await page.emulateMedia({ colorScheme: null });
    expect(await page.evaluate(() => matchMedia('(prefers-color-scheme: dark)').matches)).toBe(osScheme === 'dark');
  });

  test('Notes imports supported text, skips invalid pictures and keeps empty paste and file drops unchanged', async () => {
    await newNote(page, 'Windows imports');
    await pasteNote(page, '<html xmlns:o="urn:schemas-microsoft-com:office:office"><p><span style="color:red;background-color:yellow">Word</span></p><table><tr><td>one</td><td><b>two</b></td></tr></table></html>');
    await expect(page.locator('.tiptap mark')).toHaveText('Word');
    await expect(page.locator('.tiptap')).toContainText('one');
    await expect(page.locator('.tiptap')).toContainText('two');
    expect(await page.locator('.tiptap table, .tiptap [style*="color:red"]').count()).toBe(0);
    await historyAction(page, 'Undo');
    await expect(page.locator('.tiptap')).toHaveText('');
    await historyAction(page, 'Redo');
    const before = await noteJSON(page);
    await page.locator('.tiptap').evaluate((element) => (element as any).chaEditor.commands.selectAll());
    await pasteNote(page, '<iframe src="https://example.com"></iframe>');
    expect(await noteJSON(page)).toEqual(before);
    await pasteNote(page, '<p>must not insert<img src="data:image/png;base64,%%%"></p>');
    await expect(page.locator('.tiptap')).toHaveText('must not insert');
    await expect(page.locator('.tiptap img')).toHaveCount(0);
    await historyAction(page, 'Undo');
    expect(await noteJSON(page)).toEqual(before);
    await page.locator('.tiptap').evaluate((element) => {
      const data = new DataTransfer();
      data.items.add(new File(['file'], 'ignored.png', { type: 'image/png' }));
      data.setData('text/html', '<p>alternate format</p>');
      data.setData('text/plain', 'alternate format');
      element.dispatchEvent(new DragEvent('drop', { dataTransfer: data, bubbles: true }));
    });
    expect(await noteJSON(page)).toEqual(before);
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes search supports literal empty replacements, regex limits and whole-action history', async () => {
    await newNote(page, 'Windows search');
    await pasteNote(page, '<p><b>ca</b>t Cat catalog cat</p><p>hard<br>break</p>');
    const original = await noteJSON(page);
    await page.locator('.tiptap').click();
    await page.keyboard.press('Control+f');
    await page.getByRole('textbox', { name: 'Find', exact: true }).fill('cat');
    await page.getByLabel('Whole word', { exact: true }).check();
    await expect(page.locator('.cha-notes-find')).toContainText('/ 3');
    await page.getByLabel('Replace', { exact: true }).fill('$1');
    await page.getByRole('button', { name: 'Replace all', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveText('$1 $1 catalog $1hardbreak');
    await historyAction(page, 'Undo');
    expect(await noteJSON(page)).toEqual(original);
    await page.getByLabel('Replace', { exact: true }).fill('');
    await page.getByRole('button', { name: 'Replace all', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveText('  catalog hardbreak');
    await historyAction(page, 'Undo');
    expect(await noteJSON(page)).toEqual(original);
    await page.getByLabel('Regular expression', { exact: true }).check();
    await expect(page.getByLabel('Whole word', { exact: true })).not.toBeChecked();
    await expect(page.getByLabel('Whole word', { exact: true })).toBeDisabled();
    await page.getByRole('textbox', { name: 'Find', exact: true }).fill('(?<=c)at');
    await expect(page.locator('.cha-notes-find')).toContainText('No matches');
    await page.getByRole('textbox', { name: 'Find', exact: true }).fill('cat');
    await page.getByLabel('Match case', { exact: true }).check();
    await expect(page.locator('.cha-notes-find')).toContainText('/ 3');
    await page.getByRole('button', { name: 'Close', exact: true }).click();
    expect(await page.locator('.tiptap .find-and-replace-result').count()).toBe(0);
    expect(await noteJSON(page)).toEqual(original);
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes pictures preserve four formats, skip oversized pictures and reopen offline', async () => {
    test.setTimeout(60_000);
    await newNote(page, 'Windows pictures');
    const sources = await page.evaluate(() => {
      const fixtures = [
        ['image/png', [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]],
        ['image/jpeg', [0xff, 0xd8, 0xff]],
        ['image/gif', [0x47, 0x49, 0x46, 0x38, 0x39, 0x61]],
        ['image/webp', [0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x45, 0x42, 0x50]],
      ] as const;
      return fixtures.map(([mime, header]) => `data:${mime};base64,${btoa(String.fromCharCode(...header))}`);
    });
    await pasteNote(page, '<p>pictures</p>' + sources.map((src) => `<img src="${src}">`).join(''));
    await expect(page.locator('.tiptap img')).toHaveCount(4);
    expect(await page.locator('.tiptap img').evaluateAll((images) => images.map((image) => image.getAttribute('src')))).toEqual(sources);
    await historyAction(page, 'Undo');
    await expect(page.locator('.tiptap img')).toHaveCount(0);
    await historyAction(page, 'Redo');
    await expect(page.locator('.tiptap img')).toHaveCount(4);
    await page.locator('.tiptap').evaluate((element) => {
      const editor = (element as any).chaEditor;
      editor.commands.insertContentAt(editor.state.doc.content.size, { type: 'paragraph' });
      editor.commands.setTextSelection(editor.state.doc.content.size - 1);
      const data = new DataTransfer();
      const bytes = new Uint8Array(1_048_576);
      bytes.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
      data.items.add(new File([bytes], 'clipboard.png', { type: 'image/png' }));
      element.dispatchEvent(new ClipboardEvent('paste', { clipboardData: data, bubbles: true }));
    });
    await expect(page.locator('.tiptap img')).toHaveCount(5);
    await page.locator('.tiptap').evaluate((element) => {
      const editor = (element as any).chaEditor;
      editor.commands.insertContentAt(editor.state.doc.content.size, { type: 'paragraph' });
      editor.commands.setTextSelection(editor.state.doc.content.size - 1);
    });
    const before = await noteJSON(page);
    const oversized = 'data:image/png;base64,' + Buffer.concat([
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
      Buffer.alloc(1_048_576 - 7),
    ]).toString('base64');
    await pasteNote(page, `<p>keep text<img src="${oversized}"></p>`);
    await expect(page.locator('.tiptap')).toContainText('keep text');
    await expect(page.locator('.tiptap img')).toHaveCount(5);
    await expect(page.getByRole('alert')).toHaveCount(0);
    await historyAction(page, 'Undo');
    expect(await noteJSON(page)).toEqual(before);
    await page.getByRole('button', { name: 'Settings', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveCount(0);
    await page.getByRole('treeitem', { name: 'Windows pictures', exact: true }).click();
    await expect(page.locator('.tiptap img')).toHaveCount(5);
    expect(await noteJSON(page)).toEqual(before);
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes pasted links retain marks, reject unsupported destinations and persist', async () => {
    await newNote(page, 'Windows links');
    await pasteNote(page, '<p><b><a href="http://example.com/old">named</a></b> <a href="file:///C:/private">blocked</a></p>');
    await expect(page.locator('.tiptap a')).toHaveCount(1);
    const original = await noteJSON(page);
    await expect(page.locator('.tiptap a')).toHaveAttribute('href', 'http://example.com/old');
    await expect(page.locator('.tiptap strong')).toHaveText('named');
    await historyAction(page, 'Undo');
    await expect(page.locator('.tiptap')).toHaveText('');
    await historyAction(page, 'Redo');
    expect(await noteJSON(page)).toEqual(original);
    await page.getByRole('button', { name: 'Settings', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveCount(0);
    await page.getByRole('treeitem', { name: 'Windows links', exact: true }).click();
    await expect(page.locator('.tiptap')).toBeVisible();
    expect(await noteJSON(page)).toEqual(original);
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes deletion waits for an admitted save and blocks navigation until completion', async () => {
    await newNote(page, 'Windows deletion');
    await holdReply(page, 'notes.save');
    await pasteNote(page, '<p>admitted save for deletion</p>');
    await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
    await page.getByRole('textbox', { name: 'Prompt', exact: true }).fill('retained draft');
    await page.getByRole('button', { name: 'Actions for Windows deletion', exact: true }).click();
    await page.getByRole('menuitem', { name: 'Delete', exact: true }).click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete note', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'false');
    // The modal also prevents physical clicks. Invoke the competing callback
    // directly to verify the operation slot protects it independently.
    await page.getByRole('button', { name: 'Settings', exact: true }).evaluate((button: HTMLButtonElement) => button.click());
    await expect(page.locator('.tiptap')).toBeVisible();
    await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
    await expect(page.locator('.tiptap')).toHaveCount(0);
    await expect(page.getByRole('treeitem', { name: 'Windows deletion', exact: true })).toHaveCount(0);
    await expect(page.getByRole('textbox', { name: 'Prompt', exact: true })).toHaveValue('retained draft');
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes near-limit edits and Replace All reject atomically and reopen through the real bridge', async () => {
    test.setTimeout(120_000);
    await newNote(page, 'Windows limit');
    const measured = await page.locator('.tiptap').evaluate((element) => {
      const editor = (element as any).chaEditor;
      const pictureBytes = new Uint8Array(1_048_576);
      const canvas = document.createElement('canvas');
      canvas.width = canvas.height = 1;
      const png = atob(canvas.toDataURL('image/png').split(',')[1]);
      pictureBytes.set(Uint8Array.from(png, (character) => character.charCodeAt(0)));
      let raw = '';
      for (let i = 0; i < pictureBytes.length; i += 8192) {
        raw += String.fromCharCode(...pictureBytes.subarray(i, i + 8192));
      }
      const src = 'data:image/png;base64,' + btoa(raw);
      const content = [
        ...Array.from({ length: 8 }, () => ({ type: 'image', attrs: { src, alt: null, title: null } })),
        { type: 'paragraph', content: [{ type: 'text', text: 'cat 世界 привет' }] },
      ];
      editor.commands.setContent({ type: 'doc', content });
      // Measure the schema's serialized attributes, including image defaults.
      const doc = editor.getJSON();
      const size = new TextEncoder().encode(JSON.stringify(doc)).byteLength;
      doc.content[8].content[0].text += 'x'.repeat(12_582_912 - size);
      const start = performance.now();
      editor.commands.setContent(doc);
      const bytes = () => new TextEncoder().encode(JSON.stringify(editor.getJSON())).byteLength;
      const initialBytes = bytes();
      editor.commands.setTextSelection(editor.state.doc.content.size - 1);
      editor.commands.insertContent('z');
      return { initialBytes, rejectedBytes: bytes(), elapsedMs: performance.now() - start };
    });
    expect(measured.initialBytes).toBe(12_582_912);
    expect(measured.rejectedBytes).toBe(12_582_912);
    await expect(page.getByRole('alert')).toContainText('Note exceeds 12 MiB');
    await page.getByRole('button', { name: 'Find', exact: true }).click();
    await page.getByRole('textbox', { name: 'Find', exact: true }).fill('cat');
    await page.getByLabel('Replace', { exact: true }).fill('longer');
    await page.getByRole('button', { name: 'Replace all', exact: true }).click();
    const finalSize = await page.locator('.tiptap').evaluate((element) => new TextEncoder()
      .encode(JSON.stringify((element as any).chaEditor.getJSON())).byteLength);
    expect(finalSize).toBe(12_582_912);
    await page.getByRole('button', { name: 'Settings', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveCount(0);
    await page.getByRole('treeitem', { name: 'Windows limit', exact: true }).click();
    await expect(page.locator('.tiptap img[src^="data:"]')).toHaveCount(8);
    expect(await page.locator('.tiptap').evaluate((element) => new TextEncoder()
      .encode(JSON.stringify((element as any).chaEditor.getJSON())).byteLength)).toBe(12_582_912);
    console.log(`Notes limit: exact acceptance/rejection ${measured.elapsedMs.toFixed(1)} ms`);
    await page.reload();
    await waitForApplication(page);
  });

  test('Notes native window close saves before shutdown and duplicate Exit preserves the wait', async () => {
    await newNote(page, 'Windows close save');
    await holdReply(page, 'notes.save');
    await trackClose(page);
    await pasteNote(page, '<p>saved by normal window close</p>');
    const title = nativeWindow(host!, 'close').title;
    await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
    nativeWindow(host!, 'exit');
    expect(nativeWindow(host!, 'inspect').title).toBe(title);
    expect(await page.evaluate(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length)).toBe(1);
    expect(host!.exitCode).toBeNull();
    await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
    await expect.poll(() => host!.exitCode).toBe(0);
    await browser!.close().catch(() => undefined);
    const reopened = await launchHost(userData);
    host = reopened.child;
    browser = reopened.browser;
    page = browser.contexts()[0].pages()[0];
    await waitForApplication(page);
    await page.getByRole('tab', { name: 'Notes', exact: true }).click();
    await page.getByRole('treeitem', { name: 'Windows close save', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveText('saved by normal window close');
  });

  test('Notes native deferred leave timeout aborts before the dialog and late completion cannot close', async () => {
    test.setTimeout(45_000);
    await newNote(page, 'Windows leave timeout');
    await holdReply(page, 'notes.save');
    await trackClose(page);
    await pasteNote(page, '<p>pending leave save</p>');
    await page.getByRole('button', { name: 'Settings', exact: true }).click();
    await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
    const title = nativeWindow(host!, 'close').title;
    await page.waitForTimeout(9_000);
    nativeWindow(host!, 'exit');
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ABORTS__.length === 1,
      undefined, { timeout: 12_000 });
    const elapsed = await page.evaluate(() => {
      const nativeWindow = window as any;
      return nativeWindow.__CHA_TEST_CLOSE_ABORTS__[0].time - nativeWindow.__CHA_TEST_CLOSE_ATTEMPTS__[0].time;
    });
    expect(elapsed).toBeGreaterThanOrEqual(14_000);
    expect(elapsed).toBeLessThan(21_000);
    expect(nativeWindow(host!, 'inspect')).toEqual({ title, dialog: true });
    nativeWindow(host!, 'cancel');
    await expect.poll(() => nativeWindow(host!, 'inspect').dialog).toBe(false);
    await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
    await expect(page.locator('.tiptap')).toHaveCount(0);
    const settingsTitle = nativeWindow(host!, 'inspect').title;
    await page.evaluate(() => {
      const nativeWindow = window as any;
      nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({
        native_action: 'close_reply', connection_id: nativeWindow.__CHA_NATIVE_CONNECTION_ID__,
        attempt_id: nativeWindow.__CHA_TEST_CLOSE_ATTEMPTS__[0].attempt, result: 'ready',
      }));
    });
    await page.waitForTimeout(300);
    expect(host!.exitCode).toBeNull();
    expect(nativeWindow(host!, 'inspect').title).toBe(settingsTitle);
  });

  test('Notes native Close anyway shuts down a connected page that withholds its reply', async () => {
    test.setTimeout(30_000);
    await newNote(page, 'Windows forced close');
    await trackClose(page, true);
    nativeWindow(host!, 'exit');
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ABORTS__.length === 1,
      undefined, { timeout: 20_000 });
    expect(nativeWindow(host!, 'inspect').dialog).toBe(true);
    nativeWindow(host!, 'anyway');
    await expect.poll(() => host!.exitCode).toBe(0);
  });

  test('Notes native failed save followed by hung Retry has a fresh timed attempt', async () => {
    test.setTimeout(50_000);
    await newNote(page, 'Windows close retry');
    await holdReply(page, 'notes.save', true);
    await trackClose(page);
    await pasteNote(page, '<p>retry under the host timer</p>');
    nativeWindow(host!, 'close');
    const decision = page.getByRole('dialog', { name: 'Could not save “Windows close retry”.', exact: true });
    await expect(decision).toBeVisible();
    const title = nativeWindow(host!, 'inspect').title;
    // No native timer is allowed to run while the page asks for a decision.
    await page.waitForTimeout(16_000);
    expect(nativeWindow(host!, 'inspect')).toEqual({ title, dialog: false });
    await decision.getByRole('button', { name: 'Retry', exact: true }).click();
    await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
    expect(await page.evaluate(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length)).toBe(2);
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ABORTS__.length === 1,
      undefined, { timeout: 20_000 });
    nativeWindow(host!, 'cancel');
    await expect.poll(() => nativeWindow(host!, 'inspect').dialog).toBe(false);
    await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
    await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'true');
    expect(host!.exitCode).toBeNull();
  });

  for (const choice of ['Stay', 'Discard'] as const) {
    test(`Notes native failed save ${choice} preserves the correct close outcome`, async () => {
      await newNote(page, `Windows close ${choice}`);
      await holdReply(page, 'notes.save', true);
      await trackClose(page);
      await pasteNote(page, '<p>unsaved after failed close</p>');
      nativeWindow(host!, 'exit');
      const decision = page.getByRole('dialog', { name: `Could not save “Windows close ${choice}”.`, exact: true });
      await expect(decision).toBeVisible();
      await decision.getByRole('button', { name: choice, exact: true }).click();
      if (choice === 'Stay') {
        await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'true');
        await expect(page.locator('.tiptap')).toHaveText('unsaved after failed close');
        expect(host!.exitCode).toBeNull();
        expect(nativeWindow(host!, 'inspect').dialog).toBe(false);
      } else {
        await expect.poll(() => host!.exitCode).toBe(0);
      }
    });
  }

  test('Notes native close ignores wrong connections, invalid IDs and replaced-page replies', async () => {
    await newNote(page, 'Windows close identity');
    await trackClose(page, true);
    const oldConnection = await page.evaluate(() => (window as any).__CHA_NATIVE_CONNECTION_ID__);
    await page.evaluate(() => {
      const nativeWindow = window as any;
      nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({ native_action: 'close', connection_id: 'wrong' }));
    });
    await page.waitForTimeout(200);
    expect(await page.evaluate(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length)).toBe(0);
    nativeWindow(host!, 'close');
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length === 1);
    const oldAttempt = await page.evaluate(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__[0].attempt);
    await page.evaluate((attempt) => {
      const nativeWindow = window as any;
      for (const bad of [0, -1, 1.5, attempt + 1]) {
        nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({ native_action: 'close_reply',
          connection_id: nativeWindow.__CHA_NATIVE_CONNECTION_ID__, attempt_id: bad, result: 'ready' }));
      }
    }, oldAttempt);
    await page.waitForTimeout(200);
    expect(host!.exitCode).toBeNull();
    await page.reload();
    await waitForApplication(page);
    await trackClose(page, true);
    nativeWindow(host!, 'close');
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length === 1);
    await page.evaluate(({ connection, attempt }) => {
      const nativeWindow = window as any;
      nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({ native_action: 'close_reply',
        connection_id: connection, attempt_id: attempt, result: 'ready' }));
      nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({ native_action: 'close_reply',
        connection_id: nativeWindow.__CHA_NATIVE_CONNECTION_ID__, attempt_id: attempt, result: 'ready' }));
      nativeWindow.__CHA_NATIVE_POST__(JSON.stringify({ native_action: 'close_reply',
        connection_id: nativeWindow.__CHA_NATIVE_CONNECTION_ID__,
        attempt_id: nativeWindow.__CHA_TEST_CLOSE_ATTEMPTS__[0].attempt, result: 'cancelled' }));
    }, { connection: oldConnection, attempt: oldAttempt });
    await page.waitForTimeout(200);
    expect(host!.exitCode).toBeNull();
    expect(nativeWindow(host!, 'inspect').dialog).toBe(false);
  });

  for (const fail of [false, true]) {
    test(`Notes native close resumes after ${fail ? 'failed' : 'successful'} deletion`, async () => {
      await newNote(page, 'Windows delete close');
      await pasteNote(page, '<p>unsaved deletion remainder</p>');
      if (fail) {
        await page.evaluate(() => {
          const nativeWindow = window as any;
          const post = nativeWindow.chrome.webview.postMessage.bind(nativeWindow.chrome.webview);
          nativeWindow.chrome.webview.postMessage = (raw: string) => {
            const message = JSON.parse(raw);
            if (message.method === 'notes.delete') message.params.id = Number.MAX_SAFE_INTEGER;
            post(JSON.stringify(message));
          };
        });
      }
      await holdReply(page, 'notes.delete');
      await trackClose(page);
      await page.getByRole('button', { name: 'Actions for Windows delete close', exact: true }).click();
      await page.getByRole('menuitem', { name: 'Delete', exact: true }).click();
      await page.getByRole('dialog').getByRole('button', { name: 'Delete note', exact: true }).click();
      await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
      nativeWindow(host!, 'exit');
      await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length === 1);
      await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'false');
      expect(host!.exitCode).toBeNull();
      await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
      await expect.poll(() => host!.exitCode).toBe(0);
      await browser!.close().catch(() => undefined);
      const reopened = await launchHost(userData);
      host = reopened.child;
      browser = reopened.browser;
      page = browser.contexts()[0].pages()[0];
      await waitForApplication(page);
      await page.getByRole('tab', { name: 'Notes', exact: true }).click();
      const row = page.getByRole('treeitem', { name: 'Windows delete close', exact: true });
      if (fail) {
        await row.click();
        await expect(page.locator('.tiptap')).toHaveText('unsaved deletion remainder');
      } else await expect(row).toHaveCount(0);
    });
  }

  test('Notes native timeout cancels a deferred delete close without canceling deletion', async () => {
    test.setTimeout(30_000);
    await newNote(page, 'Windows hung delete');
    await pasteNote(page, '<p>discard after confirmed deletion</p>');
    await holdReply(page, 'notes.delete');
    await trackClose(page);
    await page.getByRole('button', { name: 'Actions for Windows hung delete', exact: true }).click();
    await page.getByRole('menuitem', { name: 'Delete', exact: true }).click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete note', exact: true }).click();
    await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
    nativeWindow(host!, 'close');
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ABORTS__.length === 1,
      undefined, { timeout: 20_000 });
    nativeWindow(host!, 'cancel');
    await expect.poll(() => nativeWindow(host!, 'inspect').dialog).toBe(false);
    // Cancellation belongs to close; the earlier deletion remains frozen.
    await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'false');
    await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
    await expect(page.locator('.tiptap')).toHaveCount(0);
    await expect(page.getByRole('treeitem', { name: 'Windows hung delete', exact: true })).toHaveCount(0);
    await page.waitForTimeout(300);
    expect(host!.exitCode).toBeNull();
  });

  test('Notes native fallback works while the renderer is paused', async () => {
    test.setTimeout(30_000);
    await newNote(page, 'Windows paused renderer');
    await trackClose(page);
    const session = await browser!.contexts()[0].newCDPSession(page);
    await session.send('Debugger.enable');
    await session.send('Debugger.pause');
    try {
      const title = nativeWindow(host!, 'close').title;
      await expect.poll(() => nativeWindow(host!, 'inspect'), { timeout: 20_000 })
        .toEqual({ title, dialog: true });
      nativeWindow(host!, 'cancel');
    } finally {
      await session.send('Debugger.resume');
      await session.detach();
    }
    await expect.poll(() => nativeWindow(host!, 'inspect').dialog).toBe(false);
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length === 1);
    await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'true');
    expect(host!.exitCode).toBeNull();
  });

  test('Notes browser Back and Forward use Chat history and Stay restores the exact address', async () => {
    const bootstrap = await invoke(page, 'app.bootstrap', {}, 0);
    await invoke(page, 'session.create', { forum_id: 'lobby', label: 'History A' }, bootstrap.context_epoch);
    await invoke(page, 'session.create', { forum_id: 'lobby', label: 'History B' }, bootstrap.context_epoch);
    await page.reload();
    await waitForApplication(page);
    await page.getByRole('button', { name: /^History A/ }).click();
    await page.waitForFunction(() => location.hash.startsWith('#/s/'));
    const first = page.url();
    await page.getByRole('button', { name: /^History B/ }).click();
    await expect.poll(() => page.url()).not.toBe(first);
    const previous = page.url();
    const historyLength = await page.evaluate(() => history.length);
    await newNote(page, 'Windows history');
    expect(page.url()).toBe(previous);
    expect(await page.evaluate(() => history.length)).toBe(historyLength);
    await holdReply(page, 'notes.save', true);
    await pasteNote(page, '<p>keep on Stay</p>');
    // CDP key events do not enter WebView2's native accelerator handler.
    // Physical Alt+Left/mouse Back remain operator checks; this exercises
    // actual browser history and its paired popstate/hashchange events.
    await page.goBack();
    const decision = page.getByRole('dialog', { name: 'Could not save “Windows history”.', exact: true });
    await expect(decision).toBeVisible();
    await decision.getByRole('button', { name: 'Stay', exact: true }).click();
    await expect(page.locator('.tiptap')).toHaveAttribute('contenteditable', 'true');
    expect(page.url()).toBe(previous);
    await expect(page.locator('.tiptap')).toHaveText('keep on Stay');
    expect(await page.evaluate(() => history.length)).toBe(historyLength);
    await page.goBack();
    await page.waitForFunction(() => (window as any).__CHA_TEST_HELD_REPLIES__.length === 1);
    await page.evaluate(() => (window as any).__CHA_TEST_RELEASE_REPLIES__());
    await expect(page.locator('.tiptap')).toHaveCount(0);
    await waitForApplication(page);
    await page.goForward();
    await expect.poll(() => page.url()).toBe(previous);
    await waitForApplication(page);
    expect(await page.evaluate(() => history.length)).toBe(historyLength);
  });

  test('Notes fatal error stops an existing close wait while its error dialog remains open', async () => {
    test.setTimeout(30_000);
    await newNote(page, 'Windows fatal during close');
    await trackClose(page, true);
    nativeWindow(host!, 'close');
    await page.waitForFunction(() => (window as any).__CHA_TEST_CLOSE_ATTEMPTS__.length === 1);
    // Use the existing Win32 fatal-event entry with its supported null-message
    // fallback. No test RPC or production fault-injection setting is added.
    nativeWindow(host!, 'fatal');
    await expect.poll(() => nativeWindow(host!, 'inspect').dialog).toBe(true);
    await page.waitForTimeout(16_000);
    expect(await page.evaluate(() => (window as any).__CHA_TEST_CLOSE_ABORTS__.length)).toBe(0);
    nativeWindow(host!, 'exit');
    nativeWindow(host!, 'fatal-ok');
    await expect.poll(() => host!.exitCode, { timeout: 5_000 }).toBe(1);
  });

});

test.describe('WebView2 startup close', () => {
  test.skip(!executable || !prepareVault, 'Requires the instrumented Windows host.');
  for (const phase of ['runtime', 'page'] as const) {
    test(`Notes closes directly without a ${phase}`, async () => {
      const root = mkdtempSync(join(tmpdir(), 'cha-webview2-'));
      try {
        const config = join(root, 'config');
        mkdirSync(config, { recursive: true });
        expect(spawnSync(prepareVault!, [config]).status).toBe(0);
        const started = Date.now();
        const child = spawn(executable!, [
          '--user-data', root, `--close-without-${phase}`,
        ], { stdio: 'ignore' });
        try {
          await expect.poll(() => child.exitCode, { timeout: 5_000 }).toBe(0);
          expect(Date.now() - started).toBeLessThan(5_000);
        } finally {
          if (child.exitCode === null) child.kill();
        }
      } finally {
        removeTestData(root);
      }
    });
  }

  test('Notes startup bootstrap replies ready before the application bundle loads', async () => {
    test.skip(!!devOrigin, 'Requires packaged assets without the application bundle.');
    const root = mkdtempSync(join(tmpdir(), 'cha-webview2-'));
    let launched: Awaited<ReturnType<typeof launchHost>> | undefined;
    try {
      const config = join(root, 'config');
      const bootAssets = join(root, 'assets');
      mkdirSync(config, { recursive: true });
      mkdirSync(bootAssets);
      expect(spawnSync(prepareVault!, [config]).status).toBe(0);
      // Serve the real shell/bootstrap, but omit the React bundle. No editor
      // can exist, so the injected default responder must allow startup close.
      copyFileSync(join(assets!, 'index.html'), join(bootAssets, 'index.html'));
      launched = await launchHost(root, bootAssets);
      const page = launched.browser.contexts()[0].pages()[0];
      await page.waitForFunction(() => typeof (window as any).__CHA_NATIVE_PREPARE_CLOSE__ === 'function');
      expect(await page.locator('.cha-app').count()).toBe(0);
      nativeWindow(launched.child, 'close');
      await expect.poll(() => launched!.child.exitCode, { timeout: 5_000 }).toBe(0);
    } finally {
      await launched?.browser.close().catch(() => undefined);
      if (launched?.child.exitCode === null) launched.child.kill();
      removeTestData(root);
    }
  });

  test('Notes fatal navigation error shuts down directly after acknowledgment', async () => {
    test.skip(!!devOrigin, 'Requires the development server to be stopped.');
    const root = mkdtempSync(join(tmpdir(), 'cha-webview2-'));
    let child: ChildProcess | undefined;
    try {
      const config = join(root, 'config');
      mkdirSync(config, { recursive: true });
      expect(spawnSync(prepareVault!, [config]).status).toBe(0);
      // An unavailable trusted development origin induces the real initial
      // navigation failure; this is not a synthetic fatal shell message.
      child = spawn(executable!, ['--user-data', root, '--dev-origin', 'http://127.0.0.1:5173'],
        { stdio: 'ignore' });
      await expect.poll(() => nativeWindow(child!, 'inspect').dialog, { timeout: 10_000 }).toBe(true);
      nativeWindow(child, 'fatal-ok');
      await expect.poll(() => child!.exitCode, { timeout: 5_000 }).toBe(1);
    } finally {
      if (child?.exitCode === null) child.kill();
      removeTestData(root);
    }
  });
});
