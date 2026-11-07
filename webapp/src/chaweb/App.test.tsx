import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, expect, it, vi } from 'vitest';

import type { NoteItem, SessionListing, SessionSnapshot } from '../api/client';
import { bootstrapFixture, plainVoice, snapshotFixture } from '../test/fixtures';
import { App } from './App';
import { ChaWebError, type ChaWebClient } from './client';
import { noteDocument, noteItem, paragraph } from '../test/notes';
import { noteHash, notesHash } from './route';

const lobbySessions: SessionListing[] = [
  { id: 'older', label: 'Older', live: false, updated_at: 10 },
  { id: 'planning', label: 'Planning', live: true, updated_at: 50 },
];

const archiveForum = {
  ...bootstrapFixture.forums[1],
  id: 'archive',
  display_name: 'Archive',
  members: [
    { ...bootstrapFixture.characters[0]!, id: 'editor', display_name: 'Editor' },
    { ...bootstrapFixture.characters[1]!, id: 'critic', display_name: 'Critic' },
  ],
};

function boot() {
  return {
    ...bootstrapFixture,
    forums: [...bootstrapFixture.forums, archiveForum],
  };
}

function snapshot(sessionId: string, extras: Partial<SessionSnapshot> = {}): SessionSnapshot {
  return {
    ...snapshotFixture,
    forum: bootstrapFixture.forums[1],
    session_id: sessionId,
    session_label: sessionId,
    characters: [{
      id: 'guide',
      display_name: 'Guide today',
      appearance: plainVoice,
    }],
    default_character_id: 'guide',
    transcript: [
      {
        id: 1,
        kind: 'human',
        participant_id: 'reader',
        display_name: 'Reader',
        addressed_to: '',
        addressed_to_name: '',
        text: 'First note',
        status: 'complete',
        created_at: 1_700_000_000,
      },
      {
        id: 2,
        kind: 'character',
        participant_id: 'retired',
        display_name: 'Retired',
        addressed_to: '',
        addressed_to_name: '',
        text: '**Covered** reply',
        status: 'complete',
        created_at: 1_700_000_100,
      },
    ],
    covered_until: 1,
    generation: {
      ...snapshotFixture.generation,
    },
    ...extras,
  };
}

afterEach(() => {
  vi.useRealTimers();
  Reflect.deleteProperty(window, 'visualViewport');
});

function client(overrides: Partial<ChaWebClient> = {}): ChaWebClient {
  return {
    listNoteItems: vi.fn(async () => []),
    getNote: vi.fn(),
    getVoiceInputRuntime: vi.fn(async () => null),
    connectVoiceInput: vi.fn(),
    startXaiVoiceInput: vi.fn(),
    sendXaiVoiceAudio: vi.fn(),
    stopXaiVoiceInput: vi.fn(),
    cancelXaiVoiceInput: vi.fn(),
    getVoiceOutputRuntime: vi.fn(async () => null),
    startAudio: vi.fn(),
    startAudioBatch: vi.fn(),
    getAudioStatus: vi.fn(async () => ({ cached_entry_ids: [], downloads: [] })),
    getBootstrap: vi.fn(async () => boot()),
    listSessions: vi.fn(async (forumId: string) => (
      forumId === 'entrance'
        ? [{ id: 'builtin-welcome', label: 'Welcome', live: false, updated_at: 99 }]
        : forumId === 'archive'
        ? [{ id: 'stored', label: 'Stored', live: false, updated_at: 3 }]
        : lobbySessions
    )),
    createSession: vi.fn(),
    getSession: vi.fn(async (forumId: string, sessionId: string) => (
      forumId === 'entrance' ? welcomeSnapshot('idle') : snapshot(sessionId)
    )),
    submitInput: vi.fn(),
    stopSession: vi.fn(),
    deleteSession: vi.fn(),
    ...overrides,
  };
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((yes) => { resolve = yes; });
  return { promise, resolve };
}

