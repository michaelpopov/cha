import { act, renderHook, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';

import { VoiceInputSession } from '../voiceInput';
import { createChaWebClient } from './client';
import { useVoiceInput } from './useVoiceInput';

afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); });

it('cancels capture on leaving a conversation and ignores its late transcription and send completion', async () => {
  vi.stubGlobal('isSecureContext', true);
  vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
  let transcribe!: (text: string, provisional?: boolean) => void;
  let finish!: () => void;
  const transport = { cancel: vi.fn(), stop: vi.fn(() => new Promise<void>((resolve) => { finish = resolve; })) };
  vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_runtime, onText) => {
    transcribe = onText;
    return transport;
  });
  const client = { ...createChaWebClient(), getVoiceInputRuntime: vi.fn(async () => ({
    provider: 'openai' as const, url: 'https://voice.test', model: 'test', delay: 'low' as const,
    prompt: '', send_phrase: 'send',
  })) };
  const onDraft = vi.fn();
  const onSend = vi.fn();
  const { result, rerender } = renderHook(({ key, blocked }) => useVoiceInput(
    client, key, 'Unsent draft', onDraft, onSend, blocked),
  { initialProps: { key: 'session:lobby/planning', blocked: false } });
  await waitFor(() => expect(result.current.available).toBe(true));
  act(() => result.current.setListening(true));
  await waitFor(() => expect(result.current.status).toBe('recording'));
  act(() => result.current.send());
  expect(transport.stop).toHaveBeenCalledOnce();
  rerender({ key: 'none', blocked: true });
  expect(transport.cancel).toHaveBeenCalledOnce();
  act(() => transcribe('Late words send'));
  await act(async () => finish());
  expect(onDraft).not.toHaveBeenCalled();
  expect(onSend).not.toHaveBeenCalled();
  expect(result.current.enabled).toBe(false);
  expect(client.getVoiceInputRuntime).toHaveBeenCalledTimes(1);
});
