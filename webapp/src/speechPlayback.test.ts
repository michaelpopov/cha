import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { beginSpeechPlayback, onSpeechPlaybackChange } from './speechPlayback';

beforeEach(() => { vi.useFakeTimers(); });
afterEach(async () => {
  await vi.advanceTimersByTimeAsync(400);
  vi.useRealTimers();
});

describe('speech playback echo tail', () => {
  it('waits 400 ms after the final player and keeps new listeners muted during that delay', async () => {
    const changes = vi.fn();
    const unsubscribe = onSpeechPlaybackChange(changes);
    const firstEnded = beginSpeechPlayback();
    const secondEnded = beginSpeechPlayback();
    const lateChanges = vi.fn();
    let unsubscribeLate = () => {};
    try {
      firstEnded();
      await vi.advanceTimersByTimeAsync(500);
      expect(changes.mock.calls).toEqual([[false], [true]]);
      secondEnded();
      secondEnded();
      await vi.advanceTimersByTimeAsync(399);
      expect(changes.mock.calls).toEqual([[false], [true]]);
      unsubscribeLate = onSpeechPlaybackChange(lateChanges);
      expect(lateChanges).toHaveBeenCalledExactlyOnceWith(true);
      await vi.advanceTimersByTimeAsync(1);
      expect(changes.mock.calls).toEqual([[false], [true], [false]]);
      expect(lateChanges).toHaveBeenLastCalledWith(false);
    } finally {
      firstEnded();
      secondEnded();
      unsubscribe();
      unsubscribeLate();
    }
  });

  it('cancels a pending resume when the next clip starts and waits after that clip', async () => {
    const changes = vi.fn();
    const unsubscribe = onSpeechPlaybackChange(changes);
    const firstEnded = beginSpeechPlayback();
    let secondEnded = () => {};
    try {
      firstEnded();
      await vi.advanceTimersByTimeAsync(300);
      secondEnded = beginSpeechPlayback();
      await vi.advanceTimersByTimeAsync(500);
      expect(changes.mock.calls).toEqual([[false], [true]]);
      secondEnded();
      await vi.advanceTimersByTimeAsync(400);
      expect(changes.mock.calls).toEqual([[false], [true], [false]]);
    } finally {
      firstEnded();
      secondEnded();
      unsubscribe();
    }
  });
});
