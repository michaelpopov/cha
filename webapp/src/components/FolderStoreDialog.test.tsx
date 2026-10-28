import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { ChaError, type FolderStoreOperation, type NoteItem, type NotesClient } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { FolderStoreDialog } from './FolderStoreDialog';
import type { DeleteNoteItem } from './NotesTree';

type Operation = Extract<FolderStoreOperation, { present: true }>;
const source: NoteItem = { id: 11, parent_id: null, kind: 'folder', name: 'Research', has_compressed: false, read_only: false };
const stored: NoteItem = { ...source, store: { service: 'openai', id: 77, state: 'incomplete' } };
const choices = [
  { service: 'openai' as const, api_key_id: 'key1', api_key_name: 'Personal OpenAI' },
  { service: 'google' as const, api_key_id: 'key2', api_key_name: 'Personal Google' },
];
const operation = (changes: Partial<Operation> = {}): Operation => ({
  present: true, operation_id: 5, kind: 'upload', phase: 'uploading', completed: 1, total: 3,
  error: '', result: '', terminal: false, known_cleanup_remains: false, folder_deleted: false, ...changes,
});
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(ok => { resolve = ok; });
  return { promise, resolve };
}
function setup(overrides: Partial<NotesClient> = {}, props: Partial<Parameters<typeof FolderStoreDialog>[0]> = {}) {
  const onClose = vi.fn();
  const refresh = vi.fn(async () => undefined);
  const client = fixtureClient({ contextEpoch: () => 1, storeChoices: async () => ({ choices }), storeClose: async () => undefined, ...overrides });
  const onDeleteItem = vi.fn<DeleteNoteItem>(async (_id, _listing, refresh, remove) => {
    if (!await remove!()) return 'failed';
    await refresh();
    return 'deleted';
  });
  const result = render(<FolderStoreDialog client={client} source={source} mode="upload" epoch={1}
    listing={[source]} refresh={refresh} onDeleteItem={onDeleteItem} onClose={onClose} {...props} />);
  return { ...result, client, onClose, refresh, onDeleteItem };
}
async function upload() {
  await waitFor(() => expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Upload' }));
}
const terminal = (changes: Partial<Operation> = {}) => operation({ phase: 'failed', terminal: true, result: 'error', error: 'Could not index “Experiment”.', ...changes });

describe('Folder store dialog', () => {
  it('has an accessible name without a visible title and selects one key for either provider', async () => {
    setup({ storeChoices: async () => ({ choices: [...choices, choices[0]] }) });
    const dialog = screen.getByRole('dialog', { name: 'Upload folder to store' });
    expect(dialog).not.toHaveTextContent('Upload folder to store');
    await waitFor(() => expect(screen.getByLabelText('API key')).toHaveValue('key1'));
    expect(screen.getAllByRole('option', { name: 'Personal OpenAI' })).toHaveLength(1);
    fireEvent.change(screen.getByLabelText('Provider'), { target: { value: 'google' } });
    expect(screen.getByLabelText('API key')).toHaveValue('key2');
  });
  it('requires a choice with several keys and clears it on provider change', async () => {
    setup({ storeChoices: async () => ({ choices: [...choices, { ...choices[0], api_key_id: 'key3', api_key_name: 'Other' }] }) });
    await screen.findByLabelText('API key');
    expect(screen.getByRole('button', { name: 'Upload' })).toBeDisabled();
    fireEvent.change(screen.getByLabelText('API key'), { target: { value: 'key3' } });
    expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled();
    fireEvent.change(screen.getByLabelText('Provider'), { target: { value: 'google' } });
    expect(screen.getByLabelText('API key')).toHaveValue('key2');
    fireEvent.change(screen.getByLabelText('Provider'), { target: { value: 'openai' } });
    expect(screen.getByLabelText('API key')).toHaveValue('');
  });
  it('directs missing keys to Settings and disables upload', async () => {
    setup({ storeChoices: async () => ({ choices: [] }) });
    expect(await screen.findByRole('alert')).toHaveTextContent('Add a compatible provider and saved key in Settings.');
    expect(screen.getByRole('button', { name: 'Upload' })).toBeDisabled();
  });
  it('cancels before submission without calling any operation command', async () => {
    const start = vi.fn(); const cancel = vi.fn(); const close = vi.fn();
    const { onClose } = setup({ storeUpload: start, storeCancel: cancel, storeClose: close });
    await screen.findByLabelText('Provider');
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    expect(onClose).toHaveBeenCalledOnce();
    expect(start).not.toHaveBeenCalled(); expect(cancel).not.toHaveBeenCalled(); expect(close).not.toHaveBeenCalled();
  });
  it('uploads only folder identity and saved key, blocks duplicates, and waits for indexing and ready', async () => {
    let state: FolderStoreOperation = { present: false };
    const start = vi.fn(async () => { state = operation(); return { operation_id: 5 }; });
    const close = vi.fn(async () => undefined);
    const save = vi.fn(); const get = vi.fn();
    const { onClose } = setup({ storeUpload: start, storeOperation: async () => state, storeClose: close, saveNote: save, getNote: get });
    await upload();
    expect(await screen.findByRole('status')).toHaveTextContent('Uploading 1 of 3 notes…');
    expect(start).toHaveBeenCalledExactlyOnceWith(11, 'openai', 'key1', 1);
    expect(screen.queryByLabelText('Provider')).not.toBeInTheDocument();
    expect(onClose).not.toHaveBeenCalled();
    state = operation({ phase: 'indexing', completed: 3 });
    await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Indexing notes…'));
    expect(onClose).not.toHaveBeenCalled();
    state = terminal({ phase: 'completed', result: 'success', error: '' });
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
    expect(close).toHaveBeenCalledWith(5, 1); expect(save).not.toHaveBeenCalled(); expect(get).not.toHaveBeenCalled();
  });
  it('restores editable fields after asynchronous preflight error', async () => {
    let state: FolderStoreOperation = { present: false };
    const close = vi.fn(async () => { state = { present: false }; });
    setup({ storeUpload: async () => { state = terminal({ result: 'preflight', error: 'All saved notes are empty.' }); return { operation_id: 5 }; },
      storeOperation: async () => state, storeClose: close });
    await upload();
    expect(await screen.findByRole('alert')).toHaveTextContent('All saved notes are empty.');
    expect(screen.getByLabelText('Provider')).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled();
    expect(close).toHaveBeenCalledWith(5, 1);
  });
  it.each([false, true])('keeps an upload failure visible with cleanup remaining = %s', async remaining => {
    const { onClose } = setup({ storeOperation: async () => terminal({ known_cleanup_remains: remaining }) });
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not index');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
    expect(!!screen.queryByRole('button', { name: 'Retry cleanup' })).toBe(remaining);
    expect(onClose).not.toHaveBeenCalled();
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
  });
  it.each(['preparing', 'uploading', 'indexing'] as const)('Escape requests cancellation during %s and waits for cleanup', async phase => {
    let state: FolderStoreOperation = operation({ phase });
    const cancel = vi.fn(async () => undefined);
    const { onClose } = setup({ storeOperation: async () => state, storeCancel: cancel });
    await screen.findByRole('status');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled());
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    await waitFor(() => expect(cancel).toHaveBeenCalledExactlyOnceWith(5, 1));
    expect(screen.getByRole('status')).toHaveTextContent('Cleaning up…');
    // Model a stale read, or an allocation still returning its ID, after Cancel.
    await act(() => new Promise(resolve => setTimeout(resolve, 650)));
    expect(screen.getByRole('status')).toHaveTextContent('Cleaning up…');
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    expect(cancel).toHaveBeenCalledTimes(1);
    expect(onClose).not.toHaveBeenCalled();
    state = terminal({ phase: 'cancelled', result: 'cancelled', error: '' });
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
  });
  it('keeps uncertainty visible until Close', async () => {
    const { onClose, refresh } = setup({ storeOperation: async () => terminal({ result: 'warning', phase: 'warning', error: '' }) });
    expect(await screen.findByRole('alert')).toHaveTextContent('Some remote data could not be confirmed deleted.');
    await waitFor(() => expect(refresh).toHaveBeenCalled());
    expect(onClose).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
  });
  it('removes an incomplete record with its captured ID and never offers Delete anyway', async () => {
    let state: FolderStoreOperation = { present: false };
    const remove = vi.fn(async () => { state = operation({ kind: 'remove', phase: 'removing' }); return { operation_id: 5 }; });
    const { onClose } = setup({ storeOperation: async () => state, storeRemove: remove }, { source: stored, mode: 'remove' });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Remove' })).toBeEnabled());
    fireEvent.click(screen.getByRole('button', { name: 'Remove' }));
    await waitFor(() => expect(remove).toHaveBeenCalledExactlyOnceWith(11, 77, 1));
    expect(screen.getByRole('status')).toHaveTextContent('Removing store…');
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    expect(onClose).not.toHaveBeenCalled();
    state = terminal({ kind: 'remove', known_cleanup_remains: true, error: 'The saved API key is missing.' });
    expect(await screen.findByRole('alert')).toHaveTextContent('The saved API key is missing.');
    expect(screen.queryByRole('button', { name: 'Delete anyway' })).not.toBeInTheDocument();
  });
  it('recovers an accepted start after a timeout without a second allocation', async () => {
    let state: FolderStoreOperation = { present: false };
    const start = vi.fn(async () => { state = operation({ phase: 'indexing' }); throw new Error('raw secret provider body'); });
    const { onClose } = setup({ storeUpload: start, storeOperation: async () => state });
    await upload();
    await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Indexing notes…'));
    expect(document.body).not.toHaveTextContent('raw secret');
    state = terminal({ result: 'success', phase: 'completed', error: '' });
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
    expect(start).toHaveBeenCalledOnce();
  });
  it('blocks a fresh submission if recovery fails', async () => {
    const start = vi.fn();
    setup({ storeUpload: start, storeOperation: async () => { throw new Error('raw secret'); } });
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not recover store status');
    expect(screen.queryByRole('button', { name: 'Upload' })).not.toBeInTheDocument();
    expect(start).not.toHaveBeenCalled(); expect(document.body).not.toHaveTextContent('raw secret');
  });
  it('does not poll after unmount or cancel the backend operation', async () => {
    const read = vi.fn(async () => operation()); const cancel = vi.fn();
    const { unmount } = setup({ storeOperation: read, storeCancel: cancel });
    await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Uploading'));
    unmount(); const calls = read.mock.calls.length;
    await act(() => new Promise(resolve => setTimeout(resolve, 550)));
    expect(read).toHaveBeenCalledTimes(calls); expect(cancel).not.toHaveBeenCalled();
  });
  it('discards a stale reply and sends no command into a new vault', async () => {
    let epoch = 1;
    const status = deferred<FolderStoreOperation>(); const close = vi.fn(); const start = vi.fn();
    const { onClose } = setup({ contextEpoch: () => epoch, storeOperation: () => status.promise, storeClose: close, storeUpload: start });
    epoch = 2;
    await act(async () => status.resolve(terminal({ result: 'success' })));
    expect(onClose).toHaveBeenCalled(); expect(close).not.toHaveBeenCalled(); expect(start).not.toHaveBeenCalled();
  });
  it('counts own, descendant, incomplete, and empty stores but excludes an ancestor', async () => {
    const start = vi.fn();
    const child = { ...stored, id: 12, parent_id: 11 };
    const parent = { ...stored, id: 10 };
    setup({ folderDelete: start }, { source: { ...stored, parent_id: 10 }, listing: [parent, { ...stored, parent_id: 10 }, child], mode: 'delete' });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Delete' })).toBeEnabled());
    expect(screen.getByText('2 remote stores will also be removed.')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(start).not.toHaveBeenCalled();
  });
  it.each(['Retry', 'Delete anyway'])('uses the same failed identity for %s under the delete guard', async action => {
    let state: FolderStoreOperation = terminal({ kind: 'folder_delete', known_cleanup_remains: true });
    const start = vi.fn(async () => { state = terminal({ kind: 'folder_delete', result: 'warning', phase: 'warning', folder_deleted: true, error: '' }); return { operation_id: 5 }; });
    const { onDeleteItem, onClose, refresh } = setup({ storeOperation: async () => state, folderDelete: start }, { source: stored, mode: 'delete' });
    await waitFor(() => expect(screen.getByRole('button', { name: action })).toBeEnabled());
    expect(screen.getByText(/Remote data and charges may remain/)).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: action }));
    await waitFor(() => expect(start).toHaveBeenCalledExactlyOnceWith({ operation_id: 5, delete_anyway: action === 'Delete anyway' }, 1));
    await waitFor(() => expect(refresh).toHaveBeenCalled());
    expect(onDeleteItem).toHaveBeenCalledTimes(2);
    expect(onClose).not.toHaveBeenCalled();
    expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Some remote data');
  });
  it('Escape releases a failed deletion reservation and retains notes', async () => {
    const close = vi.fn(async () => undefined); const start = vi.fn();
    const { onClose } = setup({ storeOperation: async () => terminal({ kind: 'folder_delete', known_cleanup_remains: true }), storeClose: close, folderDelete: start }, { mode: 'delete' });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled());
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: true, cancelable: true }));
    await waitFor(() => expect(close).toHaveBeenCalledExactlyOnceWith(5, 1));
    expect(onClose).toHaveBeenCalledOnce(); expect(start).not.toHaveBeenCalled();
  });
  it('submits the confirmed count and keeps a pending delete under the editor guard', async () => {
    let state: FolderStoreOperation = { present: false };
    const start = vi.fn(async () => { state = operation({ kind: 'folder_delete', phase: 'removing' }); return { operation_id: 5 }; });
    const { onClose, refresh } = setup({ storeOperation: async () => state, folderDelete: start }, { mode: 'delete', source: stored, listing: [stored] });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Delete' })).toBeEnabled());
    fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
    await waitFor(() => expect(start).toHaveBeenCalledExactlyOnceWith({ folder_id: 11, confirmed_store_count: 1 }, 1));
    expect(onClose).not.toHaveBeenCalled(); expect(refresh).not.toHaveBeenCalled();
    state = terminal({ kind: 'folder_delete', result: 'folder_deleted', folder_deleted: true, error: '', phase: 'completed' });
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
  });
  it('keeps a failed local delete visible and never treats it as committed', async () => {
    const close = vi.fn();
    const { onClose } = setup({ storeOperation: async () => terminal({ kind: 'folder_delete', error: 'The local transaction failed.' }), storeClose: close }, { mode: 'delete' });
    expect(await screen.findByRole('alert')).toHaveTextContent('The local transaction failed.');
    expect(onClose).not.toHaveBeenCalled(); expect(close).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  });
});

