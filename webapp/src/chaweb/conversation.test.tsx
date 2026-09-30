import { act, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, expect, it, vi } from 'vitest';

import { Conversation } from './conversation';

afterEach(() => vi.restoreAllMocks());

const props = {
  characters: [], personas: [], draft: 'Unsent prompt', onDraft: vi.fn(),
  expanded: false, onExpanded: vi.fn(), viewportHeight: 700, sessionKey: 'lobby/planning',
  onSessions: vi.fn(), onDelete: vi.fn(), deleteDisabled: false, deleting: false,
  mode: 'send' as const, commandDisabled: false, onSend: vi.fn(), onStop: vi.fn(), notice: null,
  entries: [{
    id: 1, kind: 'human' as const, participant_id: 'reader', display_name: 'Reader',
    addressed_to: '', addressed_to_name: '', text: 'Question',
    status: 'complete' as const, created_at: 1_700_000_000,
  }, {
    id: 2, kind: 'character' as const, participant_id: 'guide', display_name: 'Guide',
    addressed_to: '', addressed_to_name: '', text: 'Reply',
    status: 'complete' as const, created_at: 1_700_000_001,
  }],
};

// jsdom does not lay out text; the browser check covers the actual innerText.
function renderedText() {
  screen.getByLabelText('Conversation transcript').querySelectorAll<HTMLElement>(
    '.chaweb-speaker, .chaweb-entry-text, .chaweb-entry-status',
  ).forEach((part) => {
    Object.defineProperty(part, 'innerText', { configurable: true, value: part.textContent?.trim() });
  });
}

it('copies speakers and messages without their timestamps, disables duplicate requests, and confirms success', async () => {
  const user = userEvent.setup();
  let finish!: () => void;
  const writeText = vi.spyOn(navigator.clipboard, 'writeText')
    .mockImplementation(() => new Promise<void>((resolve) => { finish = resolve; }));
  render(<Conversation {...props} />);
  renderedText();
  await user.click(screen.getByRole('button', { name: 'Copy conversation' }));
  expect(writeText).toHaveBeenCalledExactlyOnceWith('Reader\nQuestion\n\nGuide\nReply');
  expect(document.querySelectorAll('.chaweb-entry-time')).toHaveLength(2);
  expect(screen.getByRole('button', { name: 'Copy conversation' })).toBeDisabled();
  await act(async () => finish());
  expect(screen.getByRole('status')).toHaveTextContent('Copied to clipboard');
  expect(screen.getByRole('textbox')).toHaveValue('Unsent prompt');
  expect(props.onSend).not.toHaveBeenCalled();
});

it('reports a failure and allows retry without changing the prompt', async () => {
  const user = userEvent.setup();
  const writeText = vi.spyOn(navigator.clipboard, 'writeText')
    .mockRejectedValueOnce(new Error('Denied')).mockResolvedValue(undefined);
  render(<Conversation {...props} />);
  renderedText();
  await user.click(screen.getByRole('button', { name: 'Copy conversation' }));
  expect(screen.getByRole('alert')).toHaveTextContent('Could not copy the conversation. Try again.');
  expect(screen.getByRole('textbox')).toHaveValue('Unsent prompt');
  await user.click(screen.getByRole('button', { name: 'Copy conversation' }));
  expect(writeText).toHaveBeenCalledTimes(2);
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('status')).toHaveTextContent('Copied to clipboard');
});

it('disables copying an empty conversation or one being deleted', () => {
  const { rerender } = render(<Conversation {...props} entries={[]} />);
  expect(screen.getByRole('button', { name: 'Copy conversation' })).toBeDisabled();
  rerender(<Conversation {...props} deleting />);
  expect(screen.getByRole('button', { name: 'Copy conversation' })).toBeDisabled();
});

it('ignores clipboard completion after switching to another session', async () => {
  const user = userEvent.setup();
  let finish!: () => void;
  vi.spyOn(navigator.clipboard, 'writeText')
    .mockImplementation(() => new Promise<void>((resolve) => { finish = resolve; }));
  const { rerender } = render(<Conversation {...props} />);
  renderedText();
  await user.click(screen.getByRole('button', { name: 'Copy conversation' }));
  rerender(<Conversation {...props} sessionKey="lobby/another" />);
  await act(async () => finish());
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Copy conversation' })).toBeEnabled();
});
