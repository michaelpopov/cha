import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError, type PromptDetail } from '../api/client';
import { DetailRefreshProvider } from '../detailRefresh';
import { initialAppState } from '../state/view';
import { fixtureClient } from '../test/fixtures';
import { PromptScreen, PromptsScreen } from './PromptSettings';

const detail: PromptDetail = {
  id: 'note-compression',
  display_name: 'Note compression',
  text: '# Literal\n\n<script>danger()</script>\n  indented  ',
  writable: true,
};
const state = {
  ...initialAppState,
  inspectedPrompt: { id: detail.id, name: detail.display_name },
};
const edit = () => fireEvent.click(screen.getByRole('button', { name: 'Edit text' }));
const save = () => screen.getByRole('button', { name: 'Save' });
const field = () => screen.getByRole('textbox', { name: 'Prompt text' });
const cancel = () => fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

describe('Prompt settings', () => {
  it('shows fixed entries and retries a failed list', async () => {
    const navigate = vi.fn();
    const listPrompts = vi.fn().mockRejectedValueOnce(new Error()).mockResolvedValueOnce([
      { id: detail.id, display_name: detail.display_name },
      { id: 'summary-l1', display_name: 'Summary L1' },
      { id: 'summary-l2', display_name: 'Summary L2' },
      { id: 'summary-l3-section', display_name: 'Summary L3 section' },
      { id: 'summary-l3-overview', display_name: 'Summary L3 overview' },
      { id: 'session-short-summary', display_name: 'Short session summary' },
      { id: 'session-long-summary', display_name: 'Long session summary' },
      { id: 'page-formatting', display_name: 'Page formatting' },
      { id: 'youtube-transcript', display_name: 'YouTube transcript formatting' },
    ]);
    render(<PromptsScreen client={fixtureClient({ listPrompts })} navigate={navigate} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Try again' }));
    await screen.findByRole('button', { name: detail.display_name });
    expect(screen.getAllByRole('button').map(button => button.textContent)).toEqual([
      'Settings', 'Note compression', 'Summary L1', 'Summary L2', 'Summary L3 section',
      'Summary L3 overview', 'Short session summary', 'Long session summary',
      'Page formatting', 'YouTube transcript formatting',
    ]);
    fireEvent.click(screen.getByRole('button', { name: detail.display_name }));
    expect(navigate).toHaveBeenCalledWith({
      type: 'inspect-prompt', promptId: detail.id, promptName: detail.display_name,
    });
  });

  it('shows literal text, cancels an edit, and saves an empty value', async () => {
    const updatePrompt = vi.fn(async () => ({ ...detail, text: '' }));
    const { container } = render(<PromptScreen client={fixtureClient({
      getPrompt: async () => detail, updatePrompt,
    })} navigate={vi.fn()} state={state} />);
    await screen.findByRole('button', { name: 'Edit text' });
    expect(container.querySelector('.cha-prompt-text')?.textContent).toBe(detail.text);
    expect(container.querySelector('script')).toBeNull();
    expect(screen.queryByRole('button', { name: /rename|delete/i })).toBeNull();

    edit();
    expect(field()).toHaveFocus();
    expect(field()).not.toHaveAttribute('placeholder');
    fireEvent.change(field(), { target: { value: 'Discard this' } });
    cancel();
    expect(updatePrompt).not.toHaveBeenCalled();
    expect(container.querySelector('.cha-prompt-text')?.textContent).toBe(detail.text);

    edit();
    fireEvent.click(screen.getByRole('button', { name: 'Clear' }));
    fireEvent.click(save());
    await waitFor(() => expect(screen.queryByRole('textbox')).toBeNull());
    expect(updatePrompt).toHaveBeenCalledExactlyOnceWith(detail.id, { text: '' });
    expect(container.querySelector('.cha-prompt-text')?.textContent).toBe('');
  });

  it('validates uploads and lets the same file be reviewed again after Cancel', async () => {
    const updatePrompt = vi.fn(async () => ({ ...detail, text: 'Uploaded' }));
    const { container } = render(<PromptScreen client={fixtureClient({
      getPrompt: async () => detail, updatePrompt,
    })} navigate={vi.fn()} state={state} />);
    await screen.findByRole('button', { name: 'Upload text' });
    const input = container.querySelector('input[type="file"]')!;
    fireEvent.change(input, { target: { files: [] } });
    expect(screen.queryByRole('textbox')).toBeNull();
    fireEvent.change(input, { target: { files: [new File(['Wrong'], 'wrong.html', { type: 'text/plain' })] } });
    expect(await screen.findByRole('alert')).toHaveTextContent('Choose a .txt or .md file.');
    expect(container.querySelector('.cha-prompt-text')?.textContent).toBe(detail.text);
    const file = new File(['Uploaded\r\n# Text'], 'CUSTOM.MD');
    for (let attempt = 0; attempt < 2; attempt += 1) {
      fireEvent.change(input, { target: { files: [file] } });
      await screen.findByRole('textbox');
      expect(field()).toHaveValue('Uploaded\n# Text');
      expect(updatePrompt).not.toHaveBeenCalled();
      if (attempt === 0) cancel();
    }
    fireEvent.change(field(), { target: { value: 'Edited upload' } });
    fireEvent.click(save());
    await waitFor(() => expect(screen.queryByRole('textbox')).toBeNull());
    expect(updatePrompt).toHaveBeenCalledExactlyOnceWith(detail.id, { text: 'Edited upload' });
    expect(container.querySelector('.cha-prompt-text')?.textContent).toBe('Uploaded');
  });

  it('blocks duplicate saves and retains a failed draft', async () => {
    let fail!: (reason: Error) => void;
    const updatePrompt = vi.fn(() => new Promise<PromptDetail>((_resolve, reject) => { fail = reject; }));
    render(<PromptScreen client={fixtureClient({
      getPrompt: async () => detail, updatePrompt,
    })} navigate={vi.fn()} state={state} />);
    await screen.findByRole('button', { name: 'Edit text' });
    edit();
    fireEvent.change(field(), { target: { value: 'Keep this draft' } });
    fireEvent.click(save());
    const pending = screen.getByRole('button', { name: 'Saving…' });
    expect(pending).toBeDisabled();
    fireEvent.submit(pending.closest('form')!);
    expect(updatePrompt).toHaveBeenCalledOnce();
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeDisabled();
    await act(async () => fail(new ChaError('invalid_argument', 'Check the text and try again.')));
    expect(screen.getByRole('alert')).toHaveTextContent('Check the text and try again.');
    expect(field()).toHaveValue('Keep this draft');
    expect(save()).toBeEnabled();
  });

  it('discards drafts and ignores a late save after the vault changes', async () => {
    let epoch = 1;
    let finish!: (value: PromptDetail) => void;
    const updatePrompt = vi.fn(() => new Promise<PromptDetail>((resolve) => { finish = resolve; }));
    const client = fixtureClient({
      contextEpoch: () => epoch,
      getPrompt: async () => epoch === 1 ? detail : { ...detail, text: 'Another vault' },
      updatePrompt,
    });
    const navigate = vi.fn();
    const view = () => <PromptScreen key={epoch} client={client} navigate={navigate} state={state} />;
    const { rerender } = render(view());
    await screen.findByRole('button', { name: 'Edit text' });
    edit();
    fireEvent.change(field(), { target: { value: 'Old vault draft' } });
    fireEvent.click(save());
    epoch = 2;
    rerender(view());
    expect(await screen.findByText('Another vault')).toBeVisible();
    await act(async () => finish({ ...detail, text: 'Old vault draft' }));
    expect(screen.queryByRole('textbox')).toBeNull();
    expect(screen.queryByText('Old vault draft')).toBeNull();
    expect(updatePrompt).toHaveBeenCalledOnce();
  });

  it('updates a clean editor and requires cancelling a dirty draft after a changed refresh', async () => {
    const getPrompt = vi.fn().mockResolvedValueOnce(detail)
      .mockResolvedValueOnce({ ...detail, text: 'Refreshed' })
      .mockResolvedValueOnce({ ...detail, text: 'Changed again' });
    const client = fixtureClient({ getPrompt });
    const navigate = vi.fn();
    const view = (epoch: number) => (
      <DetailRefreshProvider value={{ epoch, refreshing: false, failed: false, stale: false, retry() {} }}>
        <PromptScreen client={client} navigate={navigate} state={state} />
      </DetailRefreshProvider>
    );
    const { rerender } = render(view(0));
    await screen.findByRole('button', { name: 'Edit text' });
    edit();
    rerender(view(1));
    await waitFor(() => expect(field()).toHaveValue('Refreshed'));
    fireEvent.change(field(), { target: { value: 'My draft' } });
    rerender(view(2));
    expect(await screen.findByRole('alert')).toHaveTextContent('This prompt changed.');
    expect(field()).toHaveValue('My draft');
    expect(save()).toBeDisabled();
    cancel();
    edit();
    expect(field()).toHaveValue('Changed again');
  });
});