it('allows cancellation of an upload started in this dialog after acceptance', async () => {
  let state: FolderStoreOperation = { present: false };
  const cancel = vi.fn(async () => { state = operation({ phase: 'cleaning' }); });
  const { onClose } = setup({ storeOperation: async () => state,
    storeUpload: async () => { state = operation(); return { operation_id: 5 }; }, storeCancel: cancel });
  await upload();
  await waitFor(() => expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
  await waitFor(() => expect(cancel).toHaveBeenCalledExactlyOnceWith(5, 1));
  expect(screen.getByRole('status')).toHaveTextContent('Cleaning up…');
  expect(onClose).not.toHaveBeenCalled();
  state = terminal({ phase: 'cancelled', result: 'cancelled', error: '' });
  await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
});

it('reloads compatible choices after configuration changes reject submission', async () => {
  let reads = 0;
  const start = vi.fn(async () => { throw new ChaError('invalid_argument', 'The saved key changed.'); });
  setup({ storeUpload: start, storeChoices: async () => ({ choices: reads++ === 0 ? choices : [] }) });
  await upload();
  await screen.findByText('The saved key changed.');
  await waitFor(() => expect(screen.getByLabelText('API key')).toHaveValue(''));
  expect(screen.getByRole('button', { name: 'Upload' })).toBeDisabled();
  expect(start).toHaveBeenCalledOnce();
});

it('keeps the failed reservation visible when Close fails', async () => {
  const { onClose } = setup({ storeOperation: async () => terminal({ kind: 'folder_delete', known_cleanup_remains: true }),
    storeClose: async () => { throw new ChaError('invalid_argument', 'Cleanup IDs could not be saved.'); } }, { mode: 'delete' });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
  expect(await screen.findByText('Cleanup IDs could not be saved.')).toBeInTheDocument();
  expect(onClose).not.toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
});

it('does not poll while editable or after terminal failure', async () => {
  const read = vi.fn<NotesClient['storeOperation']>().mockResolvedValue({ present: false });
  const { unmount } = setup({ storeOperation: read });
  await screen.findByLabelText('Provider');
  await act(() => new Promise(resolve => setTimeout(resolve, 550)));
  expect(read).toHaveBeenCalledTimes(1);
  unmount();
  read.mockClear().mockResolvedValue(terminal());
  setup({ storeOperation: read });
  await screen.findByRole('button', { name: 'Close' });
  await act(() => new Promise(resolve => setTimeout(resolve, 550)));
  expect(read).toHaveBeenCalledTimes(1);
});

it('keeps cancellation cleanup failure visible with a cleanup retry action', async () => {
  let state: FolderStoreOperation = operation({ phase: 'cleaning' });
  const { onClose } = setup({ storeOperation: async () => state });
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Cleaning up…'));
  state = terminal({ known_cleanup_remains: true, error: 'Could not delete the uploaded file.' });
  expect(await screen.findByRole('alert')).toHaveTextContent('Could not delete the uploaded file.');
  expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeInTheDocument();
  expect(onClose).not.toHaveBeenCalled();
});

it('dismisses a disconnected dialog without cancelling or acknowledging the old operation', async () => {
  let connected = true;
  const cancel = vi.fn(); const close = vi.fn();
  const { onClose } = setup({ contextEpoch: () => {
    if (!connected) throw new ChaError('application_unavailable', 'The connection to CHA ended.');
    return 1;
  }, storeOperation: async () => operation(), storeCancel: cancel, storeClose: close });
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Uploading'));
  connected = false;
  await waitFor(() => expect(onClose).toHaveBeenCalled());
  expect(cancel).not.toHaveBeenCalled(); expect(close).not.toHaveBeenCalled();
});

it('allows closing the view if the captured operation is no longer current without acknowledging another operation', async () => {
  const read = vi.fn<NotesClient['storeOperation']>().mockResolvedValueOnce(operation()).mockResolvedValue({ present: false });
  const close = vi.fn(); const start = vi.fn();
  const { onClose } = setup({ storeOperation: read, storeClose: close, storeUpload: start });
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Uploading'));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
  expect(screen.getByRole('alert')).toHaveTextContent('The store operation changed.');
  fireEvent.click(screen.getByRole('button', { name: 'Close' }));
  expect(onClose).toHaveBeenCalledOnce(); expect(close).not.toHaveBeenCalled(); expect(start).not.toHaveBeenCalled();
});

it('keeps a failed upload and its unconfirmed remote outcome visible until Close', async () => {
  const message = 'The store connection failed. Some remote data could not be confirmed deleted. See Events for manual-cleanup details.';
  const close = vi.fn(async () => undefined);
  const { onClose } = setup({ storeOperation: async () => terminal({ error: message }), storeClose: close });
  expect(await screen.findByRole('alert')).toHaveTextContent(message);
  await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
  expect(screen.queryByRole('button', { name: 'Retry cleanup' })).not.toBeInTheDocument();
  expect(onClose).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('button', { name: 'Close' }));
  await waitFor(() => expect(close).toHaveBeenCalledExactlyOnceWith(5, 1));
  expect(onClose).toHaveBeenCalledOnce();
});

