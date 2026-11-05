import { afterEach, describe, expect, it, vi } from 'vitest';

import { TextToSpeechSession } from './textToSpeech';

// Only the members that TextToSpeechSession uses on the blob path.
class FakeAudio extends EventTarget {
  src = '';
  currentTime = 0;
  duration = 10;
  readyState = 0;
  paused = true;
  ended = false;
  defaultPlaybackRate = 1;
  playbackRate = 1;
  startRates: number[][] = [];
  // The media load algorithm resets the current rate to the default rate.
  load = vi.fn(() => { this.playbackRate = this.defaultPlaybackRate; });
  play = vi.fn(async () => {
    this.startRates.push([this.defaultPlaybackRate, this.playbackRate]);
    this.paused = false;
  });
  pause = vi.fn(() => { this.paused = true; });
  removeAttribute = vi.fn();
}

class TestURL extends URL {
  static createObjectURL = () => 'blob:audio';
  static revokeObjectURL = () => undefined;
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('TextToSpeechSession playback rate', () => {
  it('applies a rate set before the audio element exists, then changes it in place', async () => {
    let respond!: (response: unknown) => void;
    vi.stubGlobal('fetch', vi.fn(() => new Promise((resolve) => { respond = resolve; })));
    vi.stubGlobal('URL', TestURL);
    const audio = new FakeAudio();
    const session = new TextToSpeechSession({
      text: 'Answer', cachedUrl: '/media/1', onEnded: vi.fn(), playbackRate: 1.5,
      audio: audio as unknown as HTMLAudioElement,
    });
    const playing = session.play();
    session.setPlaybackRate(2);
    expect(audio.load).not.toHaveBeenCalled();
    respond({ ok: true, blob: async () => new Blob(['audio'], { type: 'audio/wav' }) });
    await playing;
    expect(audio.startRates).toEqual([[2, 2]]);

    audio.currentTime = 3;
    audio.load.mockClear();
    audio.play.mockClear();
    session.setPlaybackRate(0.5);
    expect([audio.defaultPlaybackRate, audio.playbackRate]).toEqual([0.5, 0.5]);
    expect(audio.currentTime).toBe(3);
    expect(audio.load).not.toHaveBeenCalled();
    expect(audio.play).not.toHaveBeenCalled();
    expect(audio.pause).not.toHaveBeenCalled();
    session.stop();
  });
});

describe('TextToSpeechSession pause', () => {
  it('pauses and continues the same audio element without loading it again', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => ({
      ok: true, blob: async () => new Blob(['audio'], { type: 'audio/wav' }),
    })));
    vi.stubGlobal('URL', TestURL);
    const audio = new FakeAudio();
    const onEnded = vi.fn();
    const session = new TextToSpeechSession({
      text: 'Answer', cachedUrl: '/media/1', onEnded, audio: audio as unknown as HTMLAudioElement,
    });
    await session.play();
    audio.load.mockClear();
    session.togglePause();
    expect(audio.pause).toHaveBeenCalledOnce();
    expect(audio.paused).toBe(true);
    session.togglePause();
    expect(audio.play).toHaveBeenCalledTimes(2);
    expect(audio.paused).toBe(false);
    expect(audio.load).not.toHaveBeenCalled();
    expect(onEnded).not.toHaveBeenCalled();
    session.stop();
    session.togglePause();
    expect(audio.play).toHaveBeenCalledTimes(2);
  });

  it('keeps playback when a pause interrupts a continue that has not started', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => ({
      ok: true, blob: async () => new Blob(['audio'], { type: 'audio/wav' }),
    })));
    vi.stubGlobal('URL', TestURL);
    const audio = new FakeAudio();
    const onEnded = vi.fn();
    const onError = vi.fn();
    const session = new TextToSpeechSession({
      text: 'Answer', cachedUrl: '/media/1', onEnded, onError, audio: audio as unknown as HTMLAudioElement,
    });
    await session.play();
    session.togglePause();
    // Audio that waits for data keeps play() pending; pause() then rejects it.
    let reject!: (error: unknown) => void;
    audio.play.mockImplementationOnce(() => {
      audio.paused = false;
      return new Promise<void>((_resolve, rejectPlay) => { reject = rejectPlay; });
    });
    session.togglePause();
    session.togglePause();
    reject(new DOMException('The play() request was interrupted by a call to pause().', 'AbortError'));
    await Promise.resolve();
    expect(onError).not.toHaveBeenCalled();
    expect(onEnded).not.toHaveBeenCalled();
    session.togglePause();
    expect(audio.play).toHaveBeenCalledTimes(3);
    session.stop();
  });
});
