import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useState } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';

import { beginSpeechPlayback } from '../speechPlayback';
import { VoiceInputSession, type VoiceInputTransport } from '../voiceInput';
import { Composer } from './composer';
import type { ChaWebClient } from './client';
import { useVoiceInput } from './useVoiceInput';

const runtime = {
  provider: 'openai' as const, url: 'https://example.test/realtime', model: 'test',
  delay: 'low' as const, prompt: '', send_phrase: 'over to you',
};
const api = {
  getVoiceInputRuntime: vi.fn(async () => runtime),
  connectVoiceInput: vi.fn(), startXaiVoiceInput: vi.fn(), sendXaiVoiceAudio: vi.fn(),
  stopXaiVoiceInput: vi.fn(), cancelXaiVoiceInput: vi.fn(),
} as unknown as ChaWebClient;
const submitted = vi.fn();
let captures: Array<{
  text(text: string, provisional?: boolean): void;
  fail(failure: unknown): void;
  stop: ReturnType<typeof vi.fn<VoiceInputTransport['stop']>>;
  cancel: ReturnType<typeof vi.fn>;
}>;

function Harness({ blocked = false, speechBusy = false, sessionKey = 'session:lobby/one' }) {
  const [draft, setDraft] = useState('');
  const voice = useVoiceInput(api, sessionKey, draft, setDraft,
    (text) => { submitted(text); setDraft(''); }, blocked, speechBusy);
  return <>
    {voice.error && <p role="alert">{voice.error}</p>}
    <Composer value={draft} onChange={setDraft} expanded={false} onExpanded={() => {}}
      viewportHeight={600} onSessions={() => {}} onDelete={() => {}} onCopy={() => {}}
      copyDisabled deleteDisabled={false} deleting={false} copied={false}
      mode="send" commandDisabled={blocked || voice.finishing || !draft.trim()}
      onSend={voice.send} onStop={() => {}} voice={voice} />
  </>;
}

beforeEach(() => {
  captures = [];
  submitted.mockReset();
  vi.stubGlobal('isSecureContext', true);
  vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
  vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_config, text, fail) => {
    const capture = { text, fail, stop: vi.fn(async () => {}), cancel: vi.fn() };
    captures.push(capture);
    return capture;
  });
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

async function listen() {
  fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
  await waitFor(() => expect(captures).toHaveLength(1));
  await screen.findByRole('button', { name: 'Stop voice input' });
}

it('shows starting until the connection is ready and hides the indicator when setup is cancelled', async () => {
  let ready!: (transport: VoiceInputTransport) => void;
  vi.mocked(VoiceInputSession.start).mockImplementation(() => new Promise((resolve) => { ready = resolve; }));
  render(<Harness />);
  const mic = await screen.findByRole('button', { name: 'Start voice input' });
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  fireEvent.click(mic);
  expect(screen.getByRole('status')).toHaveTextContent('Microphone starting…');
  expect(screen.queryByText('Speak now')).not.toBeInTheDocument();
  const transport = { stop: vi.fn(async () => {}), cancel: vi.fn() };
  await act(async () => ready(transport));
  expect(screen.getByRole('status')).toHaveTextContent('Speak now');
  await act(async () => fireEvent.click(mic));
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  fireEvent.click(mic);
  expect(screen.getByRole('status')).toHaveTextContent('Microphone starting…');
  fireEvent.click(mic);
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  await act(async () => ready(transport));
  expect(transport.cancel).toHaveBeenCalledOnce();
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
});

it('shows paused during generation, audio loading, playback and the echo delay, then waits for reconnection', async () => {
  const view = render(<Harness />);
  await listen();
  expect(screen.getByRole('status')).toHaveTextContent('Speak now');
  view.rerender(<Harness blocked />);
  expect(screen.getByRole('status')).toHaveTextContent('Microphone paused');
  view.rerender(<Harness speechBusy />);
  expect(screen.getByRole('status')).toHaveTextContent('Microphone paused');
  let endPlayback!: () => void;
  act(() => { endPlayback = beginSpeechPlayback(); });
  view.rerender(<Harness />);
  expect(screen.getByRole('status')).toHaveTextContent('Microphone paused');
  let ready!: (transport: VoiceInputTransport) => void;
  vi.mocked(VoiceInputSession.start).mockImplementation(() => new Promise((resolve) => { ready = resolve; }));
  vi.useFakeTimers();
  act(() => endPlayback());
  await act(async () => { await vi.advanceTimersByTimeAsync(399); });
  expect(screen.getByRole('status')).toHaveTextContent('Microphone paused');
  await act(async () => { await vi.advanceTimersByTimeAsync(1); });
  expect(screen.getByRole('status')).toHaveTextContent('Microphone starting…');
  await act(async () => ready({ stop: vi.fn(async () => {}), cancel: vi.fn() }));
  expect(screen.getByRole('status')).toHaveTextContent('Speak now');
});

