import { act, fireEvent, render, screen } from '@testing-library/react';
import { StrictMode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ChaError, type NoteAudioClient, type NoteAudioStatus } from '../api/client';
import { NoteAudioDialog, noteAudioPhase } from './NoteAudioDialog';

const initial: NoteAudioStatus = { id: 4, phase: 'uploading', done: false, error: '' };
function client(): NoteAudioClient {
  return {
    startBackground: vi.fn().mockResolvedValue({ attempt_id: 1 }), backgroundStatus: vi.fn(),
    info: vi.fn().mockResolvedValue({ count: 2 }), start: vi.fn().mockResolvedValue(initial),
    status: vi.fn().mockResolvedValue(initial), cancel: vi.fn().mockResolvedValue(undefined), source: vi.fn(), release: vi.fn(),
  };
}
async function flush() { await act(async () => {}); }
const targets = [{ id: 4, name: 'First note' }, { id: 5, name: 'Second note' }, { id: 6, name: 'Third note' }];
async function open(audio = client(), prepare = vi.fn().mockResolvedValue(['First', 'Second']), folderMode = false,
  selected = folderMode ? targets : targets.slice(0, 1), onSettled: (id: number) => void = vi.fn()) {
  const onClose = vi.fn();
  const view = render(<NoteAudioDialog client={audio} targets={selected} folderMode={folderMode} prepare={prepare} onSettled={onSettled} onClose={onClose} />);
  await flush();
  return { ...view, audio, prepare, onClose, onSettled };
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
    const { audio, onClose, onSettled } = await open();
    expect(screen.getByLabelText('Voice')).toHaveValue('');
    expect(screen.getByLabelText('Voice')).toHaveFocus();
    expect(screen.getAllByRole('option').map(option => option.textContent)).toEqual(['Select voice', 'boroda', 'grant', 'dzig', 'gerdt', 'Kore']);
    expect(screen.getByText('Existing audio will be replaced after successful generation.')).toBeVisible();
    await generate();
    expect(audio.startBackground).toHaveBeenCalledExactlyOnceWith(4, 'Kore', ['First', 'Second']);
    expect(onClose).toHaveBeenCalledOnce();
    expect(onSettled).not.toHaveBeenCalled();
    expect(audio.status).not.toHaveBeenCalled();
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
    expect(audio.cancel).not.toHaveBeenCalled();
  });

  it('closes before Generate without submitting', async () => {
    const { audio, onClose, unmount } = await open();
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(onClose).toHaveBeenCalledOnce();
    unmount();
    expect(audio.startBackground).not.toHaveBeenCalled();
    expect(audio.cancel).not.toHaveBeenCalled();
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
      expect(audio.cancel).not.toHaveBeenCalled();
    });

  it('shows folder phases without fabricated counts', () => {
    expect(noteAudioPhase({ ...initial, phase: 'generating' })).toBe('Generating');
    expect(noteAudioPhase({ ...initial, phase: 'generating', completed: 8, total: 20 })).toBe('Generating · 8 of 20 clips complete');
    expect(noteAudioPhase({ ...initial, phase: 'queued' })).toBe('Queued');
  });
});

async function tick(ms = 1000) { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); }
function done(id: number, error = '', finished_at_ms = Date.now()): NoteAudioStatus {
  return { id, phase: error ? 'saving' : 'complete', done: true, error, total: 2, finished_at_ms };
}
function folderClient() {
  const audio = client();
  vi.mocked(audio.info).mockResolvedValue({ count: 0 });
  vi.mocked(audio.start).mockImplementation(async id => ({ ...initial, id }));
  return audio;
}
function stopRun() { fireEvent.click(screen.getByRole('button', { name: 'Stop' })); }

