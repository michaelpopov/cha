import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { expect, it, vi } from 'vitest';

import { AppErrorBoundary } from './AppErrorBoundary';

function BrokenApplication(): never {
  throw new Error('render failed');
}

it('offers to reload after an application render failure', async () => {
  const user = userEvent.setup();
  const reload = vi.fn();
  const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);

  render(
    <AppErrorBoundary onReload={reload}>
      <BrokenApplication />
    </AppErrorBoundary>,
  );

  expect(screen.getByRole('alert')).toHaveTextContent(
    'Something went wrong while showing CHA.',
  );
  await user.click(screen.getByRole('button', { name: 'Reload CHA' }));
  expect(reload).toHaveBeenCalledOnce();
  consoleError.mockRestore();
});