function notesClient(overrides: Partial<ChaWebClient> = {}) {
  const items = [noteItem({ id: 10, kind: 'folder', name: 'Reading' }),
    noteItem({ id: 1, parent_id: 10, name: 'First article' }),
    noteItem({ id: 2, parent_id: 10, name: 'Second article' }),
    noteItem({ id: 3, name: 'Top-level article' })];
  return client({
    listNoteItems: vi.fn(async () => items),
    getNote: vi.fn(async (id) => ({ item: items.find((item) => item.id === id)!,
      document: noteDocument(paragraph(`Complete prose for article ${id}`)) })),
    ...overrides,
  });
}

it('browses flat folders and real note links with Back/Forward and no duplicate root entries', async () => {
  const api = notesClient();
  await showList(api);
  const user = userEvent.setup();
  const originalLength = window.history.length;
  await user.click(screen.getByRole('link', { name: 'Notes' }));
  await screen.findByRole('button', { name: 'Folder' });
  await waitFor(() => expect(window.location.hash).toBe(notesHash('root')));
  expect(window.history.length).toBe(originalLength + 1);
  expect(api.listNoteItems).toHaveBeenCalledTimes(1);
  await user.click(screen.getByRole('link', { name: 'Notes' }));
  expect(window.history.length).toBe(originalLength + 1);
  await user.click(screen.getByRole('button', { name: 'Folder' }));
  await user.click(screen.getByRole('button', { name: 'Reading' }));
  expect(window.location.hash).toBe(notesHash(10));
  expect(screen.getByRole('button', { name: 'Folder' })).toHaveAccessibleDescription('Reading');
  expect(screen.getByRole('link', { name: 'First article' })).toHaveAttribute('href', noteHash(1));
  expect(screen.queryByRole('link', { name: 'Top-level article' })).not.toBeInTheDocument();
  await user.click(screen.getByRole('link', { name: 'First article' }));
  expect(await screen.findByRole('article')).toHaveAccessibleName('First article');
  expect(screen.queryByRole('list', { name: 'Notes' })).not.toBeInTheDocument();
  expect(document.title).toBe('First article — CHA');
  expect(document.documentElement).toHaveClass('chaweb-reading');
  expect((document.querySelector('.chaweb-app') as HTMLElement).style.height).toBe('');
  act(() => window.history.back());
  await screen.findByRole('button', { name: 'Folder' });
  expect(window.location.hash).toBe(notesHash(10));
  expect(document.documentElement).not.toHaveClass('chaweb-reading');
  act(() => window.history.forward());
  await screen.findByRole('article');
  expect(api.getNote).toHaveBeenCalledTimes(2);
  await user.click(screen.getByRole('link', { name: 'Notes' }));
  expect(window.location.hash).toBe(notesHash(10));
  expect(screen.getByRole('link', { name: 'Notes' })).toHaveAttribute('aria-current', 'page');
  expect(api.stopSession).not.toHaveBeenCalled();
  expect(api.startAudio).not.toHaveBeenCalled();
});

it('keeps an unsent chat draft when Notes opens and returns through the Chats root', async () => {
  const user = userEvent.setup();
  const api = notesClient();
  await showList(api);
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Keep this draft');
  await user.click(screen.getByRole('link', { name: 'Notes' }));
  await screen.findByRole('button', { name: 'Folder' });
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  expect(screen.getByRole('button', { name: 'Forum' })).toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Keep this draft');
  expect(api.createSession).not.toHaveBeenCalled();
});

it('loads a direct note without metadata or voice settings and returns to its containing folder', async () => {
  window.history.replaceState(null, '', `/${noteHash(1)}`);
  const api = notesClient();
  render(<App client={api} />);
  await screen.findByRole('article');
  expect(api.listNoteItems).not.toHaveBeenCalled();
  expect(api.getVoiceInputRuntime).not.toHaveBeenCalled();
  expect(api.getVoiceOutputRuntime).not.toHaveBeenCalled();
  await userEvent.click(screen.getByRole('link', { name: 'Notes' }));
  await screen.findByRole('button', { name: 'Folder' });
  expect(window.location.hash).toBe(notesHash(10));
  expect(screen.getByRole('button', { name: 'Folder' })).toHaveTextContent('Reading');
});

