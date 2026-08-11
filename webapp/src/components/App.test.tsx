import { StrictMode } from 'react';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { ChaError, ChaUnavailableError, type Bootstrap } from '../api/client';
import type { SessionEventHandlers } from '../api/events';
import {
  bootstrapFixture,
  fixtureClient,
  personaDetailFixture,
  snapshotFixture,
} from '../test/fixtures';
import { App } from './App';

function lobbySnapshot(sessionId = 'planning', sessionLabel = 'Planning') {
  return {
    ...snapshotFixture,
    forum: bootstrapFixture.forums[1],
    session_id: sessionId,
    session_label: sessionLabel,
    characters: [bootstrapFixture.characters[1]],
    default_character_id: 'guide',
  };
}

function inertSessionEvents() {
  return { close: vi.fn() };
}

// Hands back the handlers so a test can drive the stream the application owns.
function drivableSessionEvents() {
  const handlers: SessionEventHandlers[] = [];
  const connections: { key: string; close: ReturnType<typeof vi.fn> }[] = [];
  return {
    handlers,
    connections,
    connect(forumId: string, sessionId: string, given: SessionEventHandlers) {
      handlers.push(given);
      const connection = { key: `${forumId}/${sessionId}`, close: vi.fn() };
      connections.push(connection);
      return connection;
    },
  };
}

function recordingSessionEvents() {
  const connections: { key: string; close: ReturnType<typeof vi.fn> }[] = [];
  return {
    connections,
    connect(forumId: string, sessionId: string) {
      const connection = { key: `${forumId}/${sessionId}`, close: vi.fn() };
      connections.push(connection);
      return connection;
    },
  };
}

// jsdom's history has no user gesture, so a Back is the entry it would restore
// followed by the event the browser would deliver.
function goBackTo(pathname: string) {
  window.history.replaceState(null, '', pathname);
  window.dispatchEvent(new PopStateEvent('popstate'));
}

function deferred() {
  let settle!: () => void;
  const promise = new Promise<void>((resolve) => { settle = resolve; });
  return { promise, settle };
}

function sessionRow(name: RegExp | string) {
  return within(screen.getByLabelText('Forum sessions navigation')).getByRole('button', { name });
}

// Recent also carries a Planning row, so the stored-session row is reached
// through the sessions list itself.
async function openPlanningFromTheLobby() {
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await screen.findByRole('button', { name: 'New sessionEnter a name to begin' });
  fireEvent.click(sessionRow(/^Planning/));
}

function storedPlanningClient(overrides = {}) {
  return fixtureClient({
    listSessions: async () => [{ id: 'planning', label: 'Planning', live: false, updated_at: 1 }],
    getSessionSnapshot: async (forumId) => (
      forumId === 'lobby' ? lobbySnapshot() : snapshotFixture
    ),
    ...overrides,
  });
}

function renderAt(width: number) {
  Object.defineProperty(window, 'innerWidth', { configurable: true, value: width });
  return render(<App client={fixtureClient()} />);
}

describe.each([
  ['desktop', 1280],
  ['iPhone', 390],
])('App shell at %s width', (_name, width) => {
  it('lets only the two-line control change sidebar visibility', async () => {
    const { container } = renderAt(width);
    const app = container.querySelector('.cha-app');
    expect(app).toHaveAttribute('data-sidebar', 'open');

    fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
    expect(app).toHaveAttribute('data-sidebar', 'open');

    fireEvent.click(screen.getByRole('button', { name: 'Hide sidebar' }));
    expect(app).toHaveAttribute('data-sidebar', 'closed');
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
    expect(app).toHaveAttribute('data-sidebar', 'closed');
  });
});

it('renders bootstrap discovery data and preserves conversation context while navigating', async () => {
  const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
    forum_id: forumId,
    session_id: sessionId,
  }));
  render(
    <App client={fixtureClient({ openSession })} connectSessionEvents={inertSessionEvents} />,
  );

  expect(await screen.findByLabelText('Current chat context')).toHaveTextContent(
    'EntranceFrom: GuestTo: Assistant',
  );
  const recents = screen.getByLabelText('Recent sessions');
  expect(within(recents).getByText('Welcome')).toBeInTheDocument();
  expect(within(recents).getByText('Planning')).toBeInTheDocument();
  expect(within(recents).getByText('The Lobby')).toBeInTheDocument();

  // The startup conversation is active but not yet attached, so Recent opens it.
  fireEvent.click(screen.getByRole('button', { name: 'WelcomeEntrance' }));
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('From: Guest'));
  expect(openSession).toHaveBeenCalledWith('entrance', 'welcome');

  // Attached now, so returning to it is a view change and not a second open.
  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: 'WelcomeEntrance' }));
  await waitFor(() => expect(screen.getByLabelText('Current chat context')).toBeInTheDocument());
  expect(openSession).toHaveBeenCalledTimes(1);
});

