import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { expect, it, vi } from 'vitest';

import type { ChaClient, ForumSummary } from '../api/client';
import { DetailRefreshProvider } from '../detailRefresh';
import { initialAppState, type AppState } from '../state/view';
import {
  bootstrapFixture, fixtureClient, forumDetailFixture, personaDetailFixture,
} from '../test/fixtures';
import { ForumMembersScreen } from './ForumScreens';
import { PersonaDetailScreen } from './PersonaScreens';

it('refreshes a persona back to its original value after a local save', async () => {
  const user = userEvent.setup();
  const renamed = { ...personaDetailFixture, display_name: 'New local name' };
  const client = fixtureClient({
    getPersona: async () => personaDetailFixture,
    updatePersona: async () => renamed,
  });
  const dispatch = vi.fn();
  const state = {
    ...initialAppState, bootstrap: bootstrapFixture,
    inspectedPersona: { id: personaDetailFixture.id, writable: true },
  };
  const viewFor = (epoch: number) => (
    <DetailRefreshProvider value={{ epoch, refreshing: false, failed: false, stale: false, retry() {} }}>
      <PersonaDetailScreen client={client} dispatch={dispatch} navigate={dispatch} state={state} />
    </DetailRefreshProvider>
  );
  const view = render(viewFor(0));
  const originalName = `Rename ${personaDetailFixture.display_name}`;
  await user.click(await screen.findByRole('button', { name: originalName }));
  fireEvent.change(screen.getByLabelText('Persona name'), { target: { value: renamed.display_name } });
  await user.click(screen.getByRole('button', { name: 'Save persona name' }));
  await screen.findByRole('button', { name: `Rename ${renamed.display_name}` });

  view.rerender(viewFor(1));
  expect(await screen.findByRole('button', { name: originalName })).toBeInTheDocument();
});

const lobby = bootstrapFixture.forums[1];

function membersState(forum: ForumSummary): AppState {
  return {
    ...initialAppState,
    currentForumId: forum.id,
    bootstrap: {
      ...bootstrapFixture,
      forums: bootstrapFixture.forums.map((original) => original.id === forum.id ? forum : original),
    },
  };
}

it.each(['members', 'persona'] as const)('refreshes untouched forum %s without a draft conflict', (field) => {
  const client = fixtureClient();
  const dispatch = vi.fn();
  const view = render(<ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={membersState(lobby)} />);
  const updated = field === 'members'
    ? { ...lobby, members: bootstrapFixture.characters }
    : { ...lobby, default_persona_id: 'guest', default_persona_display_name: 'Guest' };
  view.rerender(<ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={membersState(updated)} />);

  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('checkbox', { name: 'Assistant' }).matches(':checked')).toBe(field === 'members');
  expect(screen.getByLabelText('Persona')).toHaveValue(updated.default_persona_id);
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});

it('keeps a forum draft during a conflict and clears it only when new values are accepted', async () => {
  const user = userEvent.setup();
  const updateForumMembers = vi.fn<ChaClient['updateForumMembers']>();
  const client = fixtureClient({ updateForumMembers });
  const dispatch = vi.fn();
  const viewFor = (forum: ForumSummary) => (
    <ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={membersState(forum)} />
  );
  const view = render(viewFor(lobby));
  await user.click(screen.getByRole('checkbox', { name: 'Assistant' }));
  const changed = { ...lobby, default_persona_id: 'guest', default_persona_display_name: 'Guest' };
  view.rerender(viewFor(changed));

  expect(screen.getByRole('alert')).toHaveTextContent('This item changed. Load the new values to save.');
  expect(screen.getByRole('checkbox', { name: 'Assistant' })).toBeChecked();
  expect(screen.getByLabelText('Persona')).toHaveValue('reader');
  const save = screen.getByRole('button', { name: 'Save' });
  expect(save).toBeDisabled();
  fireEvent.submit(save.closest('form')!);
  expect(updateForumMembers).not.toHaveBeenCalled();

  // An undo of the external change makes the original draft safe again.
  view.rerender(viewFor(lobby));
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('checkbox', { name: 'Assistant' })).toBeChecked();
  expect(save).toBeEnabled();

  view.rerender(viewFor(changed));
  await user.click(screen.getByRole('button', { name: 'Load new values' }));
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('checkbox', { name: 'Assistant' })).not.toBeChecked();
  expect(screen.getByLabelText('Persona')).toHaveValue('guest');
  expect(save).toBeDisabled();
});

