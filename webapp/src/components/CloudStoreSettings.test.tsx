import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { ChaError, type ApiKeyDetail, type CloudStoreDetail } from '../api/client';
import { DetailRefreshProvider, type DetailRefreshValue } from '../detailRefresh';
import { initialAppState, type AppState } from '../state/view';
import { fixtureClient } from '../test/fixtures';
import { CloudStoreKeysScreen, CloudStoreScreen, CloudStoresScreen, cloudStoreLabels } from './CloudStoreSettings';

const key = (id: string, name: string, cloudStore: ApiKeyDetail['cloud_store']): ApiKeyDetail => ({
  id, display_name: name, has_value: true, cloud_store: cloudStore, used_by: [],
});
const detail: CloudStoreDetail = {
  id: 'vs_1', name: 'Research', description: 'First line\nSecond line', created_at: '2026-11-03T09:15:00Z',
};
const storeState = (storeId = 'vs_1', label = 'Research'): AppState => ({
  ...initialAppState,
  inspectedCloudStoreKey: { id: 'api_key_1', name: 'OpenAI work', notice: null },
  inspectedCloudStore: { id: storeId, label },
});
function deferred() {
  let reject!: (failure: unknown) => void;
  let resolve!: () => void;
  const promise = new Promise<void>((ok, fail) => { resolve = ok; reject = fail; });
  return { promise, resolve, reject };
}