it('saves list scroll only on application departure and restores it with existing history state', async () => {
  window.history.replaceState({ other: 'kept' }, '', `/${notesHash(10)}`);
  const api = notesClient();
  render(<App client={api} />);
  const list = await screen.findByRole('list', { name: 'Notes' });
  await screen.findByRole('link', { name: 'First article' });
  Object.defineProperties(list, { scrollHeight: { configurable: true, value: 1000 },
    clientHeight: { configurable: true, value: 200 } });
  list.scrollTop = 350;
  const replace = vi.spyOn(window.history, 'replaceState');
  list.dispatchEvent(new Event('scroll'));
  expect(replace).not.toHaveBeenCalled();
  await userEvent.click(screen.getByRole('link', { name: 'First article' }));
  expect(replace).toHaveBeenCalledWith({ other: 'kept', chawebNotesScroll: 350 }, '');
  await screen.findByRole('article');
  // jsdom has no layout; supply the destination list dimensions before restoration.
  const height = vi.spyOn(HTMLElement.prototype, 'scrollHeight', 'get').mockReturnValue(1000);
  const clientHeight = vi.spyOn(HTMLElement.prototype, 'clientHeight', 'get').mockReturnValue(200);
  const refreshed = deferred<NoteItem[]>();
  vi.mocked(api.listNoteItems).mockReturnValueOnce(refreshed.promise);
  act(() => window.history.back());
  await screen.findByRole('link', { name: 'First article' });
  const restored = screen.getByRole('list', { name: 'Notes' });
  expect(restored.scrollTop).toBe(350);
  // The user scrolls before fresh metadata arrives; the refresh keeps that position.
  restored.scrollTop = 120;
  await act(async () => refreshed.resolve([...await vi.mocked(api.listNoteItems).mock.results[0].value]));
  expect(restored.scrollTop).toBe(120);
  replace.mockRestore(); height.mockRestore(); clientHeight.mockRestore();
});

it('clears the old article and title during navigation and ignores late note results', async () => {
  const one = deferred<Awaited<ReturnType<ChaWebClient['getNote']>>>();
  const two = deferred<Awaited<ReturnType<ChaWebClient['getNote']>>>();
  const api = notesClient({ getNote: vi.fn().mockReturnValueOnce(one.promise).mockReturnValueOnce(two.promise) });
  window.history.replaceState(null, '', `/${noteHash(1)}`);
  const title = document.title;
  render(<App client={api} />);
  await waitFor(() => expect(api.getNote).toHaveBeenCalledTimes(1));
  act(() => {
    window.history.pushState(null, '', `/${noteHash(2)}`);
    window.dispatchEvent(new PopStateEvent('popstate'));
  });
  expect(screen.queryByRole('article')).not.toBeInTheDocument();
  await act(async () => two.resolve({ item: noteItem({ id: 2, name: 'Second title' }), document: noteDocument(paragraph('Second prose')) }));
  expect(screen.getByRole('article')).toHaveAccessibleName('Second title');
  await act(async () => one.resolve({ item: noteItem({ id: 1, name: 'First title' }), document: noteDocument(paragraph('First prose')) }));
  expect(document.title).toBe('Second title — CHA');
  expect(screen.queryByText('First prose')).not.toBeInTheDocument();
  await userEvent.click(screen.getByRole('link', { name: 'Chats' }));
  expect(document.title).toBe(title);
});

