import { act, fireEvent, render, screen } from '@testing-library/react';
import { StrictMode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ChaError, type NoteAudioClient } from '../api/client';
import { NoteAudioDialog } from './NoteAudioDialog';

function client(): NoteAudioClient {
  return {
    startBackground: vi.fn().mockResolvedValue({ attempt_id: 1 }), backgroundStatus: vi.fn().mockResolvedValue({ pending: false }), contextEpoch: vi.fn(() => 1),
    info: vi.fn().mockResolvedValue({ count: 2 }),
    source: vi.fn(), release: vi.fn(),
  };
}
async function flush() { await act(async () => {}); }
const targets = [{ id: 4, name: 'First note' }, { id: 5, name: 'Second note' }, { id: 6, name: 'Third note' }];
async function open(audio = client(), prepare = vi.fn().mockResolvedValue(['First', 'Second']), folderMode = false,
  selected = folderMode ? targets : targets.slice(0, 1)) {
  const onClose = vi.fn();
  const view = render(<NoteAudioDialog client={audio} targets={selected} folderMode={folderMode} prepare={prepare} epoch={1} onClose={onClose} />);
  await flush();
  return { ...view, audio, prepare, onClose };
}
async function generate() {
  fireEvent.change(screen.getByLabelText('Voice'), { target: { value: 'Kore' } });
  fireEvent.click(screen.getByRole('button', { name: 'Generate' }));
  await flush();
}
beforeEach(() => { vi.useFakeTimers(); vi.setSystemTime(100000); });
afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks(); });

describe('single-note audio setup', () => {
  it('requires a fresh voice and closes on durable acceptance without polling', async () => {
    const { audio, onClose } = await open();
    expect(screen.getByLabelText('Voice')).toHaveValue('');
    expect(screen.getByLabelText('Voice')).toHaveFocus();
    expect(screen.getAllByRole('option').map(option => option.textContent)).toEqual(['Select voice', 'boroda', 'grant', 'dzig', 'gerdt', 'Kore']);
    expect(screen.getByText('Existing audio will be replaced after successful generation.')).toBeVisible();
    await generate();
    expect(audio.startBackground).toHaveBeenCalledExactlyOnceWith(4, 'Kore', ['First', 'Second']);
    expect(onClose).toHaveBeenCalledOnce();
    expect(screen.queryByRole('timer')).toBeNull();
    expect(screen.queryByRole('progressbar')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Stop' })).toBeNull();
  });

  it('blocks double clicks during preparation and never cancels on unmount', async () => {
    let resolve!: (blocks: string[]) => void;
    const prepare = vi.fn(() => new Promise<string[]>(done => { resolve = done; }));
    const { audio, unmount } = await open(undefined, prepare);
    await generate();
    fireEvent.click(screen.getByRole('button', { name: 'Generate' }));
    expect(prepare).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(screen.getByLabelText('Voice')).toBeDisabled();
    await act(async () => resolve(['Saved text']));
    expect(audio.startBackground).toHaveBeenCalledTimes(1);
    unmount();
  });

  it('closes before Generate without submitting', async () => {
    const { audio, onClose, unmount } = await open();
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(onClose).toHaveBeenCalledOnce();
    unmount();
    expect(audio.startBackground).not.toHaveBeenCalled();
  });

  it.each(['invalid_argument', 'body_too_large'] as const)('permits correction and retry after %s', async code => {
    const audio = client();
    vi.mocked(audio.startBackground).mockRejectedValueOnce(new ChaError(code, 'Correct this input.'));
    const { onClose } = await open(audio);
    await generate();
    expect(screen.getByRole('alert')).toHaveTextContent('Correct this input.');
    expect(screen.getByLabelText('Voice')).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Generate' })).toBeEnabled();
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(2);
    expect(onClose).toHaveBeenCalledOnce();
  });

  it('permits retry after a preparation failure', async () => {
    const prepare = vi.fn().mockRejectedValueOnce(new Error('missing')).mockResolvedValue(['Text']);
    const { audio } = await open(undefined, prepare);
    await generate();
    expect(audio.startBackground).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Generate' })).toBeEnabled();
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(1);
  });

  it.each(['command_timeout', 'internal_error', 'operation_cancelled', 'vault_changed', 'application_unavailable', 'server_stopping'] as const)(
    'keeps an uncertain %s dismissible and prevents another invocation', async code => {
      const audio = client();
      vi.mocked(audio.startBackground).mockRejectedValue(new ChaError(code, 'Lost reply.'));
      const { onClose } = await open(audio);
      await generate();
      expect(screen.getByRole('alert')).toHaveTextContent('Could not confirm whether audio generation started. Check Settings → Events before starting again.');
      expect(screen.getByRole('button', { name: 'Generate' })).toBeDisabled();
      expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
      await tick(5000);
      expect(audio.startBackground).toHaveBeenCalledTimes(1);
      const cancel = new Event('cancel', { cancelable: true });
      screen.getByRole('dialog').dispatchEvent(cancel);
      expect(onClose).toHaveBeenCalledOnce();
    });

});

async function tick(ms = 1000) { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); }

