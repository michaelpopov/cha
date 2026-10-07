import { act, fireEvent, render, renderHook, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import type { ReactNode } from 'react';
import { expect, it, vi } from 'vitest';

import { DetailActions, EditableTitle } from './components/DetailActions';
import {
  DetailRefreshProvider,
  useFormReload,
  type DetailRefreshValue,
} from './detailRefresh';

function SaveForm({ refresh }: { refresh: DetailRefreshValue }) {
  return (
    <DetailRefreshProvider value={refresh}>
      <SaveControls />
    </DetailRefreshProvider>
  );
}

function SaveControls() {
  const reload = useFormReload('form');
  return (
    <>
      <button type="submit" disabled={reload.blocked}>Save</button>
      {reload.refreshFailed && (
        <button type="button" onClick={reload.retry}>Try again</button>
      )}
    </>
  );
}

function refreshValue(failed: boolean, retry: () => void): DetailRefreshValue {
  return { epoch: failed ? 2 : 1, refreshing: false, failed, retry };
}

it('lets a loaded form save when only the bootstrap refresh fails', () => {
  const retry = vi.fn();
  const view = render(<SaveForm refresh={refreshValue(false, retry)} />);
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();

  view.rerender(<SaveForm refresh={refreshValue(true, retry)} />);
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  expect(screen.queryByRole('button', { name: 'Try again' })).not.toBeInTheDocument();
});

it('keeps a title Save disabled when its owner refresh fails', async () => {
  const onSave = vi.fn(async () => {});
  const retry = vi.fn();
  const view = render(
    <DetailRefreshProvider value={refreshValue(false, retry)}>
      <EditableTitle available id="guide" name="Guide" onSave={onSave} subject="Character" />
    </DetailRefreshProvider>,
  );
  await userEvent.click(screen.getByRole('button', { name: 'Rename Guide' }));
  const name = screen.getByLabelText('Character name');
  await userEvent.clear(name);
  await userEvent.type(name, 'Mentor');
  expect(screen.getByRole('button', { name: 'Save character name' })).toBeEnabled();

  view.rerender(
    <DetailRefreshProvider value={refreshValue(true, retry)}>
      <EditableTitle available id="guide" name="Guide" onSave={onSave} subject="Character" />
    </DetailRefreshProvider>,
  );
  const save = screen.getByRole('button', { name: 'Save character name' });
  expect(save).toBeDisabled();
  fireEvent.submit(save.closest('form')!);
  expect(onSave).not.toHaveBeenCalled();
});

it('keeps a text-editor Save disabled when its owner refresh fails', async () => {
  const onSave = vi.fn(async () => {});
  const retry = vi.fn();
  function editor(failed: boolean) {
    return (
      <DetailRefreshProvider value={refreshValue(failed, retry)}>
        <DetailActions
          deleteMessage="Delete this file?"
          editor={{
            title: 'Edit character file',
            uploadLabel: 'Replace character file content from file',
            value: 'hello',
            onSave,
          }}
          name="CHARACTER.md"
          onDelete={async () => {}}
          subject="File"
        />
      </DetailRefreshProvider>
    );
  }
  const view = render(editor(false));
  await userEvent.click(screen.getByRole('button', { name: 'Edit character file' }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();

  view.rerender(editor(true));
  const save = screen.getByRole('button', { name: 'Save' });
  expect(save).toBeDisabled();
  fireEvent.submit(save.closest('form')!);
  expect(onSave).not.toHaveBeenCalled();
});

it('uses only the form refetch for Save, inner controls, and retry', () => {
  const retry = vi.fn();
  const refresh = refreshValue(true, retry);
  const { result } = renderHook(() => useFormReload('form'), {
    wrapper: ({ children }: { children: ReactNode }) => (
      <DetailRefreshProvider value={refresh}>{children}</DetailRefreshProvider>
    ),
  });

  act(() => {
    result.current.start();
    result.current.loaded({ name: 'Guide' }, () => {});
  });
  expect(result.current.blocked).toBe(false);
  expect(result.current.context.failed).toBe(false);
  act(() => {
    const background = result.current.start();
    result.current.fail(background);
  });
  expect(result.current.blocked).toBe(true);
  expect(result.current.refreshFailed).toBe(true);
  expect(result.current.context.failed).toBe(true);
  expect(result.current.attempt).toBe(0);

  act(() => result.current.context.retry());
  expect(retry).not.toHaveBeenCalled();
  expect(result.current.attempt).toBe(1);
});