it('keeps Notes visible when a departed conversation deletion completes', async () => {
  const deletion = deferred<void>();
  const api = notesClient({ deleteSession: vi.fn(() => deletion.promise) });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  const user = userEvent.setup();
  await screen.findByRole('textbox');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  const remove = await screen.findByRole('button', { name: 'Delete session “Planning”' });
  await waitFor(() => expect(remove).toBeEnabled());
  await user.click(remove);
  await user.click(remove);
  await user.click(screen.getByRole('link', { name: 'Notes' }));
  await screen.findByRole('button', { name: 'Folder' });
  await act(async () => deletion.resolve());
  expect(screen.getByRole('button', { name: 'Folder' })).toBeInTheDocument();
  expect(window.location.hash).toBe(notesHash('root'));
});

it('keeps missing and failed note routes in Notes with an explicit Retry', async () => {
  const api = notesClient({ getNote: vi.fn()
    .mockRejectedValueOnce(new ChaWebError(503, 'Temporarily unavailable'))
    .mockRejectedValueOnce(new ChaWebError(404, 'Missing')) });
  window.history.replaceState(null, '', `/${noteHash(1)}`);
  render(<App client={api} />);
  expect(await screen.findByRole('alert')).toHaveTextContent('Temporarily unavailable');
  expect(window.location.hash).toBe(noteHash(1));
  await userEvent.click(screen.getByRole('button', { name: 'Retry' }));
  await waitFor(() => expect(screen.getByRole('alert')).toHaveTextContent('This note is no longer available.'));
  expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
  expect(screen.getByRole('link', { name: 'Notes' })).toHaveAttribute('aria-current', 'page');
});

it('escapes an empty note title and keeps unsupported content out of an article', async () => {
  const name = '<img src=x onerror=alert(1)>';
  const api = notesClient({ getNote: vi.fn(async () => ({ item: noteItem({ name }),
    document: noteDocument({ type: 'paragraph' }) })) });
  window.history.replaceState(null, '', `/${noteHash(1)}`);
  render(<App client={api} />);
  expect(await screen.findByRole('heading', { level: 1 })).toHaveTextContent(name);
  expect(screen.getByText('This note is empty.')).toBeInTheDocument();
  expect(document.querySelector('img,[contenteditable]')).toBeNull();
  cleanup();
  const invalid = notesClient({ getNote: vi.fn(async () => ({ item: noteItem(),
    document: noteDocument({ type: 'unsupported' }) })) });
  render(<App client={invalid} />);
  expect(await screen.findByRole('alert')).toHaveTextContent('This note cannot be displayed.');
  expect(screen.queryByRole('article')).not.toBeInTheDocument();
});

it('keeps the article DOM stable through image failure and unrelated viewport updates', async () => {
  const api = notesClient({ getNote: vi.fn(async () => ({ item: noteItem(), document: noteDocument(
    paragraph('Keep the prose'), { type: 'image', attrs: { src: 'data:image/png;base64,iVBORw0KGgo=', alt: 'Failed pixels' } },
  ) })) });
  window.history.replaceState(null, '', `/${noteHash(1)}`);
  render(<App client={api} />);
  const article = await screen.findByRole('article');
  const prose = screen.getByText('Keep the prose');
  fireEvent.error(screen.getByRole('img', { name: 'Failed pixels' }));
  expect(screen.getByText('Failed pixels')).toBeInTheDocument();
  fireEvent(window, new Event('resize'));
  fireEvent(document, new Event('visibilitychange'));
  expect(screen.getByRole('article')).toBe(article);
  expect(screen.getByText('Keep the prose')).toBe(prose);
  expect(screen.queryByRole('img')).not.toBeInTheDocument();
  expect(api.getNote).toHaveBeenCalledOnce();
});

async function showList(api = client()) {
  render(<App client={api} />);
  await screen.findByRole('button', { name: 'Forum' });
  return api;
}

