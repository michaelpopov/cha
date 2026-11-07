import { act, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, expect, it, vi } from 'vitest';

import { Conversation } from './conversation';

afterEach(() => vi.restoreAllMocks());

const props = {
  characters: [], personas: [], draft: 'Unsent prompt', onDraft: vi.fn(),
  expanded: false, onExpanded: vi.fn(), viewportHeight: 700, sessionKey: 'lobby/planning',
  deleting: false,
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

it('copies individual messages, disables duplicate requests, and confirms success', async () => {
  const user = userEvent.setup();
  let finish!: () => void;
  const writeText = vi.spyOn(navigator.clipboard, 'writeText')
    .mockImplementation(() => new Promise<void>((resolve) => { finish = resolve; }));
  render(<Conversation {...props} />);
  const copyResponse = screen.getByRole('button', { name: 'Copy response' });
  expect(copyResponse.textContent).toBe('');
  await user.click(copyResponse);
  expect(writeText).toHaveBeenLastCalledWith('Reply');
  expect(copyResponse).toBeDisabled();
  await act(async () => finish());
  expect(copyResponse).toHaveAccessibleName('Copied to clipboard');
  expect(screen.getByRole('textbox')).toHaveValue('Unsent prompt');

  const copyPrompt = screen.getByRole('button', { name: 'Copy prompt' });
  expect(copyPrompt.textContent).toBe('');
  await user.click(copyPrompt);
  expect(writeText).toHaveBeenLastCalledWith('Question');
  expect(copyPrompt).toBeDisabled();
  await act(async () => finish());
  expect(copyPrompt).toHaveAccessibleName('Copied to clipboard');
  expect(screen.getByRole('textbox')).toHaveValue('Unsent prompt');
});

it('reports a failure and allows retry without changing the prompt', async () => {
  const user = userEvent.setup();
  const writeText = vi.spyOn(navigator.clipboard, 'writeText')
    .mockRejectedValueOnce(new Error('Denied')).mockResolvedValue(undefined);
  render(<Conversation {...props} />);
  await user.click(screen.getByRole('button', { name: 'Copy response' }));
  expect(screen.getByRole('alert')).toHaveTextContent('Could not copy the response. Try again.');
  await user.click(screen.getByRole('button', { name: 'Copy response' }));
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(writeText).toHaveBeenLastCalledWith('Reply');
  expect(screen.getByRole('button', { name: 'Copied to clipboard' })).toBeEnabled();

  writeText.mockRejectedValueOnce(new Error('Denied'));
  await user.click(screen.getByRole('button', { name: 'Copy prompt' }));
  expect(screen.getByRole('alert')).toHaveTextContent('Could not copy the prompt. Try again.');
  await user.click(screen.getByRole('button', { name: 'Copy prompt' }));
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(writeText).toHaveBeenLastCalledWith('Question');
  expect(screen.getByRole('textbox')).toHaveValue('Unsent prompt');
});

it('has no message copy controls for an empty conversation and disables them during deletion', () => {
  const { rerender } = render(<Conversation {...props} entries={[]} />);
  expect(screen.queryByRole('button', { name: 'Copy response' })).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Copy prompt' })).not.toBeInTheDocument();
  rerender(<Conversation {...props} deleting />);
  expect(screen.getByRole('button', { name: 'Copy response' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Copy prompt' })).toBeDisabled();
});
