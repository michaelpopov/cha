import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { fixtureClient } from '../test/fixtures';
import { PromptInput, type PromptInputProps } from './PromptInput';

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