it('shows Welcome in the forum picker and lists sessions without vault actions', async () => {
  const user = userEvent.setup();
  const api = await showList();
  const forum = screen.getByRole('button', { name: 'Forum' });
  expect(forum).toHaveTextContent('The Lobby');
  expect(forum).toHaveTextContent('Guide');
  await user.click(forum);
  expect(screen.getByRole('button', { name: 'Welcome Assistant' })).toHaveAttribute('aria-pressed', 'false');
  expect(screen.queryByRole('button', { name: /Entrance/ })).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'The Lobby Guide' })).toHaveAttribute('aria-pressed', 'true');
  expect(screen.getByRole('button', { name: 'Archive Editor · Critic' })).toHaveAttribute('aria-pressed', 'false');
  await user.keyboard('{Escape}');
  const planning = await screen.findByRole('button', { name: /^Planning/ });
  const rows = within(screen.getByRole('list', { name: 'Sessions' })).getAllByRole('button').filter((button) => !button.classList.contains('chaweb-session-delete'));
  expect(rows[0]).toBe(planning);
  expect(rows[1]).toHaveTextContent(/^Older/);
  expect(rows).toHaveLength(2);
  expect(planning).not.toHaveTextContent(/live|generating/i);
  expect(screen.queryByRole('button', { name: 'Welcome' })).not.toBeInTheDocument();
  expect(screen.queryByRole('group', { name: 'Vault actions' })).not.toBeInTheDocument();
  expect(api.getSession).not.toHaveBeenCalled();
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.listSessions).toHaveBeenCalledTimes(1);
  expect(api.listSessions).toHaveBeenCalledWith('lobby');
});

it('restores a stored session once and does not focus the editor', async () => {
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  const api = client();
  render(<App client={api} />);
  const box = await screen.findByRole('textbox', { name: 'Message' });
  expect(box).not.toHaveFocus();
  expect(screen.getByText('Retired')).toBeInTheDocument();
  expect(screen.getByText('Covered')).toBeInTheDocument();
  expect(screen.queryByText('hidden reasoning')).not.toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(1);
  expect(api.getSession).toHaveBeenCalledWith('lobby', 'planning');
  expect(api.listSessions).not.toHaveBeenCalled();
  expect(api.createSession).not.toHaveBeenCalled();
});

it('opens the current session again and ignores a duplicate history event', async () => {
  const user = userEvent.setup();
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  const api = client();
  render(<App client={api} />);
  await screen.findByRole('textbox', { name: 'Message' });
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  const planning = await screen.findByRole('button', { name: /^Planning/ });
  expect(planning).toHaveAttribute('aria-current', 'true');
  await user.click(planning);
  expect(api.getSession).toHaveBeenCalledTimes(2);

  window.history.pushState(null, '', '/#/forums/lobby/sessions/older');
  window.dispatchEvent(new PopStateEvent('popstate'));
  window.dispatchEvent(new HashChangeEvent('hashchange'));
  expect(api.getSession).toHaveBeenCalledTimes(3);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'older');
});

it('changes the list when the forum changes and does not open a session', async () => {
  const user = userEvent.setup();
  const api = await showList();
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^Archive / }));
  const forum = screen.getByRole('button', { name: 'Forum' });
  expect(forum).toHaveTextContent('Archive');
  expect(forum).toHaveTextContent('Editor · Critic');
  expect(forum).not.toHaveTextContent('Guide');
  expect(forum).toHaveFocus();
  expect(forum.closest('details')).not.toHaveAttribute('open');
  expect(await screen.findByRole('button', { name: /^Stored/ })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: /^Planning/ })).not.toBeInTheDocument();
  expect(api.getSession).not.toHaveBeenCalled();
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.listSessions).toHaveBeenCalledWith('archive');
});

it('opens Welcome from its direct address', async () => {
  window.history.replaceState(null, '', '/#/forums/entrance/sessions/builtin-welcome');
  const api = client();
  render(<App client={api} />);
  expect(await screen.findByRole('textbox', { name: 'Message' })).toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledWith('entrance', 'builtin-welcome');
  expect(api.createSession).not.toHaveBeenCalled();
});