it('clears a temporary status-read error after a successful poll', async () => {
  const read = vi.fn<NotesClient['storeOperation']>().mockResolvedValue(operation())
    .mockResolvedValueOnce(operation()).mockRejectedValueOnce(new Error('temporary disconnect'));
  setup({ storeOperation: read });
  expect(await screen.findByRole('alert')).toHaveTextContent('Could not read store status. Checking again…');
  await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
  expect(screen.getByRole('status')).toHaveTextContent('Uploading 1 of 3 notes…');
});

it('does not clear a cancellation error when the next status read succeeds', async () => {
  setup({ storeOperation: async () => operation(), storeCancel: async () => { throw new ChaError('command_timeout', 'Could not confirm cancellation.'); } });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
  expect(await screen.findByRole('alert')).toHaveTextContent('Could not confirm cancellation.');
  await act(() => new Promise(resolve => setTimeout(resolve, 550)));
  expect(screen.getByRole('alert')).toHaveTextContent('Could not confirm cancellation.');
});

it('offers cleanup retry after a failed Remove', async () => {
  setup({ storeOperation: async () => terminal({ kind: 'remove', known_cleanup_remains: true,
    error: 'Could not remove “Research” from OpenAI: The saved API key is missing or empty.' }) }, { source: stored, mode: 'remove' });
  expect(await screen.findByRole('alert')).toHaveTextContent('Could not remove “Research” from OpenAI');
  expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeInTheDocument();
});