it.each(['Enter', 'Send'])('submits typed text with %s during playback and the echo delay', async (control) => {
  render(<Harness />);
  await screen.findByRole('button', { name: 'Start voice input' });
  const editor = screen.getByRole('textbox');
  function submit(text: string) {
    fireEvent.change(editor, { target: { value: text } });
    if (control === 'Enter') fireEvent.keyDown(editor, { key: 'Enter' });
    else fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  }
  vi.useFakeTimers();
  let endPlayback!: () => void;
  act(() => { endPlayback = beginSpeechPlayback(); });
  try {
    expect(screen.getByRole('button', { name: 'Start voice input' })).toBeDisabled();
    submit('During playback');
    expect(submitted).toHaveBeenCalledExactlyOnceWith('During playback');
    act(() => endPlayback());
    await act(async () => { await vi.advanceTimersByTimeAsync(200); });
    expect(screen.getByRole('button', { name: 'Start voice input' })).toBeDisabled();
    submit('During echo delay');
    expect(submitted).toHaveBeenCalledTimes(2);
    expect(submitted).toHaveBeenLastCalledWith('During echo delay');
    expect(captures).toHaveLength(0);
  } finally {
    act(() => endPlayback());
    await act(async () => { await vi.advanceTimersByTimeAsync(400); });
  }
});

it.each(['Enter', 'Send'])('submits typed text with %s while speech audio is loading', async (control) => {
  render(<Harness speechBusy />);
  expect(await screen.findByRole('button', { name: 'Start voice input' })).toBeDisabled();
  const editor = screen.getByRole('textbox');
  fireEvent.change(editor, { target: { value: 'While speech loads' } });
  if (control === 'Enter') fireEvent.keyDown(editor, { key: 'Enter' });
  else fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  expect(submitted).toHaveBeenCalledExactlyOnceWith('While speech loads');
  expect(captures).toHaveLength(0);
});

it('dictates and sends final words without focusing the editor', async () => {
  render(<Harness />);
  const editor = screen.getByRole('textbox');
  editor.focus();
  await listen();
  expect(editor).not.toHaveFocus();
  expect(editor).toHaveAttribute('readonly');
  act(() => captures[0].text('Hello', true));
  let complete!: () => void;
  captures[0].stop.mockImplementation(() => new Promise<void>((resolve) => { complete = resolve; }));
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  expect(submitted).not.toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  await act(async () => {
    captures[0].text('', true);
    captures[0].text('Hello world.');
    complete();
  });
  expect(submitted).toHaveBeenCalledExactlyOnceWith('Hello world.');
  expect(editor).not.toHaveFocus();
});

it('keeps the editor read-only until stopping the microphone finishes recognition', async () => {
  const user = userEvent.setup();
  render(<Harness />);
  await listen();
  act(() => captures[0].text('Hello', true));
  let complete!: () => void;
  captures[0].stop.mockImplementation(() => new Promise<void>((resolve) => { complete = resolve; }));
  fireEvent.click(screen.getByRole('button', { name: 'Stop voice input' }));
  const editor = screen.getByRole('textbox');
  expect(editor).toHaveAttribute('readonly');
  await user.type(editor, ' extra{Control>}{Enter}{/Control}');
  expect(editor).toHaveValue('Hello');
  await act(async () => {
    captures[0].text('', true);
    captures[0].text('Hello world.');
    complete();
  });
  expect(editor).not.toHaveAttribute('readonly');
  expect(editor).toHaveValue('Hello world.');
  await user.type(editor, ' Extra.');
  expect(editor).toHaveValue('Hello world. Extra.');
  expect(submitted).not.toHaveBeenCalled();
});

it('finalizes a spoken command, removes it, and submits once', async () => {
  render(<Harness />);
  await listen();
  vi.useFakeTimers();
  act(() => captures[0].text('Explain this. Over to you!'));
  expect(submitted).not.toHaveBeenCalled();
  await act(async () => { await vi.advanceTimersByTimeAsync(1_000); });
  expect(captures[0].stop).toHaveBeenCalledOnce();
  expect(submitted).toHaveBeenCalledExactlyOnceWith('Explain this.');
  expect(screen.getByRole('textbox')).not.toHaveFocus();
  await act(async () => { await vi.advanceTimersByTimeAsync(2_000); });
  expect(submitted).toHaveBeenCalledOnce();
});

