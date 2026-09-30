import { afterEach, expect, it, vi } from 'vitest';

import { copyText } from './clipboard';

const originalCommand = Object.getOwnPropertyDescriptor(document, 'execCommand');

afterEach(() => {
  vi.unstubAllGlobals();
  if (originalCommand) Object.defineProperty(document, 'execCommand', originalCommand);
  else Reflect.deleteProperty(document, 'execCommand');
});

it('writes the complete text with the clipboard API', async () => {
  const writeText = vi.fn(async () => {});
  vi.stubGlobal('navigator', { clipboard: { writeText } });
  const text = 'Reader\nQuestion\n\nGuide\nReply\nwith another line';
  await copyText(text);
  expect(writeText).toHaveBeenCalledExactlyOnceWith(text);
});

it('uses the HTTP fallback and restores focus and the draft selection', async () => {
  vi.stubGlobal('navigator', {});
  const draft = document.createElement('textarea');
  draft.value = 'Unsent prompt';
  document.body.append(draft);
  draft.focus();
  draft.setSelectionRange(2, 5);
  const command = vi.fn(() => {
    const temporary = document.body.lastElementChild as HTMLTextAreaElement;
    expect(temporary.value).toBe('Whole conversation');
    expect(temporary.selectionStart).toBe(0);
    expect(temporary.selectionEnd).toBe(18);
    return true;
  });
  Object.defineProperty(document, 'execCommand', { configurable: true, value: command });
  await copyText('Whole conversation');
  expect(command).toHaveBeenCalledExactlyOnceWith('copy');
  expect(document.querySelectorAll('textarea')).toHaveLength(1);
  expect(draft).toHaveFocus();
  expect(draft).toHaveValue('Unsent prompt');
  expect([draft.selectionStart, draft.selectionEnd]).toEqual([2, 5]);
  draft.remove();
});

it('reports clipboard rejection and an unsuccessful fallback', async () => {
  vi.stubGlobal('navigator', { clipboard: { writeText: vi.fn(async () => { throw new Error('Denied'); }) } });
  await expect(copyText('Conversation')).rejects.toThrow('Denied');
  vi.stubGlobal('navigator', {});
  Object.defineProperty(document, 'execCommand', { configurable: true, value: () => false });
  await expect(copyText('Conversation')).rejects.toThrow('Copy failed');
  expect(document.querySelector('textarea')).not.toBeInTheDocument();
});
