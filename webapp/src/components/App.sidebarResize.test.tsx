import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';

import { fixtureClient } from '../test/fixtures';
import { App } from './App';

afterEach(() => vi.unstubAllGlobals());

it('resizes the sidebar with arrow keys and clamps it to the available width', async () => {
  vi.stubGlobal('innerWidth', 1280);
  const { container } = render(
    <App client={fixtureClient()} connectSessionEvents={() => ({ close: vi.fn() })} />,
  );
  await waitFor(() => expect(screen.getByRole('button', { name: 'Settings' })).toBeEnabled());
  const divider = screen.getByRole('separator', { name: 'Resize sidebar' });
  const app = container.querySelector<HTMLElement>('.cha-app')!;
  const initialWidth = Number(divider.getAttribute('aria-valuenow'));

  divider.focus();
  fireEvent.keyDown(divider, { key: 'ArrowRight' });
  expect(divider).toHaveAttribute('aria-valuenow', String(initialWidth + 16));
  expect(app.style.getPropertyValue('--cha-sidebar-width')).toBe(`${initialWidth + 16}px`);
  fireEvent.keyDown(divider, { key: 'ArrowLeft' });
  expect(divider).toHaveAttribute('aria-valuenow', String(initialWidth));

  for (let i = 0; i < 80; ++i) fireEvent.keyDown(divider, { key: 'ArrowLeft' });
  expect(divider).toHaveAttribute('aria-valuenow', '192');
  expect(app.style.getPropertyValue('--cha-sidebar-width')).toBe('192px');

  for (let i = 0; i < 80; ++i) fireEvent.keyDown(divider, { key: 'ArrowRight' });
  expect(divider).toHaveAttribute('aria-valuenow', '960');
  expect(app.style.getPropertyValue('--cha-sidebar-width')).toBe('960px');

  vi.stubGlobal('innerWidth', 800);
  fireEvent(window, new Event('resize'));
  expect(divider).toHaveAttribute('aria-valuemax', '480');
  expect(divider).toHaveAttribute('aria-valuenow', '480');
  expect(app.style.getPropertyValue('--cha-sidebar-width')).toBe('480px');
});
