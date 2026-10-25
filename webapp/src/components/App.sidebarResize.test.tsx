import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';

import { fixtureClient } from '../test/fixtures';
import { App } from './App';

afterEach(() => vi.unstubAllGlobals());

it('keeps the native Pictures checkmark in sync with the Chat picture sidebar', async () => {
  const post = vi.fn();
  vi.stubGlobal('__CHA_NATIVE_CONNECTION_ID__', 'pictures-test');
  vi.stubGlobal('__CHA_NATIVE_POST__', post);
  const { container } = render(<App client={fixtureClient()} connectSessionEvents={() => ({ close: vi.fn() })} />);
  await screen.findByRole('textbox', { name: 'Message' });
  await waitFor(() => expect(screen.getByRole('textbox', { name: 'Message' })).toBeEnabled());
  const reportedStates = () => post.mock.calls
    .map(([message]) => JSON.parse(message as string) as { native_action: string; pictures: boolean })
    .filter((message) => message.native_action === 'view_state')
    .map((message) => message.pictures);
  expect(container.querySelector('.cha-picture')).toBeNull();
  expect(reportedStates()).toEqual([false]);
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
  await waitFor(() => expect(container.querySelector('.cha-picture')).toBeInTheDocument());
  expect(screen.queryByRole('button', { name: /^(Show|Hide) picture$/ })).toBeNull();
  expect(reportedStates()).toEqual([false, true]);
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
  expect(container.querySelector('.cha-picture')).toBeNull();
  expect(reportedStates()).toEqual([false, true, false]);
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
  expect(container.querySelector('.cha-picture')).toBeInTheDocument();
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
  expect(container.querySelector('.cha-picture')).toBeNull();
  expect(reportedStates()).toEqual([false, true, false, true, false]);
});

it('keeps the native Sidebar checkmark in sync across Chat and Settings', async () => {
  const post = vi.fn();
  vi.stubGlobal('__CHA_NATIVE_CONNECTION_ID__', 'sidebar-test');
  vi.stubGlobal('__CHA_NATIVE_POST__', post);
  render(<App client={fixtureClient()} connectSessionEvents={() => ({ close: vi.fn() })} />);
  await screen.findByRole('textbox', { name: 'Message' });
  await waitFor(() => expect(screen.getByRole('textbox', { name: 'Message' })).toBeEnabled());
  const reportedStates = () => post.mock.calls
    .map(([message]) => JSON.parse(message as string) as { native_action: string; sidebar: boolean })
    .filter((message) => message.native_action === 'view_state')
    .map((message) => message.sidebar);
  expect(reportedStates()).toEqual([true]);
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-sidebar'));
  expect(document.querySelector('.cha-app')).toHaveAttribute('data-sidebar', 'closed');
  expect(screen.queryByRole('button', { name: /^(Show|Hide) sidebar$/ })).toBeNull();
  expect(reportedStates()).toEqual([true, false]);
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-sidebar'));
  expect(reportedStates()).toEqual([true, false, true]);
  fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
  expect(screen.queryByRole('button', { name: /^(Show|Hide) sidebar$/ })).toBeNull();
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-sidebar'));
  expect(reportedStates()).toEqual([true, false, true, false]);
  act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-sidebar'));
  expect(document.querySelector('.cha-app')).toHaveAttribute('data-sidebar', 'open');
  expect(screen.queryByRole('button', { name: /^(Show|Hide) sidebar$/ })).toBeNull();
  expect(reportedStates()).toEqual([true, false, true, false, true]);
});

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
