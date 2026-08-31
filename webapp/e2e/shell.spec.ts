import { expect, test, type BrowserContext, type Page } from '@playwright/test';
import { resolve } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import type { SessionSnapshot } from '../src/api/client';

// Creates a stored session in the workspace forum and leaves the page on it,
// connected, which is where every live-conversation test starts.
async function startLobbySession(page: Page, name: string): Promise<string> {
  await page.goto('/');
  await page.getByRole('button', { name: 'Forums' }).click();
  await page.getByRole('button', { name: /The Lobby\s+Guide/ }).click();
  await page.getByRole('button', { name: /New session\s+Enter a name to begin/ }).click();
  await page.getByRole('textbox', { name: 'Session name' }).fill(name);
  await page.getByRole('button', { name: 'Start session' }).click();
  await expect(page.getByRole('combobox', { name: 'Choose target character' })).toBeEnabled();
  return page.url();
}

async function createStoredLobbySession(page: Page, label: string): Promise<string> {
  await page.goto('/');
  return page.evaluate(async (sessionLabel) => {
    const response = await fetch('/api/v1/forums/lobby/sessions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ label: sessionLabel }),
    });
    if (!response.ok) throw new Error(`create answered ${response.status}`);
    return String((await response.json() as { id: string }).id);
  }, label);
}

// Creates a stored session, makes it live, and puts one exchange in it without
// ever attaching a browser stream. The recovery tests need the session's single
// stream slot free, so that what they observe is the client's own ladder rather
// than a slot held by setup machinery.
async function seedLiveSession(
  page: Page,
  label: string,
  prompt: string,
): Promise<{ sessionId: string; snapshot: SessionSnapshot }> {
  return page.evaluate(async ([sessionLabel, text]) => {
    const post = async (url: string, body: unknown): Promise<Record<string, unknown>> => {
      const response = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!response.ok) throw new Error(`POST ${url} answered ${response.status}`);
      return response.json() as Promise<Record<string, unknown>>;
    };
    const created = await post('/api/v1/forums/lobby/sessions', { label: sessionLabel });
    const id = String(created.id);
    await post(`/api/v1/forums/lobby/sessions/${id}/open`, {});
    await post(`/s/lobby/${id}/api/v1/input`, { text });

    // The recovery scenario is about a lost event stream, not a model request
    // racing the snapshot probe. Wait until the seeded exchange is durable and
    // inactive before handing the session to a fresh viewer.
    for (let attempt = 0; attempt < 200; attempt += 1) {
      const response = await fetch(`/s/lobby/${id}/api/v1/session`);
      if (!response.ok) throw new Error(`snapshot answered ${response.status}`);
      const snapshot = await response.json() as SessionSnapshot;
      if (!snapshot.generation.active) return { sessionId: id, snapshot };
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    throw new Error('seed generation did not finish');
  }, [label, prompt]);
}

