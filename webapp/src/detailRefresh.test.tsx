import { act, fireEvent, render, renderHook, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useEffect, useState, type ReactNode } from 'react';
import { expect, it, vi } from 'vitest';

import { DetailActions, EditableTitle } from './components/DetailActions';
import {
  DetailRefreshProvider,
  StaleNotice,
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
  return { epoch: failed ? 2 : 1, refreshing: false, failed, stale: false, retry };
}

it('lets a loaded form save when only the bootstrap refresh fails', () => {
  const retry = vi.fn();
  const view = render(<SaveForm refresh={refreshValue(false, retry)} />);
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();

  view.rerender(<SaveForm refresh={refreshValue(true, retry)} />);
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  expect(screen.queryByRole('button', { name: 'Try again' })).not.toBeInTheDocument();
});

it.each(['failed', 'stale'] as const)('keeps a title Save disabled when its owner is %s', async (state) => {
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
    <DetailRefreshProvider value={{ ...refreshValue(false, retry), [state]: true }}>
      <EditableTitle available id="guide" name="Guide" onSave={onSave} subject="Character" />
    </DetailRefreshProvider>,
  );
  const save = screen.getByRole('button', { name: 'Save character name' });
  expect(save).toBeDisabled();
  fireEvent.submit(save.closest('form')!);
  expect(onSave).not.toHaveBeenCalled();
});

it.each(['failed', 'stale'] as const)('keeps a text-editor Save disabled when its owner is %s', async (state) => {
  const onSave = vi.fn(async () => {});
  const retry = vi.fn();
  function editor(blocked: boolean) {
    return (
      <DetailRefreshProvider value={{ ...refreshValue(false, retry), [state]: blocked }}>
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

function StaleForm({ incoming }: { incoming: string }) {
  const reload = useFormReload('form');
  const [saved, setSaved] = useState<string | null>(null);
  const [draft, setDraft] = useState('');
  const apply = (value: string) => {
    setSaved(value);
    setDraft(value);
  };
  useEffect(() => {
    reload.start();
    reload.loaded(incoming, apply);
  }, [incoming]);
  reload.markDirty(saved !== null && draft !== saved);
  return (
    <>
      <input aria-label="Name" onChange={(event) => setDraft(event.target.value)} value={draft} />
      {reload.stale && <StaleNotice onReload={() => reload.accept(apply)} />}
      <button type="submit" disabled={reload.blocked}>Save</button>
    </>
  );
}

it('tells a stale form why Save is off and loads the new values on request', async () => {
  const view = render(<StaleForm incoming="Guide" />);
  await userEvent.type(screen.getByLabelText('Name'), ' draft');
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();

  view.rerender(<StaleForm incoming="Mentor" />);
  expect(screen.getByRole('alert')).toHaveTextContent('This item changed. Load the new values to save.');
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  expect(screen.getByLabelText('Name')).toHaveValue('Guide draft');

  await userEvent.click(screen.getByRole('button', { name: 'Load new values' }));
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByLabelText('Name')).toHaveValue('Mentor');
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
});

it('tells a title editor why Save is off when the name changes', async () => {
  const onSave = vi.fn(async () => {});
  const title = (name: string) => (
    <EditableTitle available id="guide" name={name} onSave={onSave} subject="Character" />
  );
  const view = render(title('Guide'));
  await userEvent.click(screen.getByRole('button', { name: 'Rename Guide' }));
  await userEvent.type(screen.getByLabelText('Character name'), ' draft');

  view.rerender(title('Mentor'));
  expect(screen.getByRole('alert')).toHaveTextContent('This name changed. Cancel to load the new name.');
  expect(screen.getByRole('button', { name: 'Save character name' })).toBeDisabled();
  await userEvent.click(screen.getByRole('button', { name: 'Cancel renaming' }));
  expect(screen.getByRole('button', { name: 'Rename Mentor' })).toBeInTheDocument();
});

it('tells a text editor why Save is off when the text changes', async () => {
  const actions = (value: string) => (
    <DetailActions
      deleteMessage="Delete this file?"
      editor={{
        title: 'Edit character file',
        uploadLabel: 'Replace character file content from file',
        value,
        onSave: async () => {},
      }}
      name="CHARACTER.md"
      onDelete={async () => {}}
      subject="File"
    />
  );
  const view = render(actions('hello'));
  await userEvent.click(screen.getByRole('button', { name: 'Edit character file' }));
  await userEvent.type(screen.getByLabelText('Edit character file text'), ' draft');

  view.rerender(actions('changed'));
  expect(screen.getByRole('alert')).toHaveTextContent(
    'This text changed. Cancel and open it again to load the new text.');
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});