it('lists every persona and renders one as read-only Markdown', async () => {
  const getPersona = vi.fn(async () => personaDetailFixture);
  render(<App client={fixtureClient({ getPersona })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));

  // The built-in Guest and the configured personas share the one catalog.
  const personas = within(screen.getByLabelText('Personas navigation'));
  expect(personas.getByRole('button', { name: /Guest/ })).toBeInTheDocument();
  expect(personas.getByText('Thoughtful, curious, and concise')).toBeInTheDocument();

  fireEvent.click(personas.getByRole('button', { name: /Reader/ }));
  expect(await screen.findByRole('heading', { name: 'Reader notes' })).toBeInTheDocument();
  expect(screen.getByText('thoughtful').tagName).toBe('STRONG');
  expect(getPersona).toHaveBeenCalledWith('reader');
  // The topbar names the persona from bootstrap while its description loads.
  expect(screen.getByRole('heading', { name: 'Reader' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Persona detail navigation'))
    .getByRole('button', { name: 'Personas' }));
  expect(screen.getByRole('heading', { name: 'Personas' })).toBeInTheDocument();
});

it('reports a persona with no PERSONA.md rather than an empty screen', async () => {
  const getPersona = vi.fn(async () => ({ ...personaDetailFixture, persona_markdown: '' }));
  render(<App client={fixtureClient({ getPersona })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
  fireEvent.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  expect(await screen.findByText('This persona has no PERSONA.md description.'))
    .toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('retries a failed persona-detail request without exposing implementation details', async () => {
  const getPersona = vi.fn()
    .mockRejectedValueOnce(new ChaUnavailableError())
    .mockResolvedValueOnce(personaDetailFixture);
  render(<App client={fixtureClient({ getPersona })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
  fireEvent.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('application API is unavailable');
  fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
  expect(await screen.findByRole('heading', { name: 'Reader notes' })).toBeInTheDocument();
  expect(getPersona).toHaveBeenCalledTimes(2);
});

it('loads character detail and renders the restricted Markdown presentation', async () => {
  render(<App client={fixtureClient()} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  expect(screen.getByText('A deterministic test character')).toBeInTheDocument();

  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  expect(await screen.findByRole('heading', { name: 'Guide dossier' })).toBeInTheDocument();
  expect(screen.getByText('careful').tagName).toBe('STRONG');
  expect(screen.getByRole('heading', { name: 'Guide' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Characters' }));
  expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
});

it('retries a failed character-detail request without exposing implementation details', async () => {
  const getCharacter = vi.fn()
    .mockRejectedValueOnce(new ChaUnavailableError())
    .mockResolvedValueOnce({
      id: 'guide',
      display_name: 'Guide',
      description: 'A deterministic test character',
      character_markdown: '# Guide dossier',
    });
  render(<App client={fixtureClient({ getCharacter })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('application API is unavailable');
  fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
  expect(await screen.findByRole('heading', { name: 'Guide dossier' })).toBeInTheDocument();
  expect(getCharacter).toHaveBeenCalledTimes(2);
});

it('shows real forums and their plain-text character membership', async () => {
  render(<App client={fixtureClient()} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));

  expect(screen.getByRole('button', { name: 'EntranceAssistant' })).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  expect(screen.getByRole('heading', { name: 'Sessions' })).toBeInTheDocument();
  expect(await screen.findByRole('button', { name: 'New sessionEnter a name to begin' }))
    .toBeInTheDocument();
});

it('says a forum has no sessions rather than showing an empty panel', async () => {
  render(<App client={fixtureClient({ listSessions: async () => [] })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));

  expect(await screen.findByText(/No sessions in this forum yet/)).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'New sessionEnter a name to begin' }))
    .toBeInTheDocument();

  // The built-in forum cannot be given new sessions, so it must explain itself
  // without pointing at an action that is not there.
  fireEvent.click(screen.getByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'EntranceAssistant' }));
  expect(await screen.findByText('This forum has no sessions.')).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: /New session/ })).not.toBeInTheDocument();
});

it('lists sessions with compact time metadata and opens a stored session once', async () => {
  const open = vi.fn(async (forumId: string, sessionId: string) => ({
    forum_id: forumId,
    session_id: sessionId,
  }));
  const client = fixtureClient({
    listSessions: async () => [{
      id: 'planning',
      label: 'Planning',
      live: false,
      updated_at: Math.floor(Date.now() / 1000) - 2 * 60 * 60,
    }],
    openSession: open,
    getSessionSnapshot: async () => lobbySnapshot(),
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  const session = await screen.findByRole('button', { name: 'Planning2h' });
  expect(session).toHaveTextContent('Planning');
  expect(session).toHaveTextContent('2h');

  fireEvent.click(session);
  fireEvent.click(session);
  await waitFor(() => expect(open).toHaveBeenCalledWith('lobby', 'planning'));
  expect(open.mock.calls.filter(([, sessionId]) => sessionId === 'planning')).toHaveLength(1);
  await waitFor(() => expect(window.location.pathname).toBe('/s/lobby/planning/'));
  expect(screen.getByLabelText('Current chat context')).toHaveTextContent('The Lobby');
});

it('trims a required name, creates then opens it, and refreshes Recent', async () => {
  const user = userEvent.setup();
  const refreshed = structuredClone(bootstrapFixture);
  refreshed.recent_sessions = [{
    forum_id: 'lobby',
    session_id: 'created',
    session_label: 'Architecture review',
    updated_at: 3,
  }, ...refreshed.recent_sessions];
  const getBootstrap = vi.fn()
    .mockResolvedValueOnce(bootstrapFixture)
    .mockResolvedValue(refreshed);
  const createSession = vi.fn(async (_forumId: string, label: string) => ({
    id: 'created',
    label,
  }));
  const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
    forum_id: forumId,
    session_id: sessionId,
  }));
  const connect = vi.fn((_forumId: string, _sessionId: string) => inertSessionEvents());
  const client = fixtureClient({
    getBootstrap,
    listSessions: async () => [],
    createSession,
    openSession,
    getSessionSnapshot: async () => lobbySnapshot('created', 'Architecture review'),
  });
  render(<App client={client} connectSessionEvents={connect} />);

  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New sessionEnter a name to begin' }));

  const start = screen.getByRole('button', { name: 'Start session' });
  const name = screen.getByRole('textbox', { name: 'Session name' });
  expect(start).toBeDisabled();
  await user.type(name, '   ');
  expect(start).toBeDisabled();
  await user.type(name, '  Architecture review  ');
  expect(start).toBeEnabled();
  await user.click(start);

  await waitFor(() => expect(createSession).toHaveBeenCalledWith('lobby', 'Architecture review'));
  expect(openSession).toHaveBeenCalledWith('lobby', 'created');
  await waitFor(() => expect(getBootstrap).toHaveBeenCalledTimes(2));
  expect(connect).toHaveBeenCalledWith('lobby', 'created', expect.any(Object));
  expect(connect.mock.calls.filter(([, sessionId]) => sessionId === 'created')).toHaveLength(1);
  expect(window.location.pathname).toBe('/s/lobby/created/');
  expect(screen.getByLabelText('Current chat context')).toHaveTextContent(
    'The LobbyFrom: ReaderTo: Guide',
  );
  expect(screen.getByRole('button', { name: 'Architecture reviewThe Lobby' }))
    .toHaveAttribute('aria-current', 'page');
});

// Cancelling stops the browser from following the new session, but the server
// has already written it, so it has to turn up in the lists rather than vanish.
it('refreshes Recent when a creation lands after the reader cancelled', async () => {
  const user = userEvent.setup();
  let finishCreate: (created: { id: string; label: string }) => void = () => {};
  const createSession = vi.fn(() => new Promise<{ id: string; label: string }>((resolve) => {
    finishCreate = resolve;
  }));
  const getBootstrap = vi.fn().mockResolvedValue(bootstrapFixture);
  const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
    forum_id: forumId,
    session_id: sessionId,
  }));
  const client = fixtureClient({
    getBootstrap,
    listSessions: async () => [],
    createSession,
    openSession,
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New sessionEnter a name to begin' }));
  await user.type(screen.getByRole('textbox', { name: 'Session name' }), 'Architecture review');
  await user.click(screen.getByRole('button', { name: 'Start session' }));
  await waitFor(() => expect(createSession).toHaveBeenCalled());
  const listedBeforeCancel = getBootstrap.mock.calls.length;

  await user.click(screen.getByRole('button', { name: 'Cancel' }));
  await act(async () => {
    finishCreate({ id: 'created', label: 'Architecture review' });
  });

  expect(openSession).not.toHaveBeenCalledWith('lobby', 'created');
  await waitFor(() => expect(getBootstrap.mock.calls.length).toBe(listedBeforeCancel + 1));
});

it('restores a deep link and offers Welcome when the requested session cannot open', async () => {
  window.history.replaceState(null, '', '/s/lobby/planning/');
  const client = fixtureClient({
    openSession: async (forumId, sessionId) => {
      if (sessionId === 'planning') {
        throw new ChaError(409, 'session_busy', 'Planning is already open elsewhere.');
      }
      return { forum_id: forumId, session_id: sessionId };
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('already open elsewhere');
  fireEvent.click(screen.getByRole('button', { name: 'Return to Welcome' }));
  await waitFor(() => expect(screen.getByLabelText('Current chat context')).toHaveTextContent('Entrance'));
  expect(window.location.pathname).toBe('/');
});

it('opens and snapshots a session-shaped deep link before showing Chat', async () => {
  window.history.replaceState(null, '', '/s/lobby/planning/');
  const openSession = vi.fn(async () => ({ forum_id: 'lobby', session_id: 'planning' }));
  const getSessionSnapshot = vi.fn(async () => lobbySnapshot());
  render(
    <App
      client={fixtureClient({ openSession, getSessionSnapshot })}
      connectSessionEvents={inertSessionEvents}
    />,
  );

  await waitFor(() => expect(screen.getByLabelText('Current chat context')).toHaveTextContent(
    'The LobbyFrom: ReaderTo: Guide',
  ));
  expect(openSession).toHaveBeenCalledWith('lobby', 'planning');
  expect(getSessionSnapshot).toHaveBeenCalledWith('lobby', 'planning');
  expect(window.location.pathname).toBe('/s/lobby/planning/');
});

it('keeps a live stream attached through StrictMode effect replay', async () => {
  const events = recordingSessionEvents();
  render(
    <StrictMode>
      <App
        client={fixtureClient()}
        connectSessionEvents={events.connect}
      />
    </StrictMode>,
  );

  await waitFor(() => expect(events.connections.length).toBeGreaterThan(0));
  expect(screen.queryByText('Opening session…')).not.toBeInTheDocument();
  expect(events.connections.at(-1)?.close).not.toHaveBeenCalled();
});

it.each(['session_busy', 'session_stopping', 'session_open_timeout'] as const)(
  'offers Retry when open fails with %s',
  async (code) => {
    window.history.replaceState(null, '', '/s/lobby/planning/');
    render(
      <App
        client={fixtureClient({
          openSession: async () => {
            throw new ChaError(409, code, `Retryable ${code}`);
          },
        })}
        connectSessionEvents={inertSessionEvents}
      />,
    );

    expect(await screen.findByRole('alert')).toHaveTextContent(`Retryable ${code}`);
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  },
);

it('returns to Welcome and drops the stream when the browser goes back to the root', async () => {
  const events = recordingSessionEvents();
  render(<App client={storedPlanningClient()} connectSessionEvents={events.connect} />);
  await openPlanningFromTheLobby();
  await waitFor(() => expect(window.location.pathname).toBe('/s/lobby/planning/'));
  expect(events.connections).toHaveLength(1);

  goBackTo('/');

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(events.connections[0].close).toHaveBeenCalled();
});

it('re-opens the session named by a restored history entry without pushing it again', async () => {
  const openSession = vi.fn(async () => ({ forum_id: 'lobby', session_id: 'planning' }));
  const events = recordingSessionEvents();
  render(
    <App client={storedPlanningClient({ openSession })} connectSessionEvents={events.connect} />,
  );
  await screen.findByLabelText('Current chat context');
  const entries = window.history.length;

  goBackTo('/s/lobby/planning/');

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('The Lobby'));
  expect(openSession).toHaveBeenCalledWith('lobby', 'planning');
  expect(events.connections.filter(({ key }) => key === 'lobby/planning'))
    .toEqual([expect.objectContaining({ key: 'lobby/planning' })]);
  expect(window.location.pathname).toBe('/s/lobby/planning/');
  expect(window.history.length).toBe(entries);
});

it('abandons an open that finishes after the browser has already gone back', async () => {
  const held = deferred();
  const events = recordingSessionEvents();
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    await held.promise;
    return { forum_id: forumId, session_id: sessionId };
  });
  render(
    <App client={storedPlanningClient({ openSession })} connectSessionEvents={events.connect} />,
  );
  await openPlanningFromTheLobby();
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Opening session'));

  goBackTo('/');
  held.settle();

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(window.location.pathname).toBe('/');
  expect(events.connections).toHaveLength(0);
});

it('lets a second navigation supersede an open that is still in flight', async () => {
  const held = deferred();
  const events = recordingSessionEvents();
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    if (sessionId === 'planning') await held.promise;
    return { forum_id: forumId, session_id: sessionId };
  });
  render(
    <App client={storedPlanningClient({ openSession })} connectSessionEvents={events.connect} />,
  );
  await openPlanningFromTheLobby();
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Opening session'));

  // Back to an earlier session while the first open is still waiting.
  goBackTo('/s/entrance/welcome/');
  held.settle();

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(openSession).toHaveBeenCalledWith('entrance', 'welcome');
  expect(events.connections).toEqual([expect.objectContaining({ key: 'entrance/welcome' })]);
  expect(window.location.pathname).toBe('/s/entrance/welcome/');
});

it('leaves the successor stream attached when a superseded open finishes late', async () => {
  const held = deferred();
  const events = recordingSessionEvents();
  let bootstraps = 0;
  const getBootstrap = vi.fn(async () => {
    bootstraps += 1;
    // The first open attaches its stream and then stalls refreshing Recent.
    if (bootstraps === 2) await held.promise;
    return bootstrapFixture;
  });
  render(
    <App client={storedPlanningClient({ getBootstrap })} connectSessionEvents={events.connect} />,
  );
  await openPlanningFromTheLobby();
  await waitFor(() => expect(events.connections).toHaveLength(1));

  goBackTo('/s/entrance/welcome/');
  await waitFor(() => expect(events.connections).toHaveLength(2));
  held.settle();

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(events.connections[0].close).toHaveBeenCalled();
  expect(events.connections[1].close).not.toHaveBeenCalled();
});

it('lets the sidebar navigate during an open, and that open never pulls the user back', async () => {
  const held = deferred();
  const events = recordingSessionEvents();
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    await held.promise;
    return { forum_id: forumId, session_id: sessionId };
  });
  render(
    <App client={storedPlanningClient({ openSession })} connectSessionEvents={events.connect} />,
  );
  await openPlanningFromTheLobby();
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Opening session'));

  const characters = screen.getByRole('button', { name: 'Characters' });
  expect(characters).toBeEnabled();
  fireEvent.click(characters);
  expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();

  held.settle();
  await waitFor(() => expect(openSession).toHaveResolved());

  expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
  expect(screen.queryByLabelText('Current chat context')).not.toBeInTheDocument();
  expect(events.connections).toHaveLength(0);
  expect(window.location.pathname).toBe('/');
});