test('creates a session and restores its conversation after a deep-link reload', async ({ page }) => {
  const sessionName = `Reloaded planning ${Date.now()}`;
  await page.goto('/');
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Hide sidebar' })).toBeVisible();

  await page.getByRole('button', { name: 'Forums' }).click();
  await page.getByRole('button', { name: /The Lobby\s+Guide/ }).click();
  await page.getByRole('button', { name: /New session\s+Enter a name to begin/ }).click();
  await page.getByRole('textbox', { name: 'Session name' }).fill(sessionName);
  await page.getByRole('button', { name: 'Start session' }).click();
  await expect(page).toHaveURL(/\/s\/lobby\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('The Lobby');

  await page.reload();
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByLabel('Current chat context')).toContainText('The Lobby');
  await expect(page.getByRole('button', { name: new RegExp(`${sessionName}\\s+The Lobby`) }))
    .toHaveAttribute('aria-current', 'page');

  await page.goBack();
  await expect(page).toHaveURL(/^http:\/\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');

  await page.goForward();
  await expect(page).toHaveURL(/\/s\/lobby\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('The Lobby');
});

test('renames an open session from Recent across every visible catalog', async ({ page }) => {
  const original = `Rename browser test ${Date.now()}`;
  const renamed = `Renamed browser test ${Date.now()}`;
  const sessionUrl = await startLobbySession(page, original);

  await page.getByRole('button', { name: 'Forums' }).click();
  await page.getByRole('button', { name: /The Lobby\s+Guide/ }).click();
  await expect(page.getByLabel('Forum sessions navigation')
    .getByRole('button', { name: new RegExp(`^${original}`) })).toBeVisible();

  await page.getByRole('button', { name: `Actions for ${original}` }).click();
  await page.getByRole('menuitem', { name: 'Rename…' }).click();
  await page.getByRole('textbox', { name: 'Session name' }).fill(renamed);
  await page.getByRole('button', { name: 'Rename', exact: true }).click();

  await expect(page.getByRole('button', { name: `Actions for ${renamed}` })).toBeVisible();
  await expect(page.getByLabel('Forum sessions navigation')
    .getByRole('button', { name: new RegExp(`^${renamed}`) })).toBeVisible();
  await page.getByRole('button', { name: new RegExp(`${renamed}\\s+The Lobby`) }).click();
  await expect(page).toHaveURL(sessionUrl);
  await expect(page.getByText(renamed, { exact: true }).last()).toBeVisible();
});

test('deletes a closed session and retains its archived row outside the catalog', async ({ page }) => {
  const label = `Closed delete browser test ${Date.now()}`;
  const sessionId = await createStoredLobbySession(page, label);
  await page.reload();

  await page.getByRole('button', { name: `Actions for ${label}` }).click();
  await page.getByRole('menuitem', { name: 'Delete…' }).click();
  await page.getByRole('button', { name: 'Delete', exact: true }).click();
  await expect(page.getByRole('button', { name: `Actions for ${label}` })).toHaveCount(0);

  const result = await page.evaluate(async (id) => {
    const listing = await fetch('/api/v1/forums/lobby/sessions');
    const sessions = await listing.json() as { id: string }[];
    const opened = await fetch(`/api/v1/forums/lobby/sessions/${id}/open`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}',
    });
    return { listed: sessions.some((session) => session.id === id), openStatus: opened.status };
  }, sessionId);
  expect(result).toEqual({ listed: false, openStatus: 404 });

  const databasePath = process.env.CHA_E2E_DATABASE;
  if (!databasePath) throw new Error('CHA_E2E_DATABASE is not set');
  const database = new DatabaseSync(resolve(databasePath), {
    readOnly: true,
  });
  try {
    const archived = database.prepare(`
      SELECT s.archived_at
      FROM sessions AS s JOIN forums AS f USING (forum_key)
      WHERE f.forum_id = ? AND s.session_id = ?
    `).get('lobby', sessionId) as { archived_at: number | null } | undefined;
    expect(archived?.archived_at).toEqual(expect.any(Number));
  } finally {
    database.close();
  }
});

test('deleting the active session returns the browser to Welcome', async ({ page }) => {
  const label = `Active delete browser test ${Date.now()}`;
  await startLobbySession(page, label);

  await page.getByRole('button', { name: `Actions for ${label}` }).click();
  await page.getByRole('menuitem', { name: 'Delete…' }).click();
  await page.getByRole('button', { name: 'Delete', exact: true }).click();

  await expect(page).toHaveURL(/^http:\/\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');
  await expect(page.getByRole('button', { name: `Actions for ${label}` })).toHaveCount(0);
});

test.describe('touch session actions', () => {
  test.use({ hasTouch: true, viewport: { width: 390, height: 844 } });

  test('keeps the Actions button visible and tappable without hover', async ({ page }) => {
    const label = `Touch actions ${Date.now()}`;
    await createStoredLobbySession(page, label);
    await page.reload();

    const actions = page.getByRole('button', { name: `Actions for ${label}` });
    await expect(actions).toBeVisible();
    await expect(actions).toHaveCSS('opacity', '1');
    await actions.tap();
    await expect(page.getByRole('menuitem', { name: 'Rename…' })).toBeVisible();
  });
});

test('accepts a JSON mutation with matching Host and Origin', async ({ page }) => {
  await page.goto('/');
  const result = await page.evaluate(async () => {
    const response = await fetch('/api/v1/forums/lobby/sessions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ label: `Proxy smoke test ${Date.now()}` }),
    });
    return { status: response.status, body: await response.json() };
  });

  expect(result.status).toBe(201);
  expect(result.body).toMatchObject({ label: expect.stringMatching(/^Proxy smoke test /) });
});

