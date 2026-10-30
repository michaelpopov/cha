import { createEvent, fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { fixtureClient } from '../test/fixtures';
import { PromptInput, type PromptInputProps } from './PromptInput';

function prompt(props: Partial<PromptInputProps> = {}) {
  return <PromptInput
    client={fixtureClient()}
    enabled
    inputLabel="Message"
    microphoneEnabled={false}
    onChange={vi.fn()}
    onError={vi.fn()}
    onSubmit={vi.fn()}
    statusRow={() => null}
    submitEnabled
    submitLabel="Send message"
    value="Keep my draft"
    {...props}
  />;
}

describe('prompt file drops', () => {
  it('highlights only file drags and clears the highlight on exit, drop, or becoming unavailable', () => {
    const onFileDrop = vi.fn();
    const { rerender } = render(prompt({ onFileDrop }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    const form = input.closest('form');
    const dataTransfer = { types: ['Files'], files: [new File(['text'], 'notes.txt')], dropEffect: 'none' };

    expect(fireEvent.dragOver(input, { dataTransfer })).toBe(false);
    expect(dataTransfer.dropEffect).toBe('copy');
    expect(form).toHaveClass('is-file-dragging');
    fireEvent.dragLeave(input);
    expect(form).not.toHaveClass('is-file-dragging');
    fireEvent.dragEnter(input, { dataTransfer });
    expect(form).toHaveClass('is-file-dragging');
    expect(fireEvent.drop(input, { dataTransfer })).toBe(false);
    expect(onFileDrop).toHaveBeenCalledExactlyOnceWith(dataTransfer.files[0]);
    expect(form).not.toHaveClass('is-file-dragging');
    expect(input).toHaveValue('Keep my draft');

    fireEvent.dragOver(input, { dataTransfer });
    rerender(prompt());
    expect(form).not.toHaveClass('is-file-dragging');
    dataTransfer.dropEffect = 'copy';
    expect(fireEvent.dragOver(input, { dataTransfer })).toBe(false);
    expect(dataTransfer.dropEffect).toBe('none');
    expect(fireEvent.drop(input, { dataTransfer })).toBe(false);
    expect(onFileDrop).toHaveBeenCalledTimes(1);
  });

  it('accepts a file anywhere in the composer and keeps the highlight inside it', () => {
    const onFileDrop = vi.fn();
    render(prompt({ onFileDrop }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    const send = screen.getByRole('button', { name: 'Send message' });
    const form = input.closest('form');
    const dataTransfer = { types: ['Files'], files: [new File(['text'], 'notes.txt')], dropEffect: 'none' };

    fireEvent.dragEnter(input, { dataTransfer });
    const leave = createEvent.dragLeave(input);
    Object.defineProperty(leave, 'relatedTarget', { value: send });
    fireEvent(input, leave);
    expect(form).toHaveClass('is-file-dragging');
    expect(fireEvent.dragOver(send, { dataTransfer })).toBe(false);
    expect(fireEvent.drop(send, { dataTransfer })).toBe(false);
    expect(onFileDrop).toHaveBeenCalledExactlyOnceWith(dataTransfer.files[0]);
    expect(form).not.toHaveClass('is-file-dragging');
  });

  it('leaves text drags to the browser', () => {
    const onFileDrop = vi.fn();
    render(prompt({ onFileDrop }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    const dataTransfer = { types: ['text/plain'], files: [], dropEffect: 'move' };
    expect(fireEvent.dragOver(input, { dataTransfer })).toBe(true);
    expect(input.closest('form')).not.toHaveClass('is-file-dragging');
    expect(fireEvent.drop(input, { dataTransfer })).toBe(true);
    expect(onFileDrop).not.toHaveBeenCalled();
  });
});
