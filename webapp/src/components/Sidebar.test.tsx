import { cleanup, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { appReducer, initialAppState, type AppAction, type AppState, type MainView } from '../state/view';
import { bootstrapFixture } from '../test/fixtures';
import { Sidebar } from './Sidebar';

function readyState() {
  return appReducer(initialAppState, {
    type: 'bootstrap-loaded',
    bootstrap: bootstrapFixture,
  });
}

function renderSidebar(state: AppState = readyState()) {
  const dispatch = vi.fn<(action: AppAction) => void>();
  const onOpenSession = vi.fn(async () => true);
  render(
    <Sidebar
      dispatch={dispatch}
      onClearSessionAudioCache={vi.fn(async () => undefined)}
      onDeleteSession={vi.fn(async () => undefined)}
      onDownloadSession={vi.fn(async () => undefined)}
      onOpenSession={onOpenSession}
      onRenameSession={vi.fn(async () => undefined)}
      onSwitchVault={vi.fn(async () => undefined)}
      state={state}
    />,
  );
  return { dispatch, onOpenSession };
}

describe('Sidebar navigation', () => {
  it.each(['loading', 'failed', 'incompatible'] as const)(
    'does not report empty lists when bootstrap is %s', (bootstrapStatus) => {
      renderSidebar({ ...initialAppState, bootstrapStatus });
      expect(screen.queryByText('No recent forums')).not.toBeInTheDocument();
      expect(screen.queryByText('No recent sessions')).not.toBeInTheDocument();
      expect(within(screen.getByRole('navigation', { name: 'Recent forums' }))
        .queryAllByRole('button')).toHaveLength(0);
      expect(within(screen.getByLabelText('Recent sessions'))
        .queryAllByRole('button')).toHaveLength(0);
    },
  );

  it('reports empty lists after bootstrap loads with no recent sessions', () => {
    renderSidebar({
      ...readyState(),
      bootstrap: { ...bootstrapFixture, recent_sessions: [] },
    });
    expect(screen.getByText('No recent forums')).toBeInTheDocument();
    expect(screen.getByText('No recent sessions')).toBeInTheDocument();
  });

  it('replaces configuration links with recent forums above recent sessions', async () => {
    const { dispatch } = renderSidebar();
    for (const name of ['Personas', 'Characters', 'Forums']) {
      expect(screen.queryByRole('button', { name })).not.toBeInTheDocument();
    }
    const forums = screen.getByRole('navigation', { name: 'Recent forums' });
    const sessions = screen.getByLabelText('Recent sessions');
    expect(forums.compareDocumentPosition(sessions) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(screen.getByText('Recent Forums')).toBeInTheDocument();
    expect(screen.getByText('Recent Sessions')).toBeInTheDocument();
    expect(screen.getByLabelText('Vault')).toHaveValue('Personal');
    await userEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings' });
  });

  it('excludes Entrance and deduplicates other forums in first session occurrence order', async () => {
    const [entrance, lobby] = bootstrapFixture.forums;
    const [welcome, planning] = bootstrapFixture.recent_sessions;
    const state = {
      ...readyState(),
      bootstrap: {
        ...bootstrapFixture,
        forums: [
          entrance,
          { ...lobby, id: 'studio', display_name: 'Studio' },
          lobby,
          { ...lobby, id: 'unused', display_name: 'Unused' },
        ],
        recent_sessions: [
          welcome,
          { ...planning, forum_id: 'missing', session_id: 'orphan' },
          planning,
          { ...planning, session_id: 'second', session_label: 'Second' },
          { ...planning, forum_id: 'studio', session_id: 'draft', session_label: 'Draft' },
          { ...welcome, session_id: 'third', session_label: 'Third' },
        ],
      },
    };
    const { dispatch } = renderSidebar(state);
    const forums = within(screen.getByRole('navigation', { name: 'Recent forums' }));
    expect(forums.getAllByRole('button').map((button) => button.textContent)).toEqual([
      'The Lobby', 'Studio',
    ]);
    await userEvent.click(forums.getByRole('button', { name: 'The Lobby' }));
    expect(dispatch).toHaveBeenCalledExactlyOnceWith({ type: 'select-forum', forumId: 'lobby' });
    expect(appReducer(state, dispatch.mock.calls[0][0])).toMatchObject({
      mainView: 'sessions', currentForumId: 'lobby',
    });
  });

  it.each([
    ['empty sessions', { ...readyState(), bootstrap: { ...bootstrapFixture, recent_sessions: [] } }],
    ['only Entrance sessions', {
      ...readyState(),
      bootstrap: { ...bootstrapFixture, recent_sessions: [bootstrapFixture.recent_sessions[0]] },
    }],
    ['unresolved forums', { ...readyState(), bootstrap: { ...bootstrapFixture, forums: [] } }],
  ] satisfies [string, AppState][])('handles %s without showing forum links', (_label, state) => {
    renderSidebar(state);
    const forums = within(screen.getByRole('navigation', { name: 'Recent forums' }));
    expect(forums.queryAllByRole('button')).toHaveLength(0);
    expect(forums.getByText('No recent forums')).toBeInTheDocument();
  });

  it('preserves recent session order, selection, and opening behavior', async () => {
    const state = {
      ...readyState(),
      activeConversation: { forumId: 'lobby', sessionId: 'planning' },
    };
    const { onOpenSession } = renderSidebar(state);
    const sessions = within(screen.getByLabelText('Recent sessions'));
    const buttons = sessions.getAllByRole('button').filter((button) => !button.hasAttribute('aria-haspopup'));
    expect(buttons.map((button) => button.textContent)).toEqual(['WelcomeEntrance', 'PlanningThe Lobby']);
    expect(buttons[1]).toHaveAttribute('aria-current', 'page');
    await userEvent.click(buttons[1]);
    expect(onOpenSession).toHaveBeenCalledExactlyOnceWith('lobby', 'planning');
  });

  it.each([
    'personas', 'new-persona', 'persona-detail', 'persona-settings',
    'characters', 'new-character', 'character-detail', 'character-file',
    'new-character-file', 'character-settings', 'forums', 'new-forum',
    'sessions', 'forum-detail', 'forum-file', 'new-forum-file',
    'forum-members', 'new-session', 'settings', 'settings-vaults',
    'settings-provider', 'settings-style', 'settings-voice', 'settings-api-key',
  ] satisfies MainView[])('keeps Settings current on %s', (mainView) => {
    renderSidebar({ ...readyState(), mainView });
    const settings = screen.getByRole('button', { name: 'Settings' });
    expect(settings).toHaveClass('is-current');
    expect(settings).toHaveAttribute('aria-current', 'page');
  });

  it.each([
    'sessions', 'forum-detail', 'forum-members', 'forum-file', 'new-forum-file', 'new-session',
  ] satisfies MainView[])('keeps only the selected recent forum current on %s', (mainView) => {
    const lobby = bootstrapFixture.forums[1];
    renderSidebar({
      ...readyState(),
      mainView,
      currentForumId: 'lobby',
      bootstrap: {
        ...bootstrapFixture,
        forums: [...bootstrapFixture.forums, { ...lobby, id: 'studio', display_name: 'Studio' }],
        recent_sessions: [
          ...bootstrapFixture.recent_sessions,
          { ...bootstrapFixture.recent_sessions[1], forum_id: 'studio' },
        ],
      },
    });
    const forums = within(screen.getByRole('navigation', { name: 'Recent forums' }));
    expect(forums.getByRole('button', { name: 'The Lobby' })).toHaveClass('is-current');
    expect(forums.getByRole('button', { name: 'The Lobby' })).toHaveAttribute('aria-current', 'page');
    expect(forums.getByRole('button', { name: 'Studio' })).not.toHaveClass('is-current');
    expect(forums.getByRole('button', { name: 'Studio' })).not.toHaveAttribute('aria-current');
  });

  it.each([
    'chat', 'forums', 'new-forum', 'settings', 'personas', 'characters',
  ] satisfies MainView[])('does not highlight a recent forum on %s', (mainView) => {
    renderSidebar({ ...readyState(), mainView, currentForumId: 'lobby' });
    const forum = within(screen.getByRole('navigation', { name: 'Recent forums' }))
      .getByRole('button', { name: 'The Lobby' });
    expect(forum).not.toHaveClass('is-current');
    expect(forum).not.toHaveAttribute('aria-current');
  });

  it('does not mark Settings current in chat', () => {
    renderSidebar({ ...readyState(), mainView: 'chat' });
    const settings = screen.getByRole('button', { name: 'Settings' });
    expect(settings).not.toHaveClass('is-current');
    expect(settings).not.toHaveAttribute('aria-current');
  });
});

describe('Sidebar session actions', () => {
  it('reports a vault switch failure and restores the selector', async () => {
    const user = userEvent.setup();
    const onSwitchVault = vi.fn(async () => {
      throw new ChaError('internal_error', 'Could not switch vault.');
    });
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={onSwitchVault}
        state={readyState()}
      />,
    );

    await user.selectOptions(screen.getByLabelText('Vault'), 'Projects');
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not switch vault.');
    expect(screen.getByLabelText('Vault')).toHaveValue('Personal');
    expect(screen.getByLabelText('Vault')).toBeEnabled();
  });

  it('asks for a password when a protected vault is selected', async () => {
    const user = userEvent.setup();
    const onSwitchVault = vi.fn(async (_name: string, password?: string) => {
      if (!password) {
        throw new ChaError(
          'vault_password_required', 'Password required to open this vault',
        );
      }
    });
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={onSwitchVault}
        state={readyState()}
      />,
    );

    await user.selectOptions(screen.getByLabelText('Vault'), 'Projects');
    expect(await screen.findByRole('heading', { name: 'Open Projects' })).toBeInTheDocument();
    await user.type(screen.getByLabelText('Password'), 'secret');
    await user.click(screen.getByRole('button', { name: 'Open vault' }));
    expect(onSwitchVault).toHaveBeenLastCalledWith('Projects', 'secret');
  });

  it('offers Download, Clear audio cache, Rename, and Delete in order by right-click and ellipsis but not for Welcome', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    expect(screen.queryByLabelText('Actions for Welcome')).not.toBeInTheDocument();
    await user.pointer({ keys: '[MouseRight]', target: screen.getByText('Planning') });
    expect(screen.getAllByRole('menuitem').map(({ textContent }) => textContent)).toEqual([
      'Download', 'Clear audio cache', 'Rename…', 'Delete…',
    ]);
    await user.keyboard('{Escape}');

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
    expect(screen.getByRole('heading', { name: 'Rename session' })).toBeInTheDocument();
    expect(screen.getByLabelText('Session name')).toHaveValue('Planning');
  });

  it('reports public download and audio-cache failures in the sidebar', async () => {
    for (const item of [
      { action: 'Download', message: 'Could not download session.' },
      { action: 'Clear audio cache', message: 'Could not clear audio cache.' },
    ]) {
      const user = userEvent.setup();
      const failingAction = vi.fn(async () => {
        throw new ChaError('internal_error', item.message);
      });
      render(
        <Sidebar
          dispatch={vi.fn()}
          onClearSessionAudioCache={item.action === 'Clear audio cache' ? failingAction : vi.fn(async () => undefined)}
          onDeleteSession={vi.fn(async () => undefined)}
          onDownloadSession={item.action === 'Download' ? failingAction : vi.fn(async () => undefined)}
          onOpenSession={vi.fn(async () => true)}
          onRenameSession={vi.fn(async () => undefined)}
          onSwitchVault={vi.fn(async () => undefined)}
          state={readyState()}
        />,
      );

      await user.click(screen.getByLabelText('Actions for Planning'));
      await user.click(screen.getByRole('menuitem', { name: item.action }));

      expect(failingAction).toHaveBeenCalledWith('lobby', 'planning', ...(item.action === 'Download' ? ['Planning'] : []));
      expect(await screen.findByRole('alert')).toHaveTextContent(item.message);
      cleanup();
    }
  });

  it('confirms deletion before invoking it', async () => {
    const user = userEvent.setup();
    const onDelete = vi.fn(async () => undefined);
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={onDelete}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
    expect(screen.getByText(/removed from CHA and cannot be reopened/)).toBeInTheDocument();
    expect(onDelete).not.toHaveBeenCalled();
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    expect(onDelete).toHaveBeenCalledWith('lobby', 'planning');
  });

  it('tracks expansion, supports menu keys, and restores focus on Escape', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    const actions = screen.getByLabelText('Actions for Planning');
    expect(actions).toHaveAttribute('aria-expanded', 'false');
    await user.click(actions);
    expect(actions).toHaveAttribute('aria-expanded', 'true');
    const download = screen.getByRole('menuitem', { name: 'Download' });
    const clearAudio = screen.getByRole('menuitem', { name: 'Clear audio cache' });
    const remove = screen.getByRole('menuitem', { name: 'Delete…' });
    expect(download).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(clearAudio).toHaveFocus();
    await user.keyboard('{Home}');
    expect(download).toHaveFocus();
    await user.keyboard('{End}');
    expect(remove).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(download).toHaveFocus();
    await user.keyboard('{ArrowUp}');
    expect(remove).toHaveFocus();
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(actions).toHaveAttribute('aria-expanded', 'false');
    expect(actions).toHaveFocus();
  });

  it('dismisses the menu when the reader points elsewhere', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    expect(screen.getByRole('menu')).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Settings' }));
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
  });

  it('restores focus to the Actions button when a dialog is cancelled', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    const actions = screen.getByLabelText('Actions for Planning');
    await user.click(actions);
    await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
    expect(screen.getByLabelText('Session name')).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(actions).toHaveFocus();
  });

  it('keeps an edited name and displays the public rename error', async () => {
    const user = userEvent.setup();
    const onRename = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'Invalid session label.');
    });
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={vi.fn(async () => undefined)}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={onRename}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
    const input = screen.getByLabelText('Session name');
    await user.clear(input);
    await user.type(input, 'Revised name');
    await user.click(screen.getByRole('button', { name: 'Rename' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Invalid session label.');
    expect(input).toHaveValue('Revised name');
    expect(onRename).toHaveBeenCalledWith('lobby', 'planning', 'Revised name');
    expect(screen.getByRole('dialog')).toBeInTheDocument();
  });

  it('disables the destructive action while deletion is pending', async () => {
    const user = userEvent.setup();
    let finish!: () => void;
    const pending = new Promise<void>((resolve) => { finish = resolve; });
    render(
      <Sidebar
        dispatch={vi.fn()}
        onClearSessionAudioCache={vi.fn(async () => undefined)}
        onDeleteSession={() => pending}
        onDownloadSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        onSwitchVault={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    expect(screen.getByRole('button', { name: 'Working…' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeDisabled();
    finish();
  });
});
