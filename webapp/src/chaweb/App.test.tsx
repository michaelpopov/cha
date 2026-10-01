import { act, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, expect, it, vi } from 'vitest';

import type { SessionListing, SessionSnapshot } from '../api/client';
import { bootstrapFixture, plainVoice, snapshotFixture } from '../test/fixtures';
import { App } from './App';
import { ChaWebError, type ChaWebClient } from './client';

const lobbySessions: SessionListing[] = [
  { id: 'older', label: 'Older', live: false, updated_at: 10 },
  { id: 'planning', label: 'Planning', live: true, updated_at: 50 },
  { id: 'builtin-welcome', label: 'Welcome', live: false, updated_at: 99 },
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
      reasoning_text: 'hidden reasoning',
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
    clearAudio: vi.fn(async () => undefined),
    getBootstrap: vi.fn(async () => boot()),
    listSessions: vi.fn(async (forumId: string) => (
      forumId === 'archive'
        ? [{ id: 'stored', label: 'Stored', live: false, updated_at: 3 }]
        : lobbySessions
    )),
    createSession: vi.fn(),
    getSession: vi.fn(async (_forumId: string, sessionId: string) => snapshot(sessionId)),
    submitInput: vi.fn(),
    stopSession: vi.fn(),
    deleteSession: vi.fn(),
    ...overrides,
  };
}

async function showList(api = client()) {
  render(<App client={api} />);
  await screen.findByRole('button', { name: 'Forum' });
  return api;
}

it('shows forums and recent sessions without opening Welcome or creating a session', async () => {
  const user = userEvent.setup();
  const api = await showList();
  const forum = screen.getByRole('button', { name: 'Forum' });
  expect(forum).toHaveTextContent('The Lobby');
  expect(forum).toHaveTextContent('Guide');
  await user.click(forum);
  expect(screen.queryByRole('button', { name: /Entrance/ })).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'The Lobby Guide' })).toHaveAttribute('aria-pressed', 'true');
  expect(screen.getByRole('button', { name: 'Archive Editor · Critic' })).toHaveAttribute('aria-pressed', 'false');
  await user.keyboard('{Escape}');
  const planning = await screen.findByRole('button', { name: /Planning/ });
  const rows = within(screen.getByRole('list', { name: 'Sessions' })).getAllByRole('button');
  expect(rows[0]).toBe(planning);
  expect(rows[1]).toHaveTextContent(/^Older/);
  expect(rows).toHaveLength(2);
  expect(planning).not.toHaveTextContent(/live|generating/i);
  expect(screen.queryByRole('button', { name: /Welcome/ })).not.toBeInTheDocument();
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
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  const planning = await screen.findByRole('button', { name: /Planning/ });
  expect(planning).toHaveAttribute('aria-current', 'true');
  await user.click(planning);
  expect(api.getSession).toHaveBeenCalledTimes(2);

  window.history.pushState(null, '', '/#/forums/lobby/sessions/older');
  window.dispatchEvent(new PopStateEvent('popstate'));
  window.dispatchEvent(new HashChangeEvent('hashchange'));
  expect(api.getSession).toHaveBeenCalledTimes(3);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'older');
});

it('keeps a forum draft, returns to it, and does not create a session', async () => {
  const user = userEvent.setup();
  const api = await showList();
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  const box = await screen.findByRole('textbox', { name: 'Message' });
  expect(window.location.hash).toBe('');
  await user.type(box, 'Local note');
  box.focus();
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  expect(screen.getByRole('button', { name: 'Forum' })).toHaveTextContent('The Lobby');
  expect(box).not.toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Local note');
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.getSession).not.toHaveBeenCalled();
  expect(api.submitInput).not.toHaveBeenCalled();
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
  expect(await screen.findByRole('button', { name: /Stored/ })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: /Planning/ })).not.toBeInTheDocument();
  expect(api.getSession).not.toHaveBeenCalled();
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.listSessions).toHaveBeenCalledWith('archive');
});

it('shows an error for an unknown route and refreshes the session list', async () => {
  window.history.replaceState(null, '', '/#/forums/entrance/sessions/builtin-welcome');
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
  expect(api.listSessions).toHaveBeenCalledWith('lobby');
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
  await user.click(await screen.findByRole('button', { name: /Planning/ }));
  const box = await screen.findByRole('textbox', { name: 'Message' });
  await user.click(screen.getByRole('button', { name: 'Expand editor' }));
  expect(box).toHaveStyle({ height: '306px' });
  await user.type(box, 'Draft');
  expect(box).toHaveValue('Draft');
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.submitInput).not.toHaveBeenCalled();
  expect(api.stopSession).not.toHaveBeenCalled();
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(await screen.findByRole('button', { name: /Planning/ }));
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