it('retains a failed Delete anyway attempt after its local transaction is rejected', async () => {
  const start = vi.fn<NotesClient['folderDelete']>().mockRejectedValue(new ChaError('internal_error', 'The local transaction failed.'));
  const { onClose } = setup({ storeOperation: async () => terminal({ kind: 'folder_delete', known_cleanup_remains: true }), folderDelete: start }, { mode: 'delete' });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Delete anyway' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Delete anyway' }));
  expect(await screen.findByText('The local transaction failed.')).toBeInTheDocument();
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry' })).toBeEnabled());
  expect(start).toHaveBeenCalledExactlyOnceWith({ operation_id: 5, delete_anyway: true }, 1);
  expect(onClose).not.toHaveBeenCalled();
});

it.each(['upload', 'remove'] as const)('retries cleanup after Close fails for a recovered %s without requiring upload keys', async kind => {
  let state: FolderStoreOperation = terminal({ kind, known_cleanup_remains: true });
  const close = vi.fn(async (id: number) => {
    if (id === 5) throw new ChaError('invalid_argument', 'Cleanup IDs could not be saved. Retry cleanup or try Close again.');
  });
  const accepted = deferred<{ operation_id: number }>();
  const remove = vi.fn(async () => accepted.promise);
  const upload = vi.fn(); const getChoices = vi.fn();
  const { onClose } = setup({ storeOperation: async () => state, storeRemove: remove, storeClose: close,
    storeUpload: upload, storeChoices: getChoices }, { source: kind === 'upload' ? source : stored, listing: [stored] });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Close' }));
  expect(await screen.findByText('Cleanup IDs could not be saved. Retry cleanup or try Close again.')).toBeInTheDocument();
  const retry = screen.getByRole('button', { name: 'Retry cleanup' });
  fireEvent.click(retry);
  fireEvent.click(retry);
  await waitFor(() => expect(remove).toHaveBeenCalledExactlyOnceWith(11, 77, 1));
  expect(retry).toBeDisabled();
  expect(close).toHaveBeenCalledExactlyOnceWith(5, 1);
  expect(upload).not.toHaveBeenCalled(); expect(getChoices).not.toHaveBeenCalled();
  state = operation({ operation_id: 6, kind: 'remove', phase: 'removing' });
  await act(async () => accepted.resolve({ operation_id: 6 }));
  expect(screen.getByRole('dialog', { name: 'Remove folder from store' })).toBeInTheDocument();
  expect(screen.getByRole('status')).toHaveTextContent('Removing store…');
  expect(screen.queryByRole('button', { name: 'Retry cleanup' })).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Delete anyway' })).not.toBeInTheDocument();
  state = terminal({ operation_id: 6, kind: 'remove', phase: 'warning', result: 'warning', error: '' });
  await waitFor(() => expect(screen.getByRole('alert')).toHaveTextContent('Some remote data could not be confirmed deleted.'));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
  expect(onClose).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('button', { name: 'Close' }));
  await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
  expect(close).toHaveBeenLastCalledWith(6, 1);
});