it('loads Welcome navigation without waiting for an ordinary forum', async () => {
  const user = userEvent.setup();
  window.history.replaceState(null, '', '/#/forums/entrance/sessions/builtin-welcome');
  const ordinary = new Promise<SessionListing[]>(() => {});
  const api = client({
    listSessions: vi.fn((forumId: string) => forumId === 'entrance'
      ? Promise.resolve([{ id: 'builtin-welcome', label: 'Welcome', live: false, updated_at: 99 }])
      : ordinary),
  });
  render(<App client={api} />);
  await screen.findByRole('textbox', { name: 'Message' });
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  expect(await screen.findByRole('button', { name: /^Entrance/ })).toHaveAttribute('aria-current', 'true');
  expect(api.listSessions).toHaveBeenCalledWith('entrance');
});

function entranceForum() {
  return boot().forums[0]!;
}

function welcomeSnapshot(phase: 'idle' | 'active' | 'saved'): SessionSnapshot {
  const generation = {
    ...snapshotFixture.generation,
    active: phase === 'active',
    phase: phase === 'active' ? 'answering' as const : 'waiting' as const,
  };
  const transcript: SessionSnapshot['transcript'] = phase === 'idle' ? [] : [
    {
      id: 1,
      kind: 'character',
      participant_id: 'assistant',
      display_name: 'Assistant',
      addressed_to: '',
      addressed_to_name: '',
      text: 'Repaired the name.',
      status: 'complete',
      created_at: 1_700_000_100,
    },
    ...(phase === 'saved' ? [{
      id: 2,
      kind: 'notice' as const,
      participant_id: '',
      display_name: '',
      addressed_to: '',
      addressed_to_name: '',
      text: 'Configuration was saved.',
      status: 'complete' as const,
      created_at: 1_700_000_200,
    }] : []),
  ];
  return snapshot('builtin-welcome', {
    forum: entranceForum(),
    session_label: 'Welcome',
    characters: [{ id: 'assistant', display_name: 'Assistant', appearance: plainVoice }],
    default_character_id: 'assistant',
    transcript,
    generation,
    covered_until: undefined,
  });
}

function renamedBoot() {
  const loaded = boot();
  return {
    ...loaded,
    characters: loaded.characters.map((character) => (
      character.id === 'guide' ? { ...character, display_name: 'Guide renamed' } : character
    )),
    forums: loaded.forums.map((forum) => (
      forum.id === 'lobby' ? {
        ...forum,
        members: forum.members.map((member) => ({ ...member, display_name: 'Guide renamed' })),
      } : forum
    )),
  };
}

it('opens Assistant from Welcome and reloads bootstrap when the answer ends', async () => {
  const user = userEvent.setup();
  let phase: 'idle' | 'active' | 'saved' = 'idle';
  let renamed = false;
  const api = client({
    getBootstrap: vi.fn(async () => (renamed ? renamedBoot() : boot())),
    getSession: vi.fn(async () => welcomeSnapshot(phase)),
    submitInput: vi.fn(async () => { phase = 'active'; }),
  });
  await showList(api);
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: 'Welcome Assistant' }));
  expect(screen.getByRole('button', { name: 'Forum' })).toHaveTextContent('Welcome');
  expect(screen.queryByRole('button', { name: 'New Session' })).not.toBeInTheDocument();
  await user.click(await screen.findByRole('button', { name: /^Entrance/ }));
  await screen.findByRole('textbox', { name: 'Message' });
  expect(api.getSession).toHaveBeenCalledWith('entrance', 'builtin-welcome');
  expect(api.createSession).not.toHaveBeenCalled();
  expect(window.location.hash).toBe('#/forums/entrance/sessions/builtin-welcome');

  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Repair the guide name');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  expect(api.submitInput).toHaveBeenCalledWith('entrance', 'builtin-welcome', 'Repair the guide name');
  expect(await screen.findByText('Repaired the name.')).toBeInTheDocument();

  phase = 'saved';
  renamed = true;
  expect(await screen.findByText('Configuration was saved.', {}, { timeout: 3_000 })).toBeInTheDocument();

  window.history.back();
  expect(await screen.findByRole('button', { name: 'Forum' })).toHaveTextContent('Welcome');
  expect(await screen.findByRole('button', { name: /^Entrance/ })).toHaveAttribute('aria-current', 'true');
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: 'The Lobby Guide renamed' }));
  expect(screen.getByRole('button', { name: 'Forum' })).toHaveTextContent('Guide renamed');
  expect(api.createSession).not.toHaveBeenCalled();

  window.history.forward();
  expect(await screen.findByText('Configuration was saved.')).toBeInTheDocument();
  expect(window.location.hash).toBe('#/forums/entrance/sessions/builtin-welcome');

  cleanup();
  window.history.replaceState(null, '', '/#/forums/entrance/sessions/builtin-welcome');
  render(<App client={api} />);
  expect(await screen.findByText('Configuration was saved.')).toBeInTheDocument();
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.getSession).toHaveBeenCalledWith('entrance', 'builtin-welcome');
});