it('reports a failed create on the New session screen and keeps the typed name', async () => {
  const user = userEvent.setup();
  const client = fixtureClient({
    listSessions: async () => [],
    createSession: async () => {
      throw new ChaError(500, 'internal_error', 'The workspace is read-only.');
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New sessionEnter a name to begin' }));
  await user.type(screen.getByRole('textbox', { name: 'Session name' }), 'Architecture review');
  await user.click(screen.getByRole('button', { name: 'Start session' }));

  expect(await screen.findByRole('alert')).toHaveTextContent('The workspace is read-only.');
  expect(screen.getByRole('textbox', { name: 'Session name' })).toHaveValue('Architecture review');
  expect(screen.getByRole('button', { name: 'Start session' })).toBeEnabled();
});

it('reports a failed open on the sessions list without discarding it', async () => {
  const client = storedPlanningClient({
    openSession: async () => {
      throw new ChaError(409, 'session_busy', 'Planning is already open elsewhere.');
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);
  await openPlanningFromTheLobby();

  expect(await screen.findByRole('alert')).toHaveTextContent('already open elsewhere');
  expect(sessionRow(/^Planning/)).toBeEnabled();
  expect(window.location.pathname).toBe('/');
});

// Recent is reachable from every screen, so the screen the user happens to be
// looking at is where the failure has to appear. Reporting it only on the two
// screens that start an open themselves loses it silently.
it('reports a Recent open failure on whichever navigation screen is showing', async () => {
  const client = fixtureClient({
    openSession: async (forumId, sessionId) => {
      if (sessionId === 'planning') throw new ChaError(409, 'session_busy', 'Planning is busy.');
      return { forum_id: forumId, session_id: sessionId };
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);
  await screen.findByLabelText('Current chat context');

  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  const recent = within(screen.getByLabelText('Recent sessions'));
  fireEvent.click(recent.getByRole('button', { name: /^Planning/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('Planning is busy.');
  expect(screen.getByLabelText('Characters navigation')).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Return to Welcome' })).toBeInTheDocument();
});

it('offers New session for a stored forum but not for the built-in one', async () => {
  render(<App client={fixtureClient()} connectSessionEvents={inertSessionEvents} />);

  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  expect(await screen.findByRole('button', { name: 'New sessionEnter a name to begin' }))
    .toBeInTheDocument();

  fireEvent.click(screen.getByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'EntranceAssistant' }));
  await waitFor(() => expect(screen.getByRole('heading', { name: 'Sessions' })).toBeInTheDocument());
  expect(screen.queryByRole('button', { name: /New session/ })).not.toBeInTheDocument();
});

it('probes and reconnects when its stream fails', async () => {
  const events = drivableSessionEvents();
  render(
    <App
      client={storedPlanningClient()}
      connectSessionEvents={events.connect}
      retryDelays={[0]}
    />,
  );
  await openPlanningFromTheLobby();
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('The Lobby'));
  const planning = events.connections.findIndex(({ key }) => key === 'lobby/planning');
  expect(planning).toBeGreaterThanOrEqual(0);

  act(() => events.handlers[planning].onError({ kind: 'stream_failure' }));

  await waitFor(() => expect(screen.getByRole('status'))
    .toHaveTextContent('Reconnecting live updates'));
  await waitFor(() => expect(
    events.connections.filter(({ key }) => key === 'lobby/planning'),
  ).toHaveLength(2));
  expect(screen.getByLabelText('Current chat context')).toHaveTextContent('The Lobby');
});

it('names the open session in the transcript placeholder', async () => {
  render(<App client={storedPlanningClient()} connectSessionEvents={inertSessionEvents} />);
  const chat = await screen.findByLabelText('Chat area');
  expect(within(chat).getByText('Welcome')).toBeInTheDocument();

  await openPlanningFromTheLobby();
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('The Lobby'));
  expect(within(screen.getByLabelText('Chat area')).getByText('Planning')).toBeInTheDocument();
});

it('shows a clear incompatible-response state instead of a blank screen', async () => {
  const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
  const client = fixtureClient({
    getBootstrap: async () => ({ characters: [] } as unknown as Bootstrap),
  });
  render(<App client={client} />);

  expect(await screen.findByRole('heading', { name: 'Incompatible application response' }))
    .toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('matching browser files');
  expect(screen.getByRole('alert')).not.toHaveTextContent('initial_forum_id');
  expect(consoleError).toHaveBeenCalledWith(
    'CHA bootstrap validation failed.',
    expect.objectContaining({ message: 'Bootstrap is missing initial_forum_id.' }),
  );
  consoleError.mockRestore();
});

it('names an unavailable API, hides arbitrary exception details, and retries startup', async () => {
  const getBootstrap = vi.fn()
    .mockRejectedValueOnce(new Error('read /private/customer/.env: OPENAI_API_KEY=secret'))
    .mockResolvedValueOnce(bootstrapFixture);
  render(<App client={fixtureClient({ getBootstrap })} connectSessionEvents={inertSessionEvents} />);

  const alert = await screen.findByRole('alert');
  expect(alert).toHaveTextContent('Application API unavailable');
  expect(alert).not.toHaveTextContent('/private/customer');
  expect(alert).not.toHaveTextContent('secret');

  fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
  expect(await screen.findByLabelText('Current chat context')).toHaveTextContent('Entrance');
  expect(getBootstrap).toHaveBeenCalledTimes(2);
});

it('contains the amended chat controls and no Settings entry point', async () => {
  renderAt(1280);
  expect(await screen.findByRole('combobox', { name: 'Choose target character' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
  expect(screen.queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Personas' })).toBeInTheDocument();
});