test('renders discovery screens from the server workspace', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');
  await expect(page.getByLabel('Current chat context')).toContainText('From: Guest');

  // Personas is a workspace-wide catalog: the built-in Guest beside every
  // configured persona, each row opening that persona's own PERSONA.md. Which
  // persona a forum speaks as is checked where the browser shows it, on the
  // chat context line of a Lobby session further down this file.
  await page.getByRole('button', { name: 'Personas' }).click();
  await expect(page.getByRole('button', { name: /Guest/ })).toBeVisible();
  await page.getByRole('button', { name: /Reader\s+The deterministic browser-test persona/ })
    .click();
  await expect(page.getByText('Write concise browser-test prompts.')).toBeVisible();
  await page.getByLabel('Persona detail navigation')
    .getByRole('button', { name: 'Personas' }).click();
  await expect(page.getByText('The deterministic browser-test persona')).toBeVisible();

  await page.getByRole('button', { name: 'Characters' }).click();
  await expect(page.getByText('A deterministic test character')).toBeVisible();
  await page.getByRole('button', { name: /Guide/ }).click();
  await expect(page.getByRole('heading', { name: 'Guide' }).first()).toBeVisible();
  await expect(page.getByText('Answer deterministically in browser tests.')).toBeVisible();

  await page.getByRole('button', { name: 'Forums' }).click();
  await expect(page.getByRole('button', { name: /The Lobby\s+Guide/ })).toBeVisible();
  await page.getByRole('button', { name: /The Lobby\s+Guide/ }).click();
  const sessions = page.getByLabel('Forum sessions navigation');
  await expect(sessions).toBeVisible();

  // Sessions is titled Sessions, so its header is the only thing naming the
  // forum, and it is the way into that forum's description.
  await sessions.getByRole('button', { name: /The Lobby\s+Guide/ }).click();
  await expect(page.getByRole('heading', { name: 'Lobby house rules' })).toBeVisible();
  await expect(page.getByText('Guide · speaking as Reader')).toBeVisible();
  await expect(page.getByText('Ask one thing at a time')).toBeVisible();
  await page.getByLabel('Forum detail navigation')
    .getByRole('button', { name: 'Sessions' }).click();
  await expect(sessions).toBeVisible();
});

test('recovers when the application API is initially unavailable', async ({ page }) => {
  await page.route('**/api/v1/bootstrap', (route) => route.abort(), { times: 1 });
  await page.goto('/');

  await expect(page.getByRole('heading', { name: 'Application API unavailable' })).toBeVisible();
  await page.getByRole('button', { name: 'Retry' }).click();
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');
});

test('keeps the pushed sidebar usable at desktop and iPhone widths', async ({ page }) => {
  await page.goto('/');
  for (const width of [1280, 390]) {
    await page.setViewportSize({ width, height: 844 });
    const app = page.locator('.cha-app');
    await expect(app).toHaveAttribute('data-sidebar', 'open');
    await expect(page.getByRole('button', { name: 'Hide sidebar' })).toBeInViewport();
    await expect(page.getByRole('textbox', { name: 'Message' })).toBeInViewport();
    // A pushed panel that kept its full width would hang off the right edge and
    // carry everything centred inside it off-centre with it.
    await expect(async () => {
      const panel = await page.locator('.cha-main').boundingBox();
      expect(panel).not.toBeNull();
      expect(Math.round((panel?.x ?? 0) + (panel?.width ?? 0))).toBeLessThanOrEqual(width);
    }).toPass();
    await expect(async () => {
      const pageSize = await page.evaluate(() => ({
        height: document.documentElement.scrollHeight,
        visibleHeight: document.documentElement.clientHeight,
        visibleWidth: document.documentElement.clientWidth,
        width: document.documentElement.scrollWidth,
      }));
      expect(pageSize.width).toBeLessThanOrEqual(pageSize.visibleWidth);
      expect(pageSize.height).toBeLessThanOrEqual(pageSize.visibleHeight);
    }).toPass();

    await page.getByRole('button', { name: 'Hide sidebar' }).click();
    await expect(app).toHaveAttribute('data-sidebar', 'closed');
    await expect(page.getByRole('button', { name: 'Show sidebar' })).toBeInViewport();
    await expect(page.getByRole('textbox', { name: 'Message' })).toBeInViewport();
    await page.getByRole('button', { name: 'Show sidebar' }).click();
  }
});