it('opens Assistant with the keyboard and reloads bootstrap when its answer is already idle', async () => {
  const user = userEvent.setup();
  let phase: 'idle' | 'saved' = 'idle';
  const getBootstrap = vi.fn(async () => boot());
  const api = client({
    getBootstrap,
    getSession: vi.fn(async () => welcomeSnapshot(phase)),
    submitInput: vi.fn(async () => { phase = 'saved'; }),
  });
  await showList(api);
  screen.getByRole('button', { name: 'Forum' }).focus();
  await user.keyboard('{Enter}');
  screen.getByRole('button', { name: 'Welcome Assistant' }).focus();
  await user.keyboard('{Enter}');
  (await screen.findByRole('button', { name: /^Entrance/ })).focus();
  await user.keyboard('{Enter}');
  await screen.findByRole('textbox', { name: 'Message' });
  expect(screen.queryByRole('button', { name: /Delete session/ })).not.toBeInTheDocument();
  const opened = getBootstrap.mock.calls.length;
  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Fix it');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  expect(await screen.findByText('Configuration was saved.')).toBeInTheDocument();
  await waitFor(() => expect(getBootstrap.mock.calls.length).toBeGreaterThan(opened));
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.submitInput).toHaveBeenCalledWith('entrance', 'builtin-welcome', 'Fix it');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  expect(screen.getByRole('button', { name: 'Forum' })).toHaveTextContent('Welcome');
  expect(await screen.findByRole('button', { name: /^Entrance/ })).toHaveAttribute('aria-current', 'true');
  expect(screen.queryByRole('button', { name: 'New Session' })).not.toBeInTheDocument();
});

it('opens Assistant when the vault has no ordinary forums', async () => {
  const user = userEvent.setup();
  const entrance = bootstrapFixture.forums[0]!;
  const api = client({
    getBootstrap: async () => ({
      ...bootstrapFixture,
      forums: [entrance],
      initial_forum_id: 'entrance',
    }),
  });
  render(<App client={api} />);
  expect(await screen.findByRole('button', { name: 'Forum' })).toHaveTextContent('Welcome');
  expect(screen.queryByRole('button', { name: 'New Session' })).not.toBeInTheDocument();
  await user.click(await screen.findByRole('button', { name: /^Entrance/ }));
  expect(await screen.findByRole('textbox', { name: 'Message' })).toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledWith('entrance', 'builtin-welcome');
  expect(api.createSession).not.toHaveBeenCalled();
});

it('shows an error for an unknown route and refreshes the session list', async () => {
  window.history.replaceState(null, '', '/#/nope');
  const api = client();
  render(<App client={api} />);
  expect(await screen.findByRole('alert')).toHaveTextContent('That conversation is not available.');
  expect(api.getSession).not.toHaveBeenCalled();
  await waitFor(() => expect(api.listSessions).toHaveBeenCalledWith('lobby'));
});

it('refreshes navigation when a stored session is missing', async () => {
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/missing');
  const api = client({
    getSession: vi.fn(async () => {
      throw new ChaWebError(404, 'Session not found.', 'not_found');
    }),
  });
  render(<App client={api} />);
  expect(await screen.findByRole('alert')).toHaveTextContent('Session not found.');
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await waitFor(() => expect(api.listSessions).toHaveBeenCalledWith('lobby'));
  expect(screen.getByRole('button', { name: 'Forum' })).toBeInTheDocument();
});