describe('folder audio generation', () => {
  it('keeps one eligibility scan through StrictMode effect replay', async () => {
    const audio = folderClient();
    let resolve!: (value: { count: number }) => void;
    vi.mocked(audio.info).mockReturnValueOnce(new Promise(next => { resolve = next; }));
    render(<StrictMode><NoteAudioDialog client={audio} targets={targets} folderMode
      prepare={vi.fn().mockResolvedValue(['Text'])} onSettled={vi.fn()} onClose={vi.fn()} /></StrictMode>);
    await flush();
    expect(audio.info).toHaveBeenCalledExactlyOnceWith(4);
    expect(screen.getByText('Checking notes…')).toBeVisible();
    await act(async () => { resolve({ count: 0 }); });
    expect(vi.mocked(audio.info).mock.calls).toEqual([[4], [5], [6]]);
    expect(screen.getByText('3 notes to generate')).toBeVisible();
  });

  it('checks candidates sequentially once and fixes eligibility before Generate', async () => {
    const audio = folderClient();
    let resolve!: (value: { count: number }) => void;
    vi.mocked(audio.info).mockReturnValueOnce(new Promise(next => { resolve = next; }))
      .mockResolvedValueOnce({ count: 2 }).mockResolvedValueOnce({ count: 0 });
    const view = await open(audio, undefined, true);
    expect(screen.getByText('Checking notes…')).toBeVisible();
    expect(audio.info).toHaveBeenCalledExactlyOnceWith(4);
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Generate' })).toBeDisabled();
    await act(async () => { resolve({ count: 0 }); });
    expect(screen.getByText('2 notes to generate')).toBeVisible();
    expect(vi.mocked(audio.info).mock.calls).toEqual([[4], [5], [6]]);
    vi.mocked(audio.start).mockImplementation(async id => done(id));
    await generate();
    expect(vi.mocked(audio.start).mock.calls.map(([id, voice]) => [id, voice])).toEqual([[4, 'Kore'], [6, 'Kore']]);
    expect(audio.info).toHaveBeenCalledTimes(3);
    expect(view.onSettled).toHaveBeenCalledTimes(2);
    expect(screen.getByText('2 of 2 notes complete')).toBeVisible();
    expect(screen.getByText('Audio ready')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(view.onClose).not.toHaveBeenCalled();
  });

  it('waits for each saved result and resets the next note status and timer', async () => {
    const audio = folderClient();
    vi.mocked(audio.status).mockResolvedValueOnce({ ...initial, phase: 'generating', completed: 1, total: 2 })
      .mockImplementation(async id => done(id, '', 101500));
    let resolve!: (blocks: string[]) => void;
    const prepare = vi.fn().mockResolvedValueOnce(['First']).mockReturnValueOnce(new Promise(next => { resolve = next; }));
    await open(audio, prepare, true, targets.slice(0, 2)); await generate();
    expect(audio.start).toHaveBeenCalledTimes(1);
    await tick();
    expect(screen.getByRole('progressbar')).toHaveValue(0);
    expect(screen.getByRole('timer')).toHaveTextContent('00:01');
    await tick();
    expect(screen.getByText('Note 2 of 2 · 1 complete')).toBeVisible();
    expect(screen.getByText('Second note')).toBeVisible();
    expect(screen.getByRole('progressbar')).toHaveValue(1);
    expect(screen.getByRole('timer')).toHaveTextContent('00:00');
    expect(screen.queryByText(/Generating ·/)).toBeNull();
    expect(audio.start).toHaveBeenCalledTimes(1);
    await act(async () => { resolve(['Second']); });
    expect(audio.start).toHaveBeenLastCalledWith(5, 'Kore', ['Second']);
  });

  it.each([{ selected: [] }, { selected: targets }])('does not start when no notes are eligible: %j', async ({ selected }) => {
    const audio = client();
    await open(audio, undefined, true, selected);
    expect(screen.getByText('No notes need audio')).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Generate' })).toBeNull();
    expect(audio.start).not.toHaveBeenCalled();
  });

  it('stops preparation on an audio lookup failure', async () => {
    const audio = folderClient();
    vi.mocked(audio.info).mockRejectedValueOnce(new ChaError('not_found', 'Note missing.'));
    await open(audio, undefined, true);
    expect(screen.getByRole('alert')).toHaveTextContent('Note missing.');
    expect(audio.info).toHaveBeenCalledTimes(1);
    expect(audio.start).not.toHaveBeenCalled();
  });

  it('skips empty and oversized notes and keeps their names and reasons', async () => {
    const audio = folderClient();
    vi.mocked(audio.start).mockRejectedValueOnce(new ChaError('body_too_large', 'The note exceeds the 1,000-request batch limit.'))
      .mockImplementation(async id => done(id));
    const prepare = vi.fn().mockResolvedValueOnce([]).mockResolvedValue(['Text']);
    await open(audio, prepare, true); await generate();
    expect(vi.mocked(audio.start).mock.calls.map(([id]) => id)).toEqual([5, 6]);
    expect(screen.getByText('1 of 3 notes complete · 2 skipped')).toBeVisible();
    expect(screen.getByText('First note')).toBeVisible();
    expect(screen.getByText('No text to narrate')).toBeVisible();
    expect(screen.getByText('Second note')).toBeVisible();
    expect(screen.getByText('The note exceeds the 1,000-request batch limit.')).toBeVisible();
    expect(screen.getByRole('progressbar')).toHaveValue(3);
    expect(screen.getByText('Audio ready')).toBeVisible();
  });

  it('completes an all-empty run without submitting or claiming audio is ready', async () => {
    const audio = folderClient();
    await open(audio, vi.fn().mockResolvedValue([]), true); await generate();
    expect(audio.start).not.toHaveBeenCalled();
    expect(screen.getByText('0 of 3 notes complete · 3 skipped')).toBeVisible();
    expect(screen.queryByText('Audio ready')).toBeNull();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
  });

  it.each(['prepare', 'reject', 'native'])('stops at the first non-skippable failure: %s', async source => {
    const audio = folderClient();
    const failure = new ChaError('invalid_argument', 'Generation failed.');
    const prepare = source === 'prepare' ? vi.fn().mockRejectedValue(failure) : vi.fn().mockResolvedValue(['Text']);
    if (source === 'reject') vi.mocked(audio.start).mockRejectedValue(failure);
    if (source === 'native') vi.mocked(audio.start).mockResolvedValue(done(4, 'Generation failed.'));
    await open(audio, prepare, true); await generate();
    expect(prepare).toHaveBeenCalledTimes(1);
    expect(screen.getByText('Folder run stopped')).toBeVisible();
    expect(screen.getByRole('alert')).toHaveTextContent('Generation failed.');
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
  });

  it('does not advance after a local status error', async () => {
    const audio = folderClient();
    vi.mocked(audio.status).mockRejectedValueOnce(new Error('lost')).mockImplementation(async id => done(id));
    await open(audio, undefined, true); await generate(); await tick();
    expect(audio.start).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('progressbar')).toHaveValue(0);
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    await tick();
    expect(audio.start).toHaveBeenCalledTimes(2);
  });

  it.each(['not_found', 'older'])('ends a folder lost start without starting another note: %s', async outcome => {
    const audio = folderClient();
    vi.mocked(audio.start).mockRejectedValue(new ChaError('command_timeout', 'Reply timed out.'));
    if (outcome === 'not_found') vi.mocked(audio.status).mockRejectedValue(new ChaError('not_found', 'No generation.'));
    else vi.mocked(audio.status).mockResolvedValue(done(4, '', 99999));
    await open(audio, undefined, true); await generate(); await tick();
    expect(audio.start).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(screen.getByText('Folder run stopped')).toBeVisible();
  });

  it('stops before dispatch when document preparation is pending', async () => {
    const audio = folderClient();
    let resolve!: (blocks: string[]) => void;
    await open(audio, vi.fn(() => new Promise<string[]>(next => { resolve = next; })), true);
    await generate(); stopRun();
    expect(screen.getByText('Stopping…')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Stop' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    await act(async () => { resolve(['Text']); });
    expect(audio.start).not.toHaveBeenCalled();
    expect(audio.cancel).not.toHaveBeenCalled();
    expect(screen.getByText('Folder run stopped')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
  });

  it('waits for a delayed start, repeats a lost cancel, and waits for terminal status', async () => {
    const audio = folderClient();
    let resolve!: (status: NoteAudioStatus) => void;
    vi.mocked(audio.start).mockReturnValueOnce(new Promise(next => { resolve = next; }));
    vi.mocked(audio.cancel).mockRejectedValueOnce(new Error('reply lost'));
    vi.mocked(audio.status).mockResolvedValueOnce(initial).mockResolvedValue(done(4, 'Interrupted.'));
    const { onClose } = await open(audio, undefined, true); await generate(); stopRun();
    expect(audio.cancel).not.toHaveBeenCalled();
    await act(async () => { resolve(initial); });
    expect(audio.cancel).toHaveBeenCalledExactlyOnceWith(4);
    const escape = new Event('cancel', { cancelable: true });
    screen.getByRole('dialog').dispatchEvent(escape);
    expect(escape.defaultPrevented).toBe(true);
    expect(onClose).not.toHaveBeenCalled();
    await tick();
    expect(audio.cancel).toHaveBeenCalledTimes(2);
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    await tick();
    expect(screen.getByText('Folder run stopped')).toBeVisible();
    expect(screen.queryByRole('alert')).toBeNull();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(audio.start).toHaveBeenCalledTimes(1);
  });

  it.each(['not_found', 'active'])('stops after a lost start reply: %s', async outcome => {
    const audio = folderClient();
    vi.mocked(audio.start).mockRejectedValue(new ChaError('command_timeout', 'Reply timed out.'));
    if (outcome === 'not_found') vi.mocked(audio.status).mockRejectedValue(new ChaError('not_found', 'No generation.'));
    else vi.mocked(audio.status).mockResolvedValueOnce(initial).mockResolvedValue(done(4, 'Interrupted.'));
    await open(audio, undefined, true); await generate(); stopRun(); await tick();
    if (outcome === 'active') {
      expect(audio.cancel).toHaveBeenCalledExactlyOnceWith(4);
      expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
      await tick();
    } else expect(audio.cancel).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(screen.getByText('Folder run stopped')).toBeVisible();
    expect(audio.start).toHaveBeenCalledTimes(1);
  });

  it.each([1, 3])('counts a commit that wins Stop and stops remaining notes: %s targets', async count => {
    const audio = folderClient();
    vi.mocked(audio.status).mockImplementation(async id => done(id, '', 100500));
    await open(audio, undefined, true, targets.slice(0, count)); await generate(); stopRun(); await tick();
    expect(screen.getByRole('progressbar')).toHaveValue(1);
    expect(screen.getByRole('timer')).toHaveTextContent('00:00');
    expect(audio.start).toHaveBeenCalledTimes(1);
    expect(screen.getByText(count === 1 ? 'Audio ready' : 'Folder run stopped')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
  });

  it('stops between notes and retains the note just saved', async () => {
    const audio = folderClient();
    vi.mocked(audio.start).mockImplementation(async id => done(id));
    await open(audio, undefined, true, targets, () => stopRun()); await generate();
    expect(audio.start).toHaveBeenCalledTimes(1);
    expect(audio.cancel).not.toHaveBeenCalled();
    expect(screen.getByText('First note')).toBeVisible();
    expect(screen.getByText('Note 1 of 3 · 1 complete')).toBeVisible();
    expect(screen.getByText('Folder run stopped')).toBeVisible();
  });

  it.each(['prepare', 'start', 'status'])('unmount prevents further starts and notifications during %s', async phase => {
    const audio = folderClient();
    let resolve!: (value: never) => void;
    const deferred = new Promise<never>(next => { resolve = next; });
    const prepare = vi.fn().mockResolvedValue(['Text']);
    if (phase === 'prepare') prepare.mockReturnValueOnce(deferred);
    if (phase === 'start') vi.mocked(audio.start).mockReturnValueOnce(deferred);
    if (phase === 'status') vi.mocked(audio.status).mockReturnValueOnce(deferred);
    const view = await open(audio, prepare, true); await generate();
    if (phase === 'status') await tick();
    view.unmount();
    await act(async () => { resolve((phase === 'prepare' ? ['Text'] : done(4)) as never); });
    await tick(2000);
    expect(audio.start).toHaveBeenCalledTimes(phase === 'prepare' ? 0 : 1);
    expect(view.onSettled).not.toHaveBeenCalled();
    expect(audio.cancel).not.toHaveBeenCalled();
  });

  it.each(['prepare', 'start', 'status'])('context end prevents the next note during %s', async phase => {
    const audio = folderClient();
    const failure = new ChaError('vault_changed', 'Vault changed.');
    const prepare = vi.fn().mockResolvedValue(['Text']);
    if (phase === 'prepare') prepare.mockRejectedValueOnce(failure);
    if (phase === 'start') vi.mocked(audio.start).mockRejectedValueOnce(failure);
    if (phase === 'status') vi.mocked(audio.status).mockRejectedValueOnce(failure);
    await open(audio, prepare, true); await generate();
    if (phase === 'status') await tick();
    expect(audio.start).toHaveBeenCalledTimes(phase === 'prepare' ? 0 : 1);
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(screen.getByRole('alert')).toHaveTextContent('Vault changed.');
  });

  it('skips an oversized single-target folder and keeps the rejection reason', async () => {
    const audio = folderClient();
    vi.mocked(audio.start).mockRejectedValue(new ChaError('body_too_large', 'Too large.'));
    await open(audio, undefined, true, targets.slice(0, 1)); await generate();
    expect(screen.getByText('0 of 1 notes complete · 1 skipped')).toBeVisible();
    expect(screen.getByText('Too large.')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
  });

  it('keeps folder skip behavior with just one target', async () => {
    const audio = folderClient();
    await open(audio, vi.fn().mockResolvedValue([]), true, targets.slice(0, 1)); await generate();
    expect(screen.getByText('0 of 1 notes complete · 1 skipped')).toBeVisible();
    expect(audio.start).not.toHaveBeenCalled();
  });
});
