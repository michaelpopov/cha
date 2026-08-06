import { expect, test } from '@playwright/test';

test('loads the shell and reloads a session-shaped deep link', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Hide sidebar' })).toBeVisible();

  await page.goto('/s/entrance/welcome/');
  await page.reload();
  await expect(page.getByText('cha', { exact: true })).toBeVisible();
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');
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
