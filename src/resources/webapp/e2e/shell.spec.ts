import { expect, test } from '@playwright/test';

test('creates a session and restores its conversation after a deep-link reload', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Hide sidebar' })).toBeVisible();

  await page.getByRole('button', { name: 'Forums' }).click();
  await page.getByRole('button', { name: /The Lobby\s+Guide/ }).click();
  await page.getByRole('button', { name: /New session\s+Enter a name to begin/ }).click();
  await page.getByRole('textbox', { name: 'Session name' }).fill('Reloaded planning');
  await page.getByRole('button', { name: 'Start session' }).click();
  await expect(page).toHaveURL(/\/s\/lobby\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('The Lobby');

  await page.reload();
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByLabel('Current chat context')).toContainText('The Lobby');
  await expect(page.getByRole('button', { name: /Reloaded planning\s+The Lobby/ }))
    .toHaveAttribute('aria-current', 'page');

  await page.goBack();
  await expect(page).toHaveURL(/^http:\/\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');

  await page.goForward();
  await expect(page).toHaveURL(/\/s\/lobby\/[^/]+\/$/);
  await expect(page.getByLabel('Current chat context')).toContainText('The Lobby');
});

test('forwards a JSON mutation with matching Host and Origin', async ({ page }) => {
  await page.goto('/');
  const result = await page.evaluate(async () => {
    const response = await fetch('/api/v1/forums/lobby/sessions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ label: 'Proxy smoke test' }),
    });
    return { status: response.status, body: await response.json() };
  });

  expect(result.status).toBe(201);
  expect(result.body).toMatchObject({ label: 'Proxy smoke test' });
});

test('renders discovery screens from the server workspace', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');
  await expect(page.getByLabel('Current chat context')).toContainText('From: Guest');

  await page.getByRole('button', { name: 'Personas' }).click();
  await expect(page.getByText('Reader', { exact: true })).toBeVisible();
  await expect(page.getByText('The deterministic browser-test persona')).toBeVisible();

  await page.getByRole('button', { name: 'Characters' }).click();
  await expect(page.getByText('A deterministic test character')).toBeVisible();
  await page.getByRole('button', { name: /Guide/ }).click();
  await expect(page.getByRole('heading', { name: 'Guide' }).first()).toBeVisible();
  await expect(page.getByText('Answer deterministically in browser tests.')).toBeVisible();

  await page.getByRole('button', { name: 'Forums' }).click();
  await expect(page.getByRole('button', { name: /The Lobby\s+Guide/ })).toBeVisible();
});
