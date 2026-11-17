import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { VoiceInputUsage } from '../api/client';
import { editChatDraft, emptyPromptDrafts } from '../state/promptDrafts';
import { fixtureClient } from '../test/fixtures';
import { VoiceInputSession } from '../voiceInput';
import { PromptInput, type PromptInputProps } from './PromptInput';
import { TransliterationProvider } from './TransliterationMode';

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
    <TransliterationProvider controlsVisible><PromptInput
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
    /></TransliterationProvider>
  );
}

describe('PromptInput', () => {

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

  it.each([
    { initial: '', start: 0, end: 0, pasted: 'shhuka + hello\nПривет' },
    { initial: 'с', start: 1, end: 1, pasted: 'h' },
    { initial: 'до мир после', start: 3, end: 6, pasted: 'https://example.com/a+b' },
  ])('preserves pasted text with transliteration enabled: $pasted', async ({ initial, start, end, pasted }) => {
    const user = userEvent.setup();
    const onSubmit = vi.fn(async () => ({ clear_input: true }));
    render(<Harness initial={initial} onSubmit={onSubmit} statusRow={({ transliteration }) => transliteration} />);
    await user.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
    const input = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
    input.setSelectionRange(start, end);

    await user.paste(pasted);

    const prefix = initial.slice(0, start) + pasted;
    const suffix = initial.slice(end);
    expect(input).toHaveValue(prefix + suffix);
    expect(input.selectionStart).toBe(prefix.length);
    expect(input.selectionEnd).toBe(prefix.length);

    await user.keyboard('privet');
    expect(input).toHaveValue(`${prefix}привет${suffix}`);
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledExactlyOnceWith(`${prefix}привет${suffix}`));
  });

  it.each([
    { pasted: 'C++', typed: ' is', expected: 'C++ ис' },
    { pasted: 'мой', typed: 'a', expected: 'мойа' },
  ])('does not join typed text to the end of pasted text: $pasted', async ({ pasted, typed, expected }) => {
    const user = userEvent.setup();
    render(<Harness statusRow={({ transliteration }) => transliteration} />);
    await user.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
    await user.paste(pasted);
    await user.keyboard(typed);
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue(expected);
  });

  it('preserves dropped text with transliteration enabled', async () => {
    const user = userEvent.setup();
    render(<Harness statusRow={({ transliteration }) => transliteration} />);
    await user.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.input(input, { target: { value: 'https://example.com' }, inputType: 'insertFromDrop' });
    expect(input).toHaveValue('https://example.com');
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

  it('sends dictation usage until a prompt is accepted', async () => {
    let hear: (text: string) => void = () => {};
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (
      _configuration, onTranscription, _onFailure, _connect, _xai, _signal, _capture, recording,
    ) => {
      hear = onTranscription;
      return { cancel: vi.fn(), stop: vi.fn(async () => { if (recording) recording.audio_seconds = 3.5; }) };
    });
    const client = fixtureClient({
      getVoiceInputRuntime: async () => ({
        provider: 'xai', url: 'https://voice.test', model: 'test', delay: 'low', prompt: '', send_phrase: 'send',
      }),
    });
    let finish: (result: { clear_input: boolean }) => void = () => {};
    const onSubmit = vi.fn((_text: string, _usage?: VoiceInputUsage[]) =>
      new Promise<{ clear_input: boolean }>((resolve) => { finish = resolve; }));
    // Keep recordings with the draft, as App does.
    function Dictation() {
      const [drafts, setDrafts] = useState(emptyPromptDrafts);
      return (
        <TransliterationProvider controlsVisible><PromptInput
          client={client}
          enabled
          inputLabel="Message"
          microphoneEnabled
          onChange={(text, recording) => setDrafts((current) => editChatDraft(current, 'chat', text, recording))}
          onError={() => {}}
          onSubmit={onSubmit}
          recordings={drafts.chat.recordings}
          statusRow={() => null}
          submitEnabled
          submitLabel="Send message"
          value={drafts.chat.text}
        /></TransliterationProvider>
      );
    }
    render(<Dictation />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    act(() => hear('Book the train'));
    const input = screen.getByRole<HTMLTextAreaElement>('textbox', { name: 'Message' });
    const dictated = input.value;
    const usage = [{ provider: 'xai', model: 'test', audio_seconds: 3.5 }];

    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenLastCalledWith(dictated, usage));
    await act(async () => { finish({ clear_input: false }); });
    // A rejected prompt keeps its usage for the next attempt.
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledTimes(2));
    expect(onSubmit).toHaveBeenLastCalledWith(dictated, usage);
    fireEvent.change(input, { target: { value: `${dictated} More` } });
    await act(async () => { finish({ clear_input: true }); });
    expect(input).toHaveValue('More');
    // The accepted audio is not reported again with the rest of the draft.
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(onSubmit).toHaveBeenCalledTimes(3));
    expect(onSubmit).toHaveBeenLastCalledWith('More');
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

});