test('lays every screen out inside the visible panel', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 844 });
  await page.goto('/');
  await expect(page.getByLabel('Current chat context')).toBeVisible();

  // Each step leaves a different screen showing; the chat is already there.
  // Rows are located within their screen because other tests leave sessions
  // behind whose sidebar entries would otherwise match the same names.
  const on = (screen: string) => page.getByLabel(`${screen} navigation`);
  const steps: (() => Promise<void>)[] = [
    async () => {},
    async () => page.getByRole('button', { name: 'Personas' }).click(),
    async () => on('Personas').getByRole('button', { name: /Reader/ }).click(),
    async () => page.getByRole('button', { name: 'Characters' }).click(),
    async () => on('Characters').getByRole('button', { name: /Guide/ }).click(),
    async () => page.getByRole('button', { name: 'Forums' }).click(),
    async () => on('Forums').getByRole('button', { name: /The Lobby/ }).click(),
    async () => on('Forum sessions').getByRole('button', { name: /New session/ }).click(),
  ];

  for (const step of steps) {
    await step();
    // Nothing on a screen may reach past the edge of the window. A panel that
    // did not give up the width the sidebar pushed it by, or content laid out
    // against the panel's full width rather than its visible width, both show
    // up here.
    await expect(async () => {
      const escaped = await page.evaluate(() => {
        const limit = document.documentElement.clientWidth;
        return [...document.querySelectorAll<HTMLElement>('.cha-main *')]
          .filter((element) => {
            const box = element.getBoundingClientRect();
            return (box.width > 0 || box.height > 0) && box.right > limit + 0.5;
          })
          .map((element) => element.className);
      });
      expect(escaped).toEqual([]);
    }).toPass();
  }
});

test('rapid Recent navigation settles on the last requested session without an error', async ({ page }) => {
  await page.goto('/');
  const labels = Array.from({ length: 4 }, (_, index) => `Rapid ${Date.now()} ${index + 1}`);
  await page.evaluate(async (sessionLabels) => {
    for (const label of sessionLabels) {
      const response = await fetch('/api/v1/forums/lobby/sessions', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ label }),
      });
      if (!response.ok) throw new Error(`create answered ${response.status}`);
    }
  }, labels);
  await page.reload();

  await page.route('**/api/v1/forums/lobby/sessions/*/open', async (route) => {
    await new Promise((resolve) => setTimeout(resolve, 150));
    await route.continue();
  });
  // Session rows intentionally disable during an open, so Playwright performs
  // these as back-to-back user actions rather than concurrent HTTP requests.
  // The short idle grace still makes the sequence exercise capacity recovery.
  for (const label of labels) {
    await page.getByRole('button', { name: new RegExp(`${label}\\s+The Lobby`) }).click();
  }

  await expect(page.getByText(labels.at(-1) ?? '', { exact: true }).last()).toBeVisible();
  await expect(page.getByRole('combobox', { name: 'Choose target character' }))
    .toBeEnabled({ timeout: 15_000 });
  await expect(page.getByRole('alert')).toHaveCount(0);
});

test('submits as the forum persona and renders the streamed transcript', async ({ page }) => {
  const sessionName = `Live browser test ${Date.now()}`;
  const prompt = 'Stream this browser message back to me.';
  await startLobbySession(page, sessionName);

  await page.getByRole('textbox', { name: 'Message' }).fill(prompt);
  await page.getByRole('button', { name: 'Send message' }).click();

  await expect(page.locator('.cha-message.is-human').last()).toContainText(prompt);
  await expect(page.locator('.cha-message.is-character').last()).toContainText(prompt);
  await expect(page.getByLabel('Current chat context')).toContainText('From: Reader');

  // Guide's character.toml asks for a serif italic voice, so the whole path is
  // exercised here: the workspace file, the snapshot, and the rendered class.
  const spoken = page.locator('.cha-message.is-character .cha-message-text').last();
  await expect(spoken).toHaveClass(/cha-font-serif/);
  await expect(spoken).toHaveClass(/cha-slant-italic/);
  await expect(spoken).toHaveCSS('font-style', 'italic');
  await expect(page.locator('.cha-message.is-human .cha-message-text').last())
    .toHaveCSS('font-style', 'normal');
});

test('stops an active generation through the HTTP action', async ({ page }) => {
  await startLobbySession(page, `Stopped generation ${Date.now()}`);
  const prompt = `Stop this deterministic response ${'before it finishes '.repeat(40)}`;
  await page.getByRole('textbox', { name: 'Message' }).fill(prompt);
  await page.getByRole('button', { name: 'Send message' }).click();

  const stop = page.getByRole('button', { name: 'Stop generation' });
  await expect(stop).toBeEnabled();
  await stop.click();

  await expect(page.locator('.cha-entry-status').last()).toHaveText('Stopped');
  await expect(page.getByRole('button', { name: 'Send message' })).toBeVisible();
});

