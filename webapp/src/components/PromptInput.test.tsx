import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { fixtureClient } from '../test/fixtures';
import { VoiceInputSession, type VoiceInputTransport } from '../voiceInput';
import { beginSpeechPlayback } from '../speechPlayback';
import { PromptInput, type PromptInputProps } from './PromptInput';

const voiceRuntime = {
  provider: 'openai' as const,
  url: 'https://api.openai.com/v1/realtime/calls',
  model: 'gpt-live-transcribe',
  delay: 'low' as const,
  prompt: '',
  send_phrase: 'over to you',
};

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

function Harness({
  initial = '',
  ...props
}: Partial<PromptInputProps> & { initial?: string }) {
  const [value, setValue] = useState(initial);
  return (
    <PromptInput
      client={props.client ?? fixtureClient()}
      enabled={props.enabled ?? true}
      inputLabel="Message"
      microphoneEnabled={props.microphoneEnabled ?? true}
      onChange={props.onChange ?? setValue}
      onError={props.onError ?? (() => {})}
      onSubmit={props.onSubmit ?? (async () => ({ clear_input: true }))}
      statusRow={props.statusRow ?? (() => null)}
      submitEnabled={props.submitEnabled ?? true}
      submitLabel="Send message"
      formControls={props.formControls}
      onSubmittingChange={props.onSubmittingChange}
      placeholder={props.placeholder}
      stopAction={props.stopAction}
      value={props.value ?? value}
    />
  );
}

