import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { expect, it, vi } from 'vitest';

import type { ChaClient, ForumSummary } from '../api/client';
import { DetailRefreshProvider } from '../detailRefresh';
import { initialAppState, type AppState } from '../state/view';
import {
  bootstrapFixture, fixtureClient, forumDetailFixture, personaDetailFixture,
} from '../test/fixtures';
import { ForumMembersScreen, PersonaDetailScreen } from './Screens';

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
      <PersonaDetailScreen client={client} dispatch={dispatch} state={state} />
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
  const view = render(<ForumMembersScreen client={client} dispatch={dispatch} state={membersState(lobby)} />);
  const updated = field === 'members'
    ? { ...lobby, members: bootstrapFixture.characters }
    : { ...lobby, default_persona_id: 'guest', default_persona_display_name: 'Guest' };
  view.rerender(<ForumMembersScreen client={client} dispatch={dispatch} state={membersState(updated)} />);

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
    <ForumMembersScreen client={client} dispatch={dispatch} state={membersState(forum)} />
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
  const view = render(<ForumMembersScreen client={client} dispatch={dispatch} state={membersState(lobby)} />);
  await user.click(screen.getByRole('checkbox', { name: 'Assistant' }));
  await user.click(screen.getByRole('button', { name: 'Save' }));
  await waitFor(() => expect(dispatch).toHaveBeenCalledWith({ type: 'forum-updated', forum: saved }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();

  const updated = { ...saved, default_persona_id: 'guest', default_persona_display_name: 'Guest' };
  view.rerender(<ForumMembersScreen client={client} dispatch={dispatch} state={membersState(updated)} />);
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('checkbox', { name: 'Assistant' })).toBeChecked();
  expect(screen.getByLabelText('Persona')).toHaveValue('guest');
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});
