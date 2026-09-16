import { expect, test } from '@playwright/test';

test.afterEach(async ({ request }) => {
  const response = await request.post('/api/v1/vault/switch', {
    data: { vault_name: 'E2E', password: null },
    headers: { 'Content-Type': 'application/json' },
  });
  expect(response.status()).toBe(204);
});

test('merges Projects configuration into the active vault', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByLabel('Vault')).toHaveValue('E2E');

  await page.getByLabel('Settings').click();
  await page.getByRole('button', { name: /Vaults/ }).click();
  await page.getByRole('button', { name: /Merge into active vault/ }).click();
  await page.getByLabel('Source vault').selectOption('Projects');
  await page.getByRole('button', { name: 'Merge' }).click();
  const dialog = page.getByRole('dialog');
  await expect(dialog).toContainText('Projects');
  await expect(dialog).toContainText('E2E');
  await expect(dialog).toContainText('overwrite destination files at matching paths');
  const merged = page.waitForResponse((response) => (
    response.url().includes('/api/v1/vault/merge') && response.status() === 204
  ));
  await dialog.getByRole('button', { name: 'Merge' }).click();
  await merged;
  await expect(page.getByText('Merge complete')).toBeVisible();

  await page.getByRole('button', { name: 'Characters' }).click();
  await expect(page.getByRole('button', { name: /Merge Source/ })).toBeVisible();
  await expect(page.getByLabel('Vault')).toHaveValue('E2E');
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
