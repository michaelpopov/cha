import { act, renderHook, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';

import type { SessionSnapshot } from '../api/client';
import { snapshotFixture, voiceOutputRuntimeFixture } from '../test/fixtures';
import { TextToSpeechSession } from '../textToSpeech';
import { createChaWebClient } from './client';
import { useReadAloud } from './useReadAloud';

afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); });

it('stops local playback and disables automatic audio when the conversation is left', async () => {
  const pause = vi.fn();
  vi.stubGlobal('Audio', class { src = ''; play = vi.fn(async () => undefined); pause = pause; });
  const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
  const stop = vi.spyOn(TextToSpeechSession.prototype, 'stop').mockImplementation(() => undefined);
  const entry = { ...snapshotFixture.transcript[0], id: 1, kind: 'character' as const,
    status: 'complete' as const, text: 'Cached reply', has_cached_audio: true };
  const snapshot = { ...snapshotFixture, transcript: [entry] };
  const client = { ...createChaWebClient(), audioUrl: () => '/test-audio', getVoiceOutputRuntime: vi.fn(async () => voiceOutputRuntimeFixture),
    getAudioStatus: vi.fn(async () => ({ cached_entry_ids: [1], downloads: [] })) };
  const { result, rerender } = renderHook(({ current }: { current: SessionSnapshot | null }) => useReadAloud(
    client, current, 'Test', false, current ? 'session:lobby/planning' : ''),
  { initialProps: { current: snapshot as SessionSnapshot | null } });
  await waitFor(() => expect(result.current.available).toBe(true));
  act(() => result.current.toggleAutomatic());
  expect(result.current.automatic).toBe(true);
  act(() => result.current.toggle(entry));
  await waitFor(() => expect(play).toHaveBeenCalledOnce());
  rerender({ current: null });
  expect(stop).toHaveBeenCalledOnce();
  expect(pause).toHaveBeenCalled();
  expect(result.current.automatic).toBe(false);
  expect(result.current.entryId).toBeNull();
});
