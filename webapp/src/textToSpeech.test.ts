import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  getTextToSpeechConfiguration,
  TextToSpeechSession,
} from './textToSpeech';

afterEach(() => {
  delete window.chaTextToSpeech;
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('text to speech', () => {
  it('is available only when the native shell injects a complete configuration', () => {
    expect(getTextToSpeechConfiguration()).toBeNull();
    window.chaTextToSpeech = { url: 'https://example.com', apiKey: 'key', model: 'model' };
    expect(getTextToSpeechConfiguration()).toEqual(window.chaTextToSpeech);
  });

  it('requests ElevenLabs audio and plays it', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio'), {
        headers: { 'Content-Type': 'audio/mpeg' },
      }),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    const play = vi.fn().mockResolvedValue(undefined);
    const pause = vi.fn();
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play, pause };
    }));
    const session = new TextToSpeechSession(
      { url: 'https://example.com/speech', apiKey: 'secret', model: 'multilingual' },
      'Read this',
      vi.fn(),
    );

    await session.play();

    expect(fetchMock).toHaveBeenCalledWith('https://example.com/speech', expect.objectContaining({
      method: 'POST',
      headers: expect.objectContaining({ 'xi-api-key': 'secret' }),
      body: JSON.stringify({ text: 'Read this', model_id: 'multilingual' }),
    }));
    expect(play).toHaveBeenCalledOnce();
  });
});