it('keeps the conversation and retries a failed read with a limit', async () => {
  vi.useFakeTimers();
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  const api = client({
    getSession: vi.fn(async () => {
      throw new ChaWebError(502, 'The request failed.');
    }),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent('Reconnecting');
  expect(screen.getByRole('textbox', { name: 'Message' })).not.toHaveFocus();
  await act(() => vi.advanceTimersByTimeAsync(1_000 + 2_000 + 4_000 + 10_000));
  expect(screen.getByRole('alert')).toHaveTextContent('The request failed.');
  expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(5);
  await act(() => vi.advanceTimersByTimeAsync(30_000));
  expect(api.getSession).toHaveBeenCalledTimes(5);
  expect(api.listSessions).not.toHaveBeenCalled();
});

it('keeps the editor mode and does not submit from the keyboard', async () => {
  const user = userEvent.setup();
  Object.defineProperty(window, 'visualViewport', {
    configurable: true,
    value: {
      height: 700,
      offsetTop: 0,
      addEventListener() {},
      removeEventListener() {},
    },
  });
  const api = await showList();
  await user.click(await screen.findByRole('button', { name: /^Planning/ }));
  const box = await screen.findByRole('textbox', { name: 'Message' });
  await user.click(screen.getByRole('button', { name: 'Expand editor' }));
  expect(box).toHaveStyle({ height: '306px' });
  await user.type(box, 'Draft');
  expect(box).toHaveValue('Draft');
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.submitInput).not.toHaveBeenCalled();
  expect(api.stopSession).not.toHaveBeenCalled();
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(await screen.findByRole('button', { name: /^Planning/ }));
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Draft');
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveStyle({ height: '306px' });
});

it('reports a bootstrap failure without the raw exception and retries', async () => {
  vi.useFakeTimers();
  const api = client({
    getBootstrap: vi.fn()
      .mockRejectedValueOnce(new TypeError('socket hang up'))
      .mockRejectedValueOnce(new TypeError('socket hang up'))
      .mockRejectedValueOnce(new TypeError('socket hang up'))
      .mockRejectedValueOnce(new TypeError('socket hang up'))
      .mockRejectedValueOnce(new TypeError('socket hang up'))
      .mockResolvedValueOnce(boot()),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent('Reconnecting');
  expect(screen.queryByText(/socket hang up/)).not.toBeInTheDocument();
  await act(() => vi.advanceTimersByTimeAsync(1_000 + 2_000 + 4_000 + 10_000));
  expect(screen.getByRole('alert')).toHaveTextContent('ChaWeb could not load.');
  await act(async () => {
    screen.getByRole('button', { name: 'Retry' }).click();
    await vi.advanceTimersByTimeAsync(0);
  });
  expect(screen.getByRole('button', { name: 'Forum' })).toHaveTextContent('The Lobby');
});

it('follows the visual viewport', async () => {
  const listeners = new Map<string, Set<() => void>>();
  const viewport = {
    height: 640,
    offsetTop: 12,
    addEventListener(type: string, listener: EventListener) {
      const set = listeners.get(type) ?? new Set<() => void>();
      set.add(listener as () => void);
      listeners.set(type, set);
    },
    removeEventListener(type: string, listener: EventListener) {
      listeners.get(type)?.delete(listener as () => void);
    },
  };
  Object.defineProperty(window, 'visualViewport', { configurable: true, value: viewport });
  await showList();
  const app = document.querySelector('.chaweb-app');
  expect(app).toHaveStyle({ height: '640px', top: '12px' });
  viewport.height = 400;
  viewport.offsetTop = 30;
  act(() => {
    listeners.get('resize')?.forEach((listener) => listener());
  });
  expect(app).toHaveStyle({ height: '400px', top: '30px' });
});
