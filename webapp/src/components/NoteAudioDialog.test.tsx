import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ChaError, type NoteAudioClient, type NoteAudioStatus } from '../api/client';
import { NoteAudioDialog, noteAudioPhase } from './NoteAudioDialog';

const initial: NoteAudioStatus = { id: 4, phase: 'uploading', done: false, error: '' };
function client(): NoteAudioClient {
  return {
    info: vi.fn().mockResolvedValue({ count: 2 }), start: vi.fn().mockResolvedValue(initial),
    status: vi.fn().mockResolvedValue(initial), source: vi.fn(), release: vi.fn(),
  };
}
async function flush() { await act(async () => {}); }
async function open(audio = client(), prepare = vi.fn().mockResolvedValue(['First', 'Second'])) {
  const onClose = vi.fn();
  const view = render(<NoteAudioDialog client={audio} id={4} prepare={prepare} onClose={onClose} />);
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

describe('note audio generation dialog', () => {
  it('requires a fresh voice, sends saved blocks once, and locks the active dialog', async () => {
    const { audio, prepare, onClose } = await open();
    expect(screen.getByLabelText('Voice')).toHaveValue('');
    expect(screen.getByLabelText('Voice')).toHaveFocus();
    expect(screen.getAllByRole('option').map(option => option.textContent)).toEqual(['Select voice', 'boroda', 'grant', 'dzig', 'gerdt', 'Kore']);
    expect(audio.start).not.toHaveBeenCalled();
    expect(screen.getByText('Existing audio will be replaced after successful generation.')).toBeVisible();
    await generate();
    expect(prepare).toHaveBeenCalledTimes(1);
    expect(audio.start).toHaveBeenCalledExactlyOnceWith(4, 'Kore', ['First', 'Second']);
    expect(screen.getByLabelText('Voice')).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    const cancel = new Event('cancel', { cancelable: true });
    screen.getByRole('dialog').dispatchEvent(cancel);
    expect(cancel.defaultPrevented).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(onClose).not.toHaveBeenCalled();
  });

  it('keeps waiting through local read errors and freezes time at native completion', async () => {
    const audio = client();
    vi.mocked(audio.status).mockRejectedValueOnce(new Error('bridge lost'))
      .mockResolvedValueOnce({ ...initial, phase: 'saving' })
      .mockResolvedValueOnce({ ...initial, phase: 'complete', total: 2, completed: 2, done: true, finished_at_ms: 102900 });
    await open(audio);
    await generate();
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(screen.getByRole('alert')).toHaveTextContent('Could not read generation status');
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    expect(screen.getByRole('timer')).toHaveTextContent('00:01');
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(screen.queryByRole('alert')).toBeNull();
    expect(screen.getByText('Saving…')).toBeVisible();
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(screen.getByText('Audio ready · 2 clips')).toBeVisible();
    expect(screen.getByRole('timer')).toHaveTextContent('00:02');
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(screen.getByRole('timer')).toHaveTextContent('00:02');
    expect(audio.start).toHaveBeenCalledTimes(1);
    expect(audio.status).toHaveBeenCalledTimes(3);
  });

  it('recovers an ambiguous start without resubmitting or accepting an earlier terminal result', async () => {
    const audio = client();
    vi.mocked(audio.start).mockRejectedValue(new ChaError('command_timeout', 'Reply timed out.'));
    vi.mocked(audio.status).mockResolvedValueOnce({ ...initial, done: true, finished_at_ms: 99999 })
      .mockRejectedValueOnce(new ChaError('not_found', 'No status yet.'))
      .mockResolvedValueOnce({ ...initial, phase: 'complete', total: 2, done: true, finished_at_ms: 102500 });
    await open(audio); await generate();
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    expect(screen.queryByText(/Audio ready/)).toBeNull();
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(audio.start).toHaveBeenCalledTimes(1);
  });

  it('includes preparation time and terminates a confirmed rejection without polling', async () => {
    const audio = client();
    vi.mocked(audio.start).mockRejectedValue(new ChaError('invalid_argument', 'No text to narrate'));
    const prepare = vi.fn(async () => { vi.setSystemTime(102400); return []; });
    await open(audio, prepare); await generate();
    expect(screen.getByRole('alert')).toHaveTextContent('No text to narrate');
    expect(screen.getByRole('timer')).toHaveTextContent('00:02');
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(audio.status).not.toHaveBeenCalled();
  });

  it('ends when native start replies with an internal error', async () => {
    const audio = client();
    vi.mocked(audio.start).mockRejectedValue(new ChaError('internal_error', 'The request could not be completed.'));
    await open(audio); await generate();
    expect(screen.getByRole('alert')).toHaveTextContent('The request could not be completed.');
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(audio.status).not.toHaveBeenCalled();
  });

  it('ends locally when preparation fails and does not call start', async () => {
    const audio = client();
    await open(audio, vi.fn().mockRejectedValue(new ChaError('not_found', 'Note no longer exists.')));
    await generate();
    expect(screen.getByRole('alert')).toHaveTextContent('Note no longer exists.');
    expect(screen.getByRole('button', { name: 'Close' })).toBeEnabled();
    expect(audio.start).not.toHaveBeenCalled();
  });

  it('shows phases without fabricated counts', () => {
    expect(noteAudioPhase({ ...initial, phase: 'generating' })).toBe('Generating');
    expect(noteAudioPhase({ ...initial, phase: 'generating', completed: 8, total: 20 })).toBe('Generating · 8 of 20 complete');
    expect(noteAudioPhase({ ...initial, phase: 'queued' })).toBe('Queued');
  });
});
