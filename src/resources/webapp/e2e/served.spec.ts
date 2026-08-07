import { expect, test } from '@playwright/test';

// These load the built bundle from `chaweb` itself. No development server is in
// the path, so they are what proves the shipped application actually runs: the
// production build, the asset routes, and the Content Security Policy together.

test('serves the built shell at the root without violating its own policy', async ({ page }) => {
  const problems: string[] = [];
  page.on('console', (message) => {
    if (message.type() === 'error') problems.push(message.text());
  });
  page.on('pageerror', (error) => problems.push(error.message));

  const response = await page.goto('/');
  expect(response?.status()).toBe(200);
  expect(response?.headers()['cache-control']).toBe('no-cache');
  expect(response?.headers()['content-security-policy']).toContain("script-src 'self'");

  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Forums' })).toBeEnabled();
  // A blocked script or stylesheet reports itself here rather than as a blank page.
  expect(problems).toEqual([]);
});

test('serves the same shell at a session deep link', async ({ page, request }) => {
  // The initial IDs come from the server rather than being written in here, so
  // this stays a test of the deep link and not of the built-in naming.
  const bootstrap = await (await request.get('/api/v1/bootstrap')).json();
  const route = `/s/${bootstrap.initial_forum_id}/${bootstrap.initial_session_id}/`;

  await page.goto(route);
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');
  await expect(page).toHaveURL(new RegExp(`${route}$`));
});

test('serves hashed assets as immutable', async ({ page }) => {
  await page.goto('/');
  const source = await page.locator('head script[type="module"]').getAttribute('src');
  expect(source).toMatch(/^\/assets\/.+\.js$/);

  const asset = await page.request.get(source ?? '');
  expect(asset.status()).toBe(200);
  expect(asset.headers()['cache-control']).toBe('public, max-age=31536000, immutable');
});