test('hands the session to the device that opened it last', async ({ page, context }) => {
  const sessionName = `Two devices ${Date.now()}`;
  const sessionUrl = await startLobbySession(page, sessionName);

  const second = await context.newPage();
  await second.goto(sessionUrl);
  // The page opened last is live immediately, with no wait for the first one
  // to give the session up.
  await expect(second.getByRole('combobox', { name: 'Choose target character' }))
    .toBeEnabled({ timeout: 15_000 });
  // The page it displaced parks with its transcript instead of reconnecting.
  await expect(page.getByRole('alert')).toContainText(
    'This conversation moved to another device',
    { timeout: 15_000 },
  );
  await expect(page.getByText(sessionName, { exact: true }).last()).toBeVisible();

  // Going back to the first device is the same one gesture in reverse.
  await page.getByRole('button', { name: 'Continue here' }).click();
  await expect(page.getByRole('combobox', { name: 'Choose target character' }))
    .toBeEnabled({ timeout: 10_000 });
  await expect(second.getByRole('alert')).toContainText(
    'This conversation moved to another device',
    { timeout: 15_000 },
  );
});

// Seeds a session, then hands back a page that has never streamed anything.
// Loading /health establishes the right origin for fetch without starting the
// browser application and consuming a registry entry for Welcome. The seeding
// page is closed before the one finite event-stream response is armed.
async function seedThenOpenFreshViewer(
  page: Page,
  context: BrowserContext,
  label: string,
  prompt: string,
): Promise<{ viewer: Page; sessionId: string; snapshot: SessionSnapshot }> {
  await page.goto('/health');
  const seeded = await seedLiveSession(page, label, prompt);
  await page.close();
  return { viewer: await context.newPage(), ...seeded };
}

test('recovers a dropped stream and keeps the conversation on screen', async ({ page, context }) => {
  const prompt = 'Remember this across the reconnect.';
  const { viewer, sessionId, snapshot } = await seedThenOpenFreshViewer(
    page, context, `Dropped stream ${Date.now()}`, prompt,
  );

  // A finite, valid event stream delivers state and then reaches EOF. That is
  // the browser-visible shape of a connection that drops mid-stream, without
  // the timing ambiguity of aborting a request while it is being dispatched.
  // Interception lapses afterwards; recovery and the replacement stream use CHA.
  await viewer.route('**/api/v1/events', (route) => route.fulfill({
    status: 200,
    contentType: 'text/event-stream',
    body: `event: snapshot\ndata: ${JSON.stringify(snapshot)}\n\n`,
  }), { times: 1 });
  await viewer.goto(`/s/lobby/${sessionId}/`);

  await expect(viewer.getByRole('combobox', { name: 'Choose target character' }))
    .toBeEnabled({ timeout: 15_000 });
  await expect(viewer.locator('.cha-message.is-human').last()).toContainText(prompt);
  await expect(viewer.locator('.cha-message.is-character').last()).toContainText(prompt);
  await expect(viewer.getByRole('button', { name: 'Send message' })).toBeVisible();
});

test('re-opens after a real disconnect and server idle unload', async ({ page, context }) => {
  const sessionName = `Unloaded session ${Date.now()}`;
  const sessionUrl = await startLobbySession(page, sessionName);
  const snapshotPath = `${new URL(sessionUrl).pathname}api/v1/session`;
  await page.close();
  const viewer = await context.newPage();
  let snapshotRequests = 0;

  // End the first stream before it reaches CHA. The recovery snapshot is held
  // longer than the test server's idle grace, giving the now-unobserved
  // runtime time to unload. Continuing that real request therefore yields
  // session_not_live; the third snapshot can only happen after the client has
  // re-opened the stored session.
  await viewer.route(`**${snapshotPath}`, async (route) => {
    snapshotRequests += 1;
    if (snapshotRequests === 2) {
      await new Promise((resolve) => setTimeout(resolve, 2_500));
    }
    await route.continue();
  });
  await viewer.route('**/api/v1/events', (route) => route.fulfill({
    status: 200,
    contentType: 'text/event-stream',
    body: ': connected\n\n',
  }), { times: 1 });
  await viewer.goto(sessionUrl);

  await expect(viewer.getByRole('combobox', { name: 'Choose target character' }))
    .toBeEnabled({ timeout: 15_000 });
  await expect(viewer.getByText(sessionName, { exact: true }).last()).toBeVisible();
  expect(snapshotRequests).toBeGreaterThanOrEqual(3);
});