describe('PromptInput', () => {
  it.each(['openai', 'xai'] as const)('discards delayed %s recognition during playback and its echo delay', async (provider) => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop: vi.fn(async () => {}), cancel: vi.fn() };
    });
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    const client = fixtureClient({ getVoiceInputRuntime: async () => ({ ...voiceRuntime, provider }) });
    const view = render(<Harness client={client} initial="Draft." onSubmit={onSubmit} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    const input = screen.getByRole('textbox', { name: 'Message' });
    vi.useFakeTimers();
    const endPlayback = beginSpeechPlayback();
    try {
      act(() => {
        appendVoice('Background', true);
        appendVoice('', true);
        appendVoice('Background over to you');
      });
      expect(input).toHaveValue('Draft.');
      fireEvent.change(input, { target: { value: 'Typed during playback.' } });
      endPlayback();
      await act(async () => { await vi.advanceTimersByTimeAsync(399); });
      act(() => appendVoice('Late echo', true));
      expect(input).toHaveValue('Typed during playback.');
      await act(async () => { await vi.advanceTimersByTimeAsync(1); });
      // OpenAI repeats the cumulative preview in its final result after resume.
      if (provider === 'openai') act(() => {
        appendVoice('', true);
        appendVoice('Late echo');
      });
      expect(input).toHaveValue('Typed during playback.');
      await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
      expect(onSubmit).not.toHaveBeenCalled();
      act(() => {
        appendVoice('New speech', true);
        appendVoice('', true);
        appendVoice('New speech');
      });
      expect(input).toHaveValue('Typed during playback. New speech');
    } finally {
      view.unmount();
      endPlayback();
      await vi.advanceTimersByTimeAsync(400);
    }
  });

  it('removes an xAI preview shown before playback', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop: vi.fn(async () => {}), cancel: vi.fn() };
    });
    const client = fixtureClient({ getVoiceInputRuntime: async () => ({ ...voiceRuntime, provider: 'xai' as const }) });
    const view = render(<Harness client={client} initial="Draft." />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    const input = screen.getByRole('textbox', { name: 'Message' });
    act(() => appendVoice('Hello', true));
    expect(input).toHaveValue('Draft. Hello');
    vi.useFakeTimers();
    const endPlayback = beginSpeechPlayback();
    try {
      act(() => {
        appendVoice('', true);
        appendVoice('Hello world.');
      });
      expect(input).toHaveValue('Draft.');
      endPlayback();
      await act(async () => { await vi.advanceTimersByTimeAsync(400); });
      act(() => {
        appendVoice('Next', true);
        appendVoice('', true);
        appendVoice('Next.');
      });
      expect(input).toHaveValue('Draft. Next.');
    } finally {
      view.unmount();
      endPlayback();
      await vi.advanceTimersByTimeAsync(400);
    }
  });

  it('types and submits without a voice runtime', async () => {
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    render(<Harness onSubmit={onSubmit} />);
    const input = screen.getByRole('textbox', { name: 'Message' });
    expect(screen.queryByRole('button', { name: 'Start voice input' })).not.toBeInTheDocument();
    fireEvent.change(input, { target: { value: 'Hello' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledExactlyOnceWith('Hello'));
    expect(input).toHaveValue('');
  });

  it('submits on Enter, inserts a line on Ctrl+Enter, and ignores IME Enter', async () => {
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    render(<Harness onSubmit={onSubmit} />);
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'ab' } });
    (input as HTMLTextAreaElement).setSelectionRange(1, 1);
    fireEvent.keyDown(input, { key: 'Enter', ctrlKey: true });
    expect(input).toHaveValue('a\nb');
    expect(onSubmit).not.toHaveBeenCalled();

    const composing = new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true });
    Object.defineProperty(composing, 'isComposing', { value: true });
    input.dispatchEvent(composing);
    expect(onSubmit).not.toHaveBeenCalled();
    expect(input).toHaveValue('a\nb');

    fireEvent.keyDown(input, { key: 'Enter' });
    await waitFor(() => expect(onSubmit).toHaveBeenCalledExactlyOnceWith('a\nb'));
  });

  it('clears only an accepted prefix and keeps a rejected or replaced draft', async () => {
    let finish: (result: { clear_input: boolean }) => void = () => {};
    let fail: (reason: unknown) => void = () => {};
    const onSubmit = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve, reject) => {
      finish = resolve;
      fail = reject;
    }));
    const onSubmittingChange = vi.fn();
    render(
      <Harness onSubmit={onSubmit} onSubmittingChange={onSubmittingChange} />,
    );
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Original' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    expect(onSubmittingChange).toHaveBeenCalledWith(true);
    expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
    await waitFor(() => expect(onSubmit).toHaveBeenCalledWith('Original'));
    fireEvent.change(input, { target: { value: 'Original More text' } });
    await act(async () => { finish({ clear_input: true }); });
    expect(input).toHaveValue('More text');
    expect(onSubmittingChange).toHaveBeenLastCalledWith(false);

    fireEvent.change(input, { target: { value: 'Second' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledWith('Second'));
    fireEvent.change(input, { target: { value: 'Replaced' } });
    await act(async () => { finish({ clear_input: true }); });
    expect(input).toHaveValue('Replaced');

    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledWith('Replaced'));
    await act(async () => { fail(new Error('nope')); });
    expect(input).toHaveValue('Replaced');
  });

  it('submits the draft present when transcription finishes', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let finishRecording: () => void = () => {};
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async () => ({
      stop: () => new Promise<void>((resolve) => { finishRecording = resolve; }),
      cancel: vi.fn(),
    }));
    let finish: (result: { clear_input: boolean }) => void = () => {};
    const onSubmit = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => {
      finish = resolve;
    }));
    render(<Harness
      client={fixtureClient({ getVoiceInputRuntime: async () => voiceRuntime })}
      onSubmit={onSubmit}
    />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Original' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    fireEvent.change(input, { target: { value: 'Original More text' } });
    await act(async () => { finishRecording(); });
    await waitFor(() => expect(onSubmit).toHaveBeenCalledWith('Original More text'));
    fireEvent.change(input, { target: { value: 'Original More text tail' } });
    await act(async () => { finish({ clear_input: true }); });
    expect(input).toHaveValue('tail');
  });

  it('keeps Stop in place of Send and blocks form and Enter submission', () => {
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    const onStop = vi.fn();
    render(
      <Harness
        initial="Waiting"
        onSubmit={onSubmit}
        submitEnabled={false}
        stopAction={(
          <button aria-label="Stop generation" onClick={onStop} type="button">Stop</button>
        )}
      />,
    );
    expect(screen.queryByRole('button', { name: 'Send message' })).not.toBeInTheDocument();
    const stop = screen.getByRole('button', { name: 'Stop generation' });
    expect(stop).toBeEnabled();
    fireEvent.click(stop);
    expect(onStop).toHaveBeenCalledOnce();
    fireEvent.submit(screen.getByRole('textbox', { name: 'Message' }).closest('form')!);
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
    expect(onSubmit).not.toHaveBeenCalled();
  });

  it('sends one phrase-free prompt after Stop is removed and keeps recording', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    const stop = vi.fn(async () => {});
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop, cancel: vi.fn() };
    });
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    const client = fixtureClient({ getVoiceInputRuntime: async () => voiceRuntime });
    const { rerender } = render(
      <Harness
        client={client}
        onSubmit={onSubmit}
        stopAction={<button aria-label="Stop generation" type="button">Stop</button>}
      />,
    );
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    act(() => appendVoice('Next question. Over to you'));
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1100)); });
    expect(onSubmit).not.toHaveBeenCalled();
    expect(stop).not.toHaveBeenCalled();

    rerender(<Harness client={client} onSubmit={onSubmit} />);
    await waitFor(() => expect(onSubmit).toHaveBeenCalledExactlyOnceWith('Next question.'), { timeout: 5000 });
    expect(stop).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Stop voice input' })).toBeEnabled();
  });

  it('drops a pending spoken send when input is disabled and ignores a late transcript', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    const cancel = vi.fn();
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop: vi.fn(async () => {}), cancel };
    });
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    const client = fixtureClient({ getVoiceInputRuntime: async () => voiceRuntime });
    const { rerender } = render(<Harness client={client} onSubmit={onSubmit} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    act(() => appendVoice('Question over to you'));
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Question over to you');

    rerender(<Harness client={client} enabled={false} onSubmit={onSubmit} />);
    expect(cancel).toHaveBeenCalled();
    act(() => appendVoice(' late'));
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Question over to you');

    rerender(<Harness client={client} onSubmit={onSubmit} />);
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1200)); });
    expect(onSubmit).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Question over to you');
  });

  it('ignores a removed prompt after remount and clears a pending spoken-send timer', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop: vi.fn(async () => {}), cancel: vi.fn() } as VoiceInputTransport;
    });
    let finish: (result: { clear_input: boolean }) => void = () => {};
    const onSubmit = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => {
      finish = resolve;
    }));
    const parent = { submitting: false };
    const client = fixtureClient({ getVoiceInputRuntime: async () => voiceRuntime });
    function Mounted({ instance }: { instance: string }) {
      const [value, setValue] = useState('Hello');
      return (
        <PromptInput
          key={instance}
          client={client}
          enabled
          inputLabel="Message"
          microphoneEnabled
          onChange={setValue}
          onError={() => {}}
          onSubmit={onSubmit}
          onSubmittingChange={(submitting) => { parent.submitting = submitting; }}
          statusRow={() => null}
          submitEnabled
          submitLabel="Send message"
          value={value}
        />
      );
    }
    const { rerender } = render(<Mounted instance="A" />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledOnce());
    act(() => appendVoice(' over to you'));
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Hello over to you');
    expect(parent.submitting).toBe(true);

    rerender(<Mounted instance="B" />);
    expect(parent.submitting).toBe(false);
    await act(async () => { finish({ clear_input: true }); });
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1200)); });
    expect(onSubmit).toHaveBeenCalledOnce();
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Hello over to you');
  });

  it('does not send a spoken phrase after the prompt unmounts during the delay', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop: vi.fn(async () => {}), cancel: vi.fn() };
    });
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    const client = fixtureClient({ getVoiceInputRuntime: async () => voiceRuntime });
    function Mounted({ instance }: { instance: string }) {
      const [value, setValue] = useState('');
      return (
        <PromptInput
          key={instance}
          client={client}
          enabled
          inputLabel="Message"
          microphoneEnabled
          onChange={setValue}
          onError={() => {}}
          onSubmit={onSubmit}
          statusRow={() => null}
          submitEnabled
          submitLabel="Send message"
          value={value}
        />
      );
    }
    const { rerender } = render(<Mounted instance="A" />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    act(() => appendVoice('Ask this over to you'));
    rerender(<Mounted instance="B" />);
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1200)); });
    expect(onSubmit).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Ask this over to you');
  });
});