describe('Cloud store settings', () => {
  it('labels only rows whose names collide with their store IDs', () => {
    expect(cloudStoreLabels([
      { id: 'vs_a', name: 'Archive' },
      { id: 'vs_1', name: 'Research' },
      { id: 'vs_2', name: 'Research' },
      { id: 'fileSearchStores/x', name: '' },
    ])).toEqual(['Archive', 'Research (vs_1)', 'Research (vs_2)', 'fileSearchStores/x']);
  });

  it('lists only keys with a cloud store and opens the selected key', async () => {
    const navigate = vi.fn();
    render(<CloudStoreKeysScreen client={fixtureClient({ listApiKeys: async () => [
      key('api_key_1', 'OpenAI work', 'openai'), key('api_key_2', 'Models', 'none'), key('api_key_3', 'Gemini personal', 'google'),
    ] })} dispatch={vi.fn()} navigate={navigate} state={initialAppState} />);
    await screen.findByRole('button', { name: 'OpenAI work' });
    expect(screen.getByRole('button', { name: 'Gemini personal' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Models' })).not.toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: 'OpenAI work' }));
    expect(navigate).toHaveBeenCalledWith({ type: 'inspect-cloud-store-key', apiKeyId: 'api_key_1', apiKeyName: 'OpenAI work' });
  });

  it('shows an empty key list', async () => {
    render(<CloudStoreKeysScreen client={fixtureClient({ listApiKeys: async () => [key('api_key_2', 'Models', 'none')] })}
      dispatch={vi.fn()} navigate={vi.fn()} state={initialAppState} />);
    expect(await screen.findByText('No cloud store keys')).toBeInTheDocument();
  });

  it('lists the stores of the selected key and opens one with its label', async () => {
    const navigate = vi.fn();
    const listCloudStores = vi.fn(async () => ({ items: [
      { id: 'vs_a', name: 'Archive' }, { id: 'vs_1', name: 'Research' }, { id: 'vs_2', name: 'Research' },
    ] }));
    render(<CloudStoresScreen client={fixtureClient({ listCloudStores })} dispatch={vi.fn()} navigate={navigate}
      state={storeState()} />);
    await userEvent.click(await screen.findByRole('button', { name: 'Research (vs_2)' }));
    expect(listCloudStores).toHaveBeenCalledWith('api_key_1');
    expect(screen.getByRole('button', { name: 'Archive' })).toBeInTheDocument();
    expect(navigate).toHaveBeenCalledWith({ type: 'inspect-cloud-store', storeId: 'vs_2', label: 'Research (vs_2)' });
    await userEvent.click(screen.getByRole('button', { name: 'Cloud stores' }));
    expect(navigate).toHaveBeenLastCalledWith({ type: 'show-settings-cloud-stores' });
  });

  it('shows an empty store list, a carried notice, and a retryable provider error', async () => {
    const state = { ...storeState(), inspectedCloudStoreKey: { id: 'api_key_1', name: 'OpenAI work', notice: 'Store unavailable. File cleanup could not be verified.' } };
    const { unmount } = render(<CloudStoresScreen client={fixtureClient()} dispatch={vi.fn()} navigate={vi.fn()} state={state} />);
    expect(await screen.findByText('No cloud stores')).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Store unavailable. File cleanup could not be verified.');
    unmount();
    let calls = 0;
    render(<CloudStoresScreen client={fixtureClient({ listCloudStores: async () => {
      if (calls++ === 0) throw new ChaError('invalid_argument', 'The saved API key was rejected.');
      return { items: [{ id: 'vs_1', name: 'Research' }] };
    } })} dispatch={vi.fn()} navigate={vi.fn()} state={storeState()} />);
    expect(await screen.findByRole('alert')).toHaveTextContent('The saved API key was rejected.');
    await userEvent.click(screen.getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('button', { name: 'Research' })).toBeInTheDocument();
  });

  it('shows editable name and description with read-only local creation time', async () => {
    const getCloudStore = vi.fn(async () => detail);
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore })} dispatch={vi.fn()} navigate={vi.fn()} state={storeState()} />);
    expect(await screen.findByLabelText('Name')).toHaveValue('Research');
    expect(getCloudStore).toHaveBeenCalledWith('api_key_1', 'vs_1');
    expect(screen.getByLabelText('Name')).not.toHaveAttribute('readonly');
    expect(screen.getByLabelText('Description')).toHaveValue('First line\nSecond line');
    expect(screen.getByLabelText('Description')).not.toHaveAttribute('readonly');
    expect(screen.getByLabelText('Created')).toHaveAttribute('readonly');
    expect(screen.getByLabelText('Created')).toHaveValue(new Intl.DateTimeFormat(undefined,
      { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(detail.created_at)));
    expect(screen.getByRole('button', { name: 'OpenAI work' })).toBeInTheDocument();
  });

  it('uses the update reply for both saved fields and the deletion name', async () => {
    let saved = detail;
    const getCloudStore = vi.fn(async () => detail);
    const updateCloudStore = vi.fn(async (_key, _id, request) => {
      saved = { ...detail, ...request };
      return saved;
    });
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore, updateCloudStore })}
      dispatch={vi.fn()} navigate={vi.fn()} state={storeState()} />);
    await screen.findByLabelText('Name');
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: '  Archive  ' } });
    fireEvent.change(screen.getByLabelText('Description'), { target: { value: 'Updated\nNotes' } });
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));
    expect(updateCloudStore).toHaveBeenCalledExactlyOnceWith('api_key_1', 'vs_1', { name: 'Archive', description: 'Updated\nNotes' });
    await waitFor(() => expect(screen.getByLabelText('Name')).toHaveValue('Archive'));
    expect(screen.getByLabelText('Description')).toHaveValue('Updated\nNotes');
    expect(getCloudStore).toHaveBeenCalledTimes(1);
    await userEvent.click(screen.getByRole('button', { name: 'Delete' }));
    expect(screen.getByRole('dialog')).toHaveTextContent('Permanently delete “Archive”');
  });

  it.each(['store', 'key'] as const)('waits for the selected %s instead of editing the previous store', async (selection) => {
    const pending = deferred();
    const next = { ...detail, id: selection === 'store' ? 'vs_2' : detail.id, name: 'Other store' };
    const getCloudStore = vi.fn().mockResolvedValueOnce(detail).mockImplementation(async () => {
      await pending.promise;
      return next;
    });
    const client = fixtureClient({ getCloudStore });
    const props = { client, dispatch: vi.fn(), navigate: vi.fn() };
    const { rerender } = render(<CloudStoreScreen {...props} state={storeState()} />);
    await screen.findByLabelText('Name');
    const nextState = selection === 'store' ? storeState(next.id, next.name) : {
      ...storeState(), inspectedCloudStoreKey: { id: 'api_key_2', name: 'Other key', notice: null },
    };
    rerender(<CloudStoreScreen {...props} state={nextState} />);
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    pending.resolve();
    expect(await screen.findByLabelText('Name')).toHaveValue('Other store');
  });

  it('preserves drafts through equal refreshes and blocks stale saves until the user reloads', async () => {
    let saved = detail;
    const client = fixtureClient({ getCloudStore: async () => saved });
    const refresh: DetailRefreshValue = { epoch: 1, refreshing: false, failed: false, stale: false, retry() {} };
    const view = (epoch: number) => <DetailRefreshProvider value={{ ...refresh, epoch }}>
      <CloudStoreScreen client={client} dispatch={vi.fn()} navigate={vi.fn()} state={storeState()} />
    </DetailRefreshProvider>;
    const { rerender } = render(view(1));
    await screen.findByLabelText('Name');
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'My draft' } });
    rerender(view(2));
    await waitFor(() => expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled());
    expect(screen.getByLabelText('Name')).toHaveValue('My draft');
    saved = { ...detail, description: 'Changed elsewhere' };
    rerender(view(3));
    expect(await screen.findByRole('alert')).toHaveTextContent('The cloud store changed.');
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(screen.getByLabelText('Name')).toHaveValue('My draft');
    await userEvent.click(screen.getByRole('button', { name: 'Try again' }));
    expect(screen.getByLabelText('Name')).toHaveValue('Research');
    expect(screen.getByLabelText('Description')).toHaveValue('Changed elsewhere');
  });

  it('keeps edits on failure and allows retrying an empty description', async () => {
    const updateCloudStore = vi.fn().mockRejectedValueOnce(new ChaError('invalid_argument', 'The saved API key was rejected.'))
      .mockResolvedValue({ ...detail, description: '' });
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => detail, updateCloudStore })}
      dispatch={vi.fn()} navigate={vi.fn()} state={storeState('fileSearchStores/a')} />);
    await screen.findByLabelText('Description');
    fireEvent.change(screen.getByLabelText('Description'), { target: { value: '' } });
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('The saved API key was rejected.');
    expect(screen.getByLabelText('Description')).toHaveValue('');
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));
    expect(updateCloudStore).toHaveBeenLastCalledWith('api_key_1', 'fileSearchStores/a', { name: 'Research', description: '' });
  });

  it('blocks empty names and disables editing and deletion during a save', async () => {
    const pending = deferred();
    const updateCloudStore = vi.fn(async () => { await pending.promise; return detail; });
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => detail, updateCloudStore })}
      dispatch={vi.fn()} navigate={vi.fn()} state={storeState()} />);
    await screen.findByLabelText('Name');
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: '   ' } });
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Archive' } });
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));
    expect(screen.getByRole('button', { name: 'Saving…' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Delete' })).toBeDisabled();
    expect(screen.getByLabelText('Name')).toBeDisabled();
    expect(screen.getByLabelText('Description')).toBeDisabled();
    pending.reject(new Error('unavailable'));
    expect(await screen.findByRole('alert')).toHaveTextContent('The store could not be saved.');
    expect(screen.getByLabelText('Name')).toHaveValue('Archive');
  });

  it('confirms deletion with the provider text and returns to the refreshed list', async () => {
    const navigate = vi.fn();
    const deleteCloudStore = vi.fn(async () => undefined);
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => detail, deleteCloudStore })}
      dispatch={vi.fn()} navigate={navigate} state={storeState('vs_1', 'Research (vs_1)')} />);
    await userEvent.click(await screen.findByRole('button', { name: 'Delete' }));
    const dialog = screen.getByRole('dialog', { name: 'Delete store?' });
    expect(dialog).toHaveTextContent('Permanently delete “Research (vs_1)” and its files? Files shared with other vector stores are removed from them too.');
    await userEvent.click(screen.getByRole('button', { name: 'Delete store' }));
    await waitFor(() => expect(navigate).toHaveBeenCalledWith({
      type: 'inspect-cloud-store-key', apiKeyId: 'api_key_1', apiKeyName: 'OpenAI work', notice: undefined,
    }));
    expect(deleteCloudStore).toHaveBeenCalledExactlyOnceWith('api_key_1', 'vs_1', expect.any(AbortSignal));
  });

  it('omits the shared-file text for Gemini stores', async () => {
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => ({ ...detail, id: 'fileSearchStores/a' }) })}
      dispatch={vi.fn()} navigate={vi.fn()} state={storeState('fileSearchStores/a', 'Research')} />);
    await userEvent.click(await screen.findByRole('button', { name: 'Delete' }));
    expect(screen.getByRole('dialog', { name: 'Delete store?' })).toHaveTextContent(/^Delete store\?Permanently delete “Research” and its files\?CancelDelete store$/);
  });

  it.each(['button', 'escape'] as const)('stops a pending deletion with Cancel (%s)', async (how) => {
    const pending = deferred();
    let signal: AbortSignal | undefined;
    const deleteCloudStore = vi.fn((_key: string, _store: string, abort?: AbortSignal) => {
      signal = abort;
      return pending.promise;
    });
    const navigate = vi.fn();
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => detail, deleteCloudStore })}
      dispatch={vi.fn()} navigate={navigate} state={storeState()} />);
    await userEvent.click(await screen.findByRole('button', { name: 'Delete' }));
    await userEvent.click(screen.getByRole('button', { name: 'Delete store' }));
    const deleting = await screen.findByRole('button', { name: 'Deleting…' });
    expect(deleting).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled();
    if (how === 'button') await userEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    else fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    expect(signal?.aborted).toBe(true);
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Deletion stopped. Some files may already be deleted.');
    pending.reject(new DOMException('The operation was aborted.', 'AbortError'));
    await Promise.resolve();
    expect(navigate).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Delete' })).toBeEnabled();
  });

  it('returns to the list with the message when the store is gone', async () => {
    const navigate = vi.fn();
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => detail,
      deleteCloudStore: async () => { throw new ChaError('not_found', 'Store unavailable. File cleanup could not be verified.'); } })}
    dispatch={vi.fn()} navigate={navigate} state={storeState()} />);
    await userEvent.click(await screen.findByRole('button', { name: 'Delete' }));
    await userEvent.click(screen.getByRole('button', { name: 'Delete store' }));
    await waitFor(() => expect(navigate).toHaveBeenCalledWith({
      type: 'inspect-cloud-store-key', apiKeyId: 'api_key_1', apiKeyName: 'OpenAI work',
      notice: 'Store unavailable. File cleanup could not be verified.',
    }));
  });

  it('keeps the details with the error and allows another deletion after a failure', async () => {
    let calls = 0;
    const deleteCloudStore = vi.fn(async () => {
      if (calls++ === 0) throw new ChaError('invalid_argument', 'The store still contains documents. Retry deletion.');
    });
    const navigate = vi.fn();
    render(<CloudStoreScreen client={fixtureClient({ getCloudStore: async () => detail, deleteCloudStore })}
      dispatch={vi.fn()} navigate={navigate} state={storeState()} />);
    await userEvent.click(await screen.findByRole('button', { name: 'Delete' }));
    await userEvent.click(screen.getByRole('button', { name: 'Delete store' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('The store still contains documents. Retry deletion.');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(navigate).not.toHaveBeenCalled();
    await userEvent.click(screen.getByRole('button', { name: 'Delete' }));
    await userEvent.click(screen.getByRole('button', { name: 'Delete store' }));
    await waitFor(() => expect(navigate).toHaveBeenCalled());
    expect(deleteCloudStore).toHaveBeenCalledTimes(2);
  });
});
