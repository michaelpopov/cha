import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { ChaError, type FolderStoreOperation, type NoteItem, type NotesClient } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { FolderStoreDialog } from './FolderStoreDialog';

type Operation = Extract<FolderStoreOperation, { present: true }>;
const source: NoteItem = { id: 11, parent_id: null, kind: 'folder', name: 'Research', is_summary: false, summary_linked: false, has_compressed: false, has_audio: false, read_only: false };
const openaiChoice = { service: 'openai' as const, api_key_id: 'key1', api_key_name: 'Personal OpenAI' };
const googleChoice = { service: 'google' as const, api_key_id: 'key2', api_key_name: 'Personal Google' };
const choices = [openaiChoice];
const operation = (changes: Partial<Operation> = {}): Operation => ({
  present: true, operation_id: 5, phase: 'uploading', completed: 1, total: 3,
  error: '', result: '', terminal: false, known_cleanup_remains: false, ...changes,
});
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(ok => { resolve = ok; });
  return { promise, resolve };
}
function setup(overrides: Partial<NotesClient> = {}, props: Partial<Parameters<typeof FolderStoreDialog>[0]> = {}) {
  const onClose = vi.fn();
  const client = fixtureClient({ contextEpoch: () => 1, storeChoices: async () => ({ choices }), storeClose: async () => undefined, ...overrides });
  const result = render(<FolderStoreDialog client={client} source={source} epoch={1} onClose={onClose} {...props} />);
  return { ...result, client, onClose };
}
async function upload() {
  await screen.findByLabelText('API key');
  await waitFor(() => expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Upload' }));
}
const terminal = (changes: Partial<Operation> = {}) => operation({ phase: 'failed', terminal: true, result: 'error', error: 'Could not index “Experiment”.', ...changes });

describe('Folder store dialog', () => {
  it('has an accessible name without a visible title and selects the only key', async () => {
    setup({ storeChoices: async () => ({ choices: [openaiChoice, openaiChoice] }) });
    const dialog = screen.getByRole('dialog', { name: 'Upload folder to store' });
    expect(dialog).not.toHaveTextContent('Upload folder to store');
    await waitFor(() => expect(screen.getByLabelText('API key')).toHaveValue('key1'));
    expect(screen.getAllByRole('option', { name: 'Personal OpenAI' })).toHaveLength(1);
    expect(screen.queryByLabelText('Provider')).not.toBeInTheDocument();
  });
  it('requires a choice with several keys and uploads with the service of the selected key', async () => {
    const start = vi.fn(async () => { throw new ChaError('invalid_argument', 'Try again.'); });
    setup({ storeUpload: start, storeChoices: async () => ({ choices: [openaiChoice, googleChoice] }) });
    await screen.findByLabelText('API key');
    expect(screen.getByLabelText('API key')).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Upload' })).toBeDisabled();
    fireEvent.change(screen.getByLabelText('API key'), { target: { value: 'key2' } });
    expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled();
    fireEvent.click(screen.getByRole('button', { name: 'Upload' }));
    await waitFor(() => expect(start).toHaveBeenCalledExactlyOnceWith(11, 'google', 'key2', '', 1));
  });
  it('directs missing keys to Settings and disables upload', async () => {
    setup({ storeChoices: async () => ({ choices: [] }) });
    expect(await screen.findByRole('alert')).toHaveTextContent('Set cloud store API key');
    expect(screen.getByRole('button', { name: 'Upload' })).toBeDisabled();
  });
  it('cancels before submission without calling any operation command', async () => {
    const start = vi.fn(); const cancel = vi.fn(); const close = vi.fn();
    const { onClose } = setup({ storeUpload: start, storeCancel: cancel, storeClose: close });
    await screen.findByLabelText('API key');
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
    expect(start).toHaveBeenCalledExactlyOnceWith(11, 'openai', 'key1', '', 1);
    expect(screen.queryByLabelText('API key')).not.toBeInTheDocument();
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
    expect(screen.getByLabelText('API key')).toBeEnabled();
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
    const { onClose } = setup({ storeOperation: async () => terminal({ result: 'warning', phase: 'warning', error: '' }) });
    expect(await screen.findByRole('alert')).toHaveTextContent('Some remote data could not be confirmed deleted.');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
    expect(onClose).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
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

it('keeps a failed upload visible when Close fails', async () => {
  const { onClose } = setup({ storeOperation: async () => terminal({ known_cleanup_remains: true }),
    storeClose: async () => { throw new ChaError('invalid_argument', 'The operation is still running.'); } });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Close' }));
  expect(await screen.findByText('The operation is still running.')).toBeInTheDocument();
  expect(onClose).not.toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeInTheDocument();
});

it('does not poll while editable or after terminal failure', async () => {
  const read = vi.fn<NotesClient['storeOperation']>().mockResolvedValue({ present: false });
  const { unmount } = setup({ storeOperation: read });
  await screen.findByLabelText('API key');
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

it('retries cleanup of a recovered upload with the same operation and without upload keys', async () => {
  let state: FolderStoreOperation = terminal({ known_cleanup_remains: true });
  const close = vi.fn(async () => undefined);
  const accepted = deferred<{ operation_id: number }>();
  const cleanup = vi.fn(async () => accepted.promise);
  const upload = vi.fn(); const getChoices = vi.fn();
  const { onClose } = setup({ storeOperation: async () => state, storeCleanup: cleanup, storeClose: close,
    storeUpload: upload, storeChoices: getChoices });
  const retry = await screen.findByRole('button', { name: 'Retry cleanup' });
  await waitFor(() => expect(retry).toBeEnabled());
  fireEvent.click(retry);
  fireEvent.click(retry);
  await waitFor(() => expect(cleanup).toHaveBeenCalledExactlyOnceWith(5, 1));
  expect(retry).toBeDisabled();
  expect(upload).not.toHaveBeenCalled(); expect(getChoices).not.toHaveBeenCalled();
  state = operation({ phase: 'cleaning' });
  await act(async () => accepted.resolve({ operation_id: 5 }));
  expect(screen.getByRole('dialog', { name: 'Upload folder to store' })).toBeInTheDocument();
  expect(screen.getByRole('status')).toHaveTextContent('Cleaning up…');
  expect(screen.queryByRole('button', { name: 'Retry cleanup' })).not.toBeInTheDocument();
  state = terminal({ phase: 'warning', result: 'warning', error: '' });
  await waitFor(() => expect(screen.getByRole('alert')).toHaveTextContent('Some remote data could not be confirmed deleted.'));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled());
  expect(onClose).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('button', { name: 'Close' }));
  await waitFor(() => expect(onClose).toHaveBeenCalledOnce());
  expect(close).toHaveBeenCalledExactlyOnceWith(5, 1);
});

it('keeps cleanup retry available when its start fails', async () => {
  const cleanup = vi.fn(async () => { throw new ChaError('operation_cancelled', 'The operation could not be started.'); });
  const { onClose } = setup({ storeOperation: async () => terminal({ known_cleanup_remains: true }), storeCleanup: cleanup });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Retry cleanup' }));
  expect(await screen.findByText('The operation could not be started.')).toBeInTheDocument();
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  expect(cleanup).toHaveBeenCalledExactlyOnceWith(5, 1);
  expect(onClose).not.toHaveBeenCalled();
});

it('does not retry cleanup for an operation replaced since the displayed failure', async () => {
  let state: FolderStoreOperation = terminal({ known_cleanup_remains: true });
  const cleanup = vi.fn();
  setup({ storeOperation: async () => state, storeCleanup: cleanup });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Retry cleanup' })).toBeEnabled());
  state = terminal({ operation_id: 9, known_cleanup_remains: true });
  fireEvent.click(screen.getByRole('button', { name: 'Retry cleanup' }));
  expect(await screen.findByText('The store operation changed. Close this dialog and reopen the folder menu.')).toBeInTheDocument();
  expect(cleanup).not.toHaveBeenCalled();
});

it('submits the exact description, preserving draft after a start error', async () => {
  const start = vi.fn(async () => { throw new ChaError('invalid_argument', 'Try again.'); });
  setup({ storeUpload: start });
  await screen.findByLabelText('Description (optional)');
  const description = '  First paragraph 😀\n\nВторой абзац\n ';
  fireEvent.change(screen.getByLabelText('Description (optional)'), { target: { value: description } });
  expect(screen.getByLabelText('Description (optional)')).toHaveAttribute('rows', '6');
  await upload();
  await waitFor(() => expect(start).toHaveBeenCalledExactlyOnceWith(11, 'openai', 'key1', description, 1));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled());
  expect(screen.getByLabelText('Description (optional)')).toHaveValue(description);
});

it('opens without a name field and allows upload with the only key', async () => {
  setup();
  await waitFor(() => expect(screen.getByLabelText('API key')).toHaveValue('key1'));
  expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled();
});

it('revalidates description limits for the service of the selected key without changing text', async () => {
  setup({ storeChoices: async () => ({ choices: [openaiChoice, googleChoice] }) });
  await screen.findByLabelText('API key');
  fireEvent.change(screen.getByLabelText('API key'), { target: { value: 'key1' } });
  fireEvent.change(screen.getByLabelText('Description (optional)'), { target: { value: 'я'.repeat(511) + '\n😀' } });
  expect(screen.getByRole('alert')).toHaveTextContent('Description must be 512 characters or fewer for OpenAI.');
  fireEvent.change(screen.getByLabelText('API key'), { target: { value: 'key2' } });
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled();
  fireEvent.change(screen.getByLabelText('API key'), { target: { value: 'key1' } });
  fireEvent.change(screen.getByLabelText('Description (optional)'), { target: { value: 'я'.repeat(510) + '\n😀' } });
  expect(screen.getByRole('button', { name: 'Upload' })).toBeEnabled();
});

it('disables all fields during start and retains the draft after preflight', async () => {
  const accepted = deferred<{ operation_id: number }>();
  let state: FolderStoreOperation = { present: false };
  setup({ storeUpload: () => accepted.promise, storeOperation: async () => state,
    storeClose: async () => { state = { present: false }; } });
  await screen.findByLabelText('Description (optional)');
  fireEvent.change(screen.getByLabelText('Description (optional)'), { target: { value: 'Draft\n\nDescription' } });
  await upload();
  for (const label of ['API key', 'Description (optional)']) expect(screen.getByLabelText(label)).toBeDisabled();
  state = terminal({ result: 'preflight', error: 'All saved notes are empty.' });
  await act(async () => { accepted.resolve({ operation_id: 5 }); });
  await screen.findByText('All saved notes are empty.');
  expect(screen.getByLabelText('Description (optional)')).toHaveValue('Draft\n\nDescription');
});