function folderClient() {
  const audio = client();
  vi.mocked(audio.info).mockResolvedValue({ count: 0 });
  return audio;
}
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

describe('folder local acceptance', () => {
  it('submits the next job after acceptance and closes while remote jobs remain pending', async () => {
    const audio = folderClient();
    const a = deferred<{ attempt_id: number }>();
    const b = deferred<{ attempt_id: number }>();
    vi.mocked(audio.startBackground).mockReturnValueOnce(a.promise).mockReturnValueOnce(b.promise);
    const view = await open(audio, undefined, true, targets.slice(0, 2));
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Generate' }));
    const escape = new Event('cancel', { cancelable: true });
    screen.getByRole('dialog').dispatchEvent(escape);
    expect(view.onClose).not.toHaveBeenCalled();
    await act(async () => a.resolve({ attempt_id: 10 }));
    expect(audio.startBackground).toHaveBeenLastCalledWith(5, 'Kore', ['First', 'Second'], true);
    expect(view.onClose).not.toHaveBeenCalled();
    await act(async () => b.resolve({ attempt_id: 11 }));
    expect(view.onClose).toHaveBeenCalledOnce();
    const calls = vi.mocked(audio.backgroundStatus).mock.calls.length;
    await tick(30000);
    expect(audio.backgroundStatus).toHaveBeenCalledTimes(calls);
    expect(screen.queryByRole('timer')).toBeNull();
    expect(screen.queryByRole('progressbar')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Stop' })).toBeNull();
  });

  it('excludes stored and pending audio, including changes after scanning', async () => {
    const audio = folderClient();
    vi.mocked(audio.info).mockImplementation(async id => ({ count: id === 4 ? 1 : 0 }));
    vi.mocked(audio.backgroundStatus).mockImplementation(async id => id === 5 ? { pending: true, attempt_id: 10 } : { pending: false });
    const view = await open(audio, undefined, true);
    vi.mocked(audio.info).mockResolvedValue({ count: 1 });
    await generate();
    expect(view.prepare).not.toHaveBeenCalled();
    expect(audio.startBackground).not.toHaveBeenCalled();
    expect(view.onClose).toHaveBeenCalledOnce();
  });

  it('sends empty blocks for native warnings and closes on all skips', async () => {
    const audio = folderClient();
    vi.mocked(audio.startBackground).mockResolvedValue({ skipped: true });
    const view = await open(audio, vi.fn().mockResolvedValue([]), true);
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(3);
    expect(audio.startBackground).toHaveBeenCalledWith(4, 'Kore', [], true);
    expect(view.onClose).toHaveBeenCalledOnce();
    expect(screen.queryByText('Audio ready')).toBeNull();
  });

  it('has no Generate action for an initially empty selection', async () => {
    const audio = folderClient();
    await open(audio, undefined, true, []);
    expect(screen.getByText('No notes need audio')).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Generate' })).toBeNull();
    expect(audio.info).not.toHaveBeenCalled();
  });

  it('skips a deleted document and stops on other preparation failures', async () => {
    const audio = folderClient();
    const prepare = vi.fn().mockRejectedValueOnce(new ChaError('not_found', 'deleted'))
      .mockRejectedValueOnce(new ChaError('internal_error', 'Disk unavailable'));
    await open(audio, prepare, true);
    await generate();
    expect(prepare.mock.calls).toEqual([[4], [5]]);
    expect(audio.startBackground).not.toHaveBeenCalled();
    expect(screen.getByRole('alert')).not.toHaveTextContent('Jobs already accepted');
    expect(screen.getByRole('button', { name: 'Generate' })).toBeEnabled();
  });

  it.each([0, 1])('permits definite retry with %i earlier acceptances and rechecks pending jobs', async count => {
    const audio = folderClient();
    if (count) vi.mocked(audio.startBackground).mockResolvedValueOnce({ attempt_id: 1 });
    vi.mocked(audio.startBackground).mockRejectedValueOnce(new ChaError('invalid_argument', 'Could not record the audio skip.'));
    const view = await open(audio, undefined, true);
    await generate();
    expect(screen.getByRole('alert').textContent!.includes('Jobs already accepted')).toBe(Boolean(count));
    expect(screen.getByRole('button', { name: 'Generate' })).toBeEnabled();
    vi.mocked(audio.backgroundStatus).mockImplementation(async id => count && id === 4 ? { pending: true, attempt_id: 1 } : { pending: false });
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(4);
    expect(view.onClose).toHaveBeenCalledOnce();
  });

  it.each(['command_timeout', 'internal_error', 'operation_cancelled'] as const)('does not replay unresolved %s or perform extra status resolution', async code => {
    const audio = folderClient();
    vi.mocked(audio.startBackground).mockResolvedValueOnce({ attempt_id: 1 }).mockRejectedValueOnce(new ChaError(code, 'lost'));
    await open(audio, undefined, true);
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(2);
    expect(screen.getByRole('alert')).toHaveTextContent('Jobs already accepted continue in the background. Check Settings → Events');
    expect(screen.getByRole('button', { name: 'Generate' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(vi.mocked(audio.backgroundStatus).mock.calls.map(([id]) => id)).toEqual([4, 5, 6, 4, 5]);
  });

  it.each(['scan', 'before generate', 'prepare', 'acceptance', 'retry'])('keeps its original epoch across %s', async stage => {
    const audio = folderClient();
    let epoch = 1;
    vi.mocked(audio.contextEpoch).mockImplementation(() => epoch);
    const info = deferred<{ count: number }>();
    if (stage === 'scan') vi.mocked(audio.info).mockReturnValueOnce(info.promise);
    const prepare = vi.fn(async () => { if (stage === 'prepare') epoch = 2; return ['Text']; });
    if (stage === 'acceptance') vi.mocked(audio.startBackground).mockImplementation(async () => { epoch = 2; return { attempt_id: 1 }; });
    if (stage === 'retry') vi.mocked(audio.startBackground).mockRejectedValueOnce(new ChaError('invalid_argument', 'settings'));
    await open(audio, prepare, true);
    if (stage === 'scan') { epoch = 2; await act(async () => info.resolve({ count: 0 })); }
    else {
      if (stage === 'before generate') epoch = 2;
      await generate();
      if (stage === 'retry') { epoch = 2; await generate(); }
    }
    expect(screen.getByRole('alert')).toHaveTextContent('The vault changed. Close this dialog and start again.');
    expect(screen.getByRole('button', { name: 'Generate' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(audio.startBackground).toHaveBeenCalledTimes(['acceptance', 'retry'].includes(stage) ? 1 : 0);
    if (stage === 'scan') expect(audio.backgroundStatus).not.toHaveBeenCalled();
  });

  it.each(['unmount', 'connection'])('stops after preparation on %s', async ending => {
    const audio = folderClient();
    const prepared = deferred<string[]>();
    const view = await open(audio, vi.fn(() => prepared.promise), true);
    await generate();
    if (ending === 'unmount') view.unmount();
    else vi.mocked(audio.contextEpoch).mockImplementation(() => { throw new ChaError('application_unavailable', 'Connection ended'); });
    await act(async () => prepared.resolve(['Text']));
    expect(audio.startBackground).not.toHaveBeenCalled();
    if (ending === 'connection') {
      expect(screen.getByRole('button', { name: 'Generate' })).toBeDisabled();
      expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    }
  });

  it('does not duplicate scan or submission in StrictMode', async () => {
    const audio = folderClient();
    const onClose = vi.fn();
    render(<StrictMode><NoteAudioDialog client={audio} targets={targets} epoch={1} folderMode
      prepare={vi.fn().mockResolvedValue(['Text'])} onClose={onClose} /></StrictMode>);
    await flush();
    expect(audio.info).toHaveBeenCalledTimes(3);
    await generate();
    expect(audio.startBackground).toHaveBeenCalledTimes(3);
    expect(onClose).toHaveBeenCalledOnce();
  });
});