it('does not send a command that disappears from final recognition', async () => {
  render(<Harness />);
  await listen();
  vi.useFakeTimers();
  act(() => captures[0].text('A draft. Over to you', true));
  captures[0].stop.mockImplementation(async () => {
    captures[0].text('', true);
    captures[0].text('A draft. More to do.');
  });
  await act(async () => { await vi.advanceTimersByTimeAsync(1_000); });
  expect(submitted).not.toHaveBeenCalled();
  expect(screen.getByRole('textbox')).toHaveValue('A draft. More to do.');
});

it('stops capture during generation and playback and discards late text', async () => {
  const view = render(<Harness />);
  await listen();
  act(() => captures[0].text('Keep this'));
  view.rerender(<Harness blocked />);
  expect(captures[0].cancel).toHaveBeenCalledOnce();
  act(() => captures[0].text('ignore this'));
  expect(screen.getByRole('textbox')).toHaveValue('Keep this');
  let endPlayback!: () => void;
  act(() => { endPlayback = beginSpeechPlayback(); });
  view.rerender(<Harness />);
  expect(captures).toHaveLength(1);
  vi.useFakeTimers();
  act(() => endPlayback());
  await act(async () => { await vi.advanceTimersByTimeAsync(400); });
  expect(captures).toHaveLength(2);
  act(() => captures[1].text('next'));
  expect(screen.getByRole('textbox')).toHaveValue('Keep this next');
});

it('cannot start capture during generation or playback', async () => {
  const view = render(<Harness blocked />);
  const mic = await screen.findByRole('button', { name: 'Start voice input' });
  expect(mic).toBeDisabled();
  fireEvent.click(mic);
  expect(captures).toHaveLength(0);
  let endPlayback!: () => void;
  act(() => { endPlayback = beginSpeechPlayback(); });
  view.rerender(<Harness />);
  expect(mic).toBeDisabled();
  vi.useFakeTimers();
  act(() => endPlayback());
  await act(async () => { await vi.advanceTimersByTimeAsync(400); });
  expect(mic).toBeEnabled();
});

it('stops dictation on navigation and keeps visible provisional text on failure', async () => {
  const view = render(<Harness />);
  await listen();
  act(() => captures[0].text('Keep this', true));
  act(() => captures[0].fail(new Error('Connection lost.')));
  expect(screen.getByRole('alert')).toHaveTextContent('Connection lost.');
  expect(screen.getByRole('textbox')).toHaveValue('Keep this');
  fireEvent.click(screen.getByRole('button', { name: 'Start voice input' }));
  await waitFor(() => expect(captures).toHaveLength(2));
  view.rerender(<Harness sessionKey="session:lobby/two" />);
  expect(captures[1].cancel).toHaveBeenCalledOnce();
  expect(screen.getByRole('button', { name: 'Start voice input' })).toHaveAttribute('aria-pressed', 'false');
});

it('preserves the draft and releases capture if finalization times out', async () => {
  render(<Harness />);
  await listen();
  act(() => captures[0].text('Keep this'));
  captures[0].stop.mockImplementation(() => new Promise(() => {}));
  vi.useFakeTimers();
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  await act(async () => { await vi.advanceTimersByTimeAsync(20_000); });
  expect(submitted).not.toHaveBeenCalled();
  expect(captures[0].cancel).toHaveBeenCalledOnce();
  expect(screen.getByRole('textbox')).toHaveValue('Keep this');
  expect(screen.getByRole('alert')).toHaveTextContent('Voice input timed out.');
});

it('does not expose microphone capture on an insecure page', async () => {
  vi.stubGlobal('isSecureContext', false);
  render(<Harness />);
  await act(async () => {});
  expect(screen.queryByRole('button', { name: 'Start voice input' })).not.toBeInTheDocument();
  expect(captures).toHaveLength(0);
});

it('releases dictation when the page is hidden without sending the draft', async () => {
  render(<Harness />);
  await listen();
  act(() => captures[0].text('Keep this'));
  vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('hidden');
  fireEvent(document, new Event('visibilitychange'));
  expect(captures[0].cancel).toHaveBeenCalledOnce();
  expect(screen.getByRole('button', { name: 'Start voice input' })).toHaveAttribute('aria-pressed', 'false');
  expect(screen.getByRole('textbox')).toHaveValue('Keep this');
  expect(submitted).not.toHaveBeenCalled();
});

it('keeps microphone mode enabled when the first prompt creates a session', async () => {
  const view = render(<Harness sessionKey="new:lobby" />);
  await listen();
  act(() => captures[0].text('First prompt'));
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  await waitFor(() => expect(submitted).toHaveBeenCalledOnce());
  view.rerender(<Harness sessionKey="session:lobby/created" blocked />);
  expect(screen.getByRole('button', { name: 'Stop voice input' })).toHaveAttribute('aria-pressed', 'true');
  const before = captures.length;
  view.rerender(<Harness sessionKey="session:lobby/created" />);
  await waitFor(() => expect(captures.length).toBeGreaterThan(before));
});