it('keeps cleanup retry available when its start fails', async () => {
  const remove = vi.fn(async () => { throw new ChaError('operation_cancelled', 'The operation could not be started.'); });
  const { onClose } = setup({ storeOperation: async () => terminal({ known_cleanup_remains: true }), storeRemove: remove },
    { listing: [stored] });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Retry cleanup' }));
  expect(await screen.findByText('The operation could not be started.')).toBeInTheDocument();
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  expect(remove).toHaveBeenCalledExactlyOnceWith(11, 77, 1);
  expect(onClose).not.toHaveBeenCalled();
});

it('does not retry cleanup for an operation replaced since the displayed failure', async () => {
  let state: FolderStoreOperation = terminal({ known_cleanup_remains: true });
  const remove = vi.fn();
  setup({ storeOperation: async () => state, storeRemove: remove }, { listing: [stored] });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  state = terminal({ operation_id: 9, known_cleanup_remains: true });
  fireEvent.click(screen.getByRole('button', { name: 'Retry cleanup' }));
  expect(await screen.findByText('The store operation changed. Close this dialog and reopen the folder menu.')).toBeInTheDocument();
  expect(remove).not.toHaveBeenCalled();
});

it('does not use a stale source store when the current listing has no store', async () => {
  const remove = vi.fn();
  setup({ storeOperation: async () => terminal({ kind: 'remove', known_cleanup_remains: true }), storeRemove: remove },
    { source: stored, listing: [source], mode: 'remove' });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Retry cleanup' }));
  expect(await screen.findByText('The folder store was not found. Refresh the notes list and reopen the folder menu.')).toBeInTheDocument();
  expect(remove).not.toHaveBeenCalled();
});
