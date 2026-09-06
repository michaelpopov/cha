import { expect, test } from '@playwright/test';

test.afterEach(async ({ request }) => {
  const response = await request.post('/api/v1/vault/switch', {
    data: { vault_name: 'E2E' },
    headers: { 'Content-Type': 'application/json' },
  });
  expect(response.status()).toBe(204);
});

test('switches vaults through the selector, reloads Welcome, and keeps the footer usable', async ({ page }) => {
  await page.goto('/');
  const selector = page.getByLabel('Vault');
  await expect(selector).toBeVisible();
  await expect(selector).toHaveValue('E2E');
  await expect(page.getByLabel('Settings')).toBeVisible();
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');

  const switched = page.waitForResponse((response) => (
    response.url().includes('/api/v1/vault/switch') && response.status() === 204
  ));
  await selector.selectOption('Projects');
  await switched;
  await expect(page).toHaveURL('/');
  await expect(page.getByLabel('Vault')).toHaveValue('Projects');
  await expect(page.getByLabel('Current chat context')).toContainText('Entrance');

  const bootstrap = await page.evaluate(async () => {
    const response = await fetch('/api/v1/bootstrap');
    if (!response.ok) throw new Error(`bootstrap answered ${response.status}`);
    return response.json() as Promise<{ vault_name: string; vaults: string[] }>;
  });
  expect(bootstrap.vault_name).toBe('Projects');
  expect(bootstrap.vaults).toEqual(['E2E', 'Projects']);

  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.getByLabel('Vault')).toBeVisible();
  await expect(page.getByLabel('Settings')).toBeVisible();
});