it('uses saved forum members as the baseline for the next refresh', async () => {
  const user = userEvent.setup();
  const saved = { ...forumDetailFixture, members: bootstrapFixture.characters };
  const updateForumMembers = vi.fn<ChaClient['updateForumMembers']>(async () => saved);
  const client = fixtureClient({ updateForumMembers });
  const dispatch = vi.fn();
  const view = render(<ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={membersState(lobby)} />);
  await user.click(screen.getByRole('checkbox', { name: 'Assistant' }));
  await user.click(screen.getByRole('button', { name: 'Save' }));
  await waitFor(() => expect(dispatch).toHaveBeenCalledWith({ type: 'forum-updated', forum: saved }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();

  const updated = { ...saved, default_persona_id: 'guest', default_persona_display_name: 'Guest' };
  view.rerender(<ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={membersState(updated)} />);
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('checkbox', { name: 'Assistant' })).toBeChecked();
  expect(screen.getByLabelText('Persona')).toHaveValue('guest');
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});

it('previews unchecked characters with saved forum settings without changing a draft', async () => {
  const user = userEvent.setup();
  const prompt = '<character_instructions>\nFull prompt.\n</character_instructions>';
  const getForumSystemPrompt = vi.fn(async () => ({ system_prompt: prompt }));
  const updateForumMembers = vi.fn<ChaClient['updateForumMembers']>();
  const client = fixtureClient({ getForumSystemPrompt, updateForumMembers });
  const dispatch = vi.fn();
  render(<ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={{ ...membersState(lobby), activeConversation: { forumId: lobby.id, sessionId: 'current-session' } }} />);
  await user.selectOptions(screen.getByLabelText('Persona'), 'guest');
  const trigger = screen.getByRole('button', { name: 'Show system prompt for Assistant' });
  await user.click(trigger);
  const dialog = await screen.findByRole('dialog', { name: 'System prompt for Assistant' });
  expect(await screen.findByLabelText('System prompt')).toHaveTextContent('Full prompt.');
  expect(screen.getByLabelText('System prompt').textContent).toBe(prompt);
  expect(getForumSystemPrompt).toHaveBeenCalledWith(lobby.id, 'assistant');
  expect(screen.getByRole('checkbox', { name: 'Assistant' })).not.toBeChecked();
  expect(screen.getByLabelText('Persona')).toHaveValue('guest');
  expect(updateForumMembers).not.toHaveBeenCalled();
  const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockResolvedValue();
  await user.click(screen.getByRole('button', { name: 'Copy' }));
  expect(writeText).toHaveBeenCalledWith(prompt);
  expect(await screen.findByRole('button', { name: 'Copied' })).toBeEnabled();
  await user.click(screen.getByRole('button', { name: 'Close' }));
  expect(dialog).not.toBeInTheDocument();
  expect(trigger).toHaveFocus();
  await user.click(trigger);
  await screen.findByLabelText('System prompt');
  expect(screen.getByRole('button', { name: 'Copy' })).toBeEnabled();
  expect(screen.queryByRole('button', { name: 'Copied' })).not.toBeInTheDocument();
});

it('shows copy success only after the clipboard write succeeds and clears it on failure', async () => {
  const user = userEvent.setup();
  let finishCopy!: () => void;
  const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockReset()
    .mockImplementationOnce(() => new Promise<void>((resolve) => { finishCopy = resolve; }))
    .mockRejectedValueOnce(new Error('Clipboard unavailable'));
  const dispatch = vi.fn();
  render(<ForumMembersScreen client={fixtureClient()} dispatch={dispatch} navigate={dispatch} state={membersState(lobby)} />);
  await user.click(screen.getByRole('button', { name: 'Show system prompt for Guide' }));
  await screen.findByLabelText('System prompt');
  await user.click(screen.getByRole('button', { name: 'Copy' }));
  expect(writeText).toHaveBeenCalledTimes(1);
  expect(screen.queryByRole('button', { name: 'Copied' })).not.toBeInTheDocument();
  finishCopy();
  await user.click(await screen.findByRole('button', { name: 'Copied' }));
  expect(await screen.findByRole('alert')).toHaveTextContent('The system prompt could not be copied.');
  expect(screen.getByRole('button', { name: 'Copy' })).toBeEnabled();
  expect(screen.queryByRole('button', { name: 'Copied' })).not.toBeInTheDocument();
});

it('keeps a failed system prompt preview closable and reports the error', async () => {
  const user = userEvent.setup();
  const dispatch = vi.fn();
  const client = fixtureClient({ getForumSystemPrompt: async () => { throw new Error('Private error'); } });
  render(<ForumMembersScreen client={client} dispatch={dispatch} navigate={dispatch} state={membersState(lobby)} />);
  await user.click(screen.getByRole('button', { name: 'Show system prompt for Guide' }));
  expect(await screen.findByRole('alert')).toHaveTextContent('The system prompt could not be loaded.');
  expect(screen.getByRole('button', { name: 'Copy' })).toBeDisabled();
  fireEvent(screen.getByRole('dialog'), new Event('cancel', { bubbles: false, cancelable: true }));
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
});
