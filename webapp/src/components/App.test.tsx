import { StrictMode } from 'react';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type Bootstrap,
  type CharacterAppearance,
  type CharacterDetail,
  type SessionSnapshot,
} from '../api/client';
import type { SessionEventHandlers } from '../api/events';
import { createEnvelopeNativeBridge } from '../api/nativeBridge';
import { createNativeSessionEvents } from '../api/nativeEvents';
import type { NativeRequest } from '../api/client';
import {
  bootstrapFixture,
  characterDetailFixture,
  fixtureClient,
  forumDetailFixture,
  monoLargeVoice,
  personaDetailFixture,
  serifItalicVoice,
  snapshotFixture,
  waitingAuth,
} from '../test/fixtures';
import { App } from './App';

beforeEach(() => {
  window.history.replaceState(null, '', '/');
});

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
function goBackTo(route: string) {
  const hash = route === '/' ? '#/' : (route.startsWith('#') ? route : `#${route}`);
  window.history.replaceState(null, '', `/${hash}`);
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

async function openSettingsNavigation() {
  const settings = within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' });
  await waitFor(() => expect(settings).toBeEnabled());
  fireEvent.click(settings);
}

// Recent also carries a Planning row, so the stored-session row is reached
// through the sessions list itself.
async function openPlanningFromTheLobby() {
  await openSettingsNavigation();
  const forums = await screen.findByRole('button', { name: 'Forums' });
  await waitFor(() => expect(forums).toBeEnabled());
  fireEvent.click(forums);
  fireEvent.click(await screen.findByRole('button', { name: 'The LobbyGuide' }));
  await screen.findByRole('button', { name: 'New session' });
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

    await openSettingsNavigation();
    fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
    expect(app).toHaveAttribute('data-sidebar', 'open');

    fireEvent.click(screen.getByRole('button', { name: 'Hide sidebar' }));
    expect(app).toHaveAttribute('data-sidebar', 'closed');
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();

    await openSettingsNavigation();
    fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
    expect(app).toHaveAttribute('data-sidebar', 'closed');
  });
});

it.each(['Personas', 'Characters', 'Forums'])(
  'returns from %s to Settings with the sidebar closed', async (name) => {
    const { container } = renderAt(390);
    await openSettingsNavigation();
    fireEvent.click(screen.getByRole('button', { name }));
    fireEvent.click(screen.getByRole('button', { name: 'Hide sidebar' }));

    const navigation = screen.getByRole('region', { name: `${name} navigation` });
    const back = within(navigation).getByRole('button', { name: 'Settings' });
    expect(within(navigation).getAllByRole('button')[0]).toBe(back);
    expect(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }))
      .toHaveAttribute('aria-current', 'page');
    fireEvent.click(back);

    expect(screen.getByRole('region', { name: 'Configuration' })).toBeInTheDocument();
    expect(container.querySelector('.cha-app')).toHaveAttribute('data-sidebar', 'closed');
  },
);

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
  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: 'WelcomeEntrance' }));
  await waitFor(() => expect(screen.getByLabelText('Current chat context')).toBeInTheDocument());
  expect(openSession).toHaveBeenCalledTimes(1);
});

it('lists every persona and renders its Markdown', async () => {
  const getPersona = vi.fn(async () => personaDetailFixture);
  render(<App client={fixtureClient({ getPersona })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));

  // The built-in Guest and the configured personas share the one catalog.
  const personas = within(screen.getByLabelText('Personas navigation'));
  expect(personas.getByRole('button', { name: /Guest/ })).toBeInTheDocument();
  expect(personas.getByText('Thoughtful, curious, and concise')).toBeInTheDocument();

  fireEvent.click(personas.getByRole('button', { name: /Reader/ }));
  expect(await screen.findByRole('heading', { name: 'Reader notes' })).toBeInTheDocument();
  expect(screen.getByText('thoughtful').tagName).toBe('STRONG');
  expect(getPersona).toHaveBeenCalledWith('reader');
  // The detail screen owns the rename control alongside its loaded description.
  expect(screen.getByRole('button', { name: 'Rename Reader' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Persona detail navigation'))
    .getByRole('button', { name: 'Settings' }));
  expect(screen.getByRole('heading', { name: 'Settings' })).toBeInTheDocument();
  expect(await screen.findByLabelText('Style')).toHaveValue('serif-italic');
  fireEvent.click(screen.getByRole('button', { name: 'Reader' }));
  expect(await screen.findByRole('heading', { name: 'Reader notes' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Persona detail navigation'))
    .getByRole('button', { name: 'Personas' }));
  expect(screen.getByRole('heading', { name: 'Personas' })).toBeInTheDocument();
});

it('creates a named persona and adds it to the roster immediately', async () => {
  const user = userEvent.setup();
  const created = {
    ...personaDetailFixture,
    id: 'persona_1',
    display_name: 'Project manager',
    persona_markdown: '',
  };
  const createPersona = vi.fn(async () => created);
  const getPersona = vi.fn(async (personaId: string) => (
    personaId === created.id ? created : personaDetailFixture
  ));
  render(<App client={fixtureClient({ createPersona, getPersona })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Personas' }));
  await user.click(screen.getByRole('button', {
    name: 'New persona',
  }));

  const create = screen.getByRole('button', { name: 'Create persona' });
  const name = screen.getByRole('textbox', { name: 'Persona name' });
  expect(create).toBeDisabled();
  await user.type(name, '  Project manager  ');
  await user.click(create);

  await waitFor(() => expect(createPersona).toHaveBeenCalledWith({
    display_name: 'Project manager',
  }));
  expect(await screen.findByRole('button', { name: 'Rename Project manager' }))
    .toBeInTheDocument();
  expect(screen.getByText('This persona has no PERSONA.md description.'))
    .toBeInTheDocument();
  await user.click(within(screen.getByLabelText('Persona detail navigation'))
    .getByRole('button', { name: 'Personas' }));
  expect(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Project manager/ })).toBeInTheDocument();
});

it('propagates a persona rename to the roster, forum details, and active chat', async () => {
  const user = userEvent.setup();
  const updatePersona = vi.fn(async (_personaId, update) => ({
    ...personaDetailFixture,
    ...update,
  }));
  render(
    <App
      client={fixtureClient({ updatePersona, getSessionSnapshot: async () => lobbySnapshot() })}
      connectSessionEvents={inertSessionEvents}
    />,
  );
  await user.click(await screen.findByRole('button', { name: 'PlanningThe Lobby' }));
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('From: Reader'));
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
  fireEvent.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  await user.click(await screen.findByRole('button', { name: 'Rename Reader' }));
  const input = screen.getByRole('textbox', { name: 'Persona name' });
  await user.clear(input);
  await user.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
  await user.type(input, 'Redaktor');
  await user.click(screen.getByRole('button', { name: 'Save persona name' }));

  await waitFor(() => expect(updatePersona).toHaveBeenCalledWith(
    'reader', { display_name: 'Редактор' },
  ));
  expect(await screen.findByRole('button', { name: 'Rename Редактор' })).toBeInTheDocument();
  fireEvent.click(within(screen.getByLabelText('Persona detail navigation'))
    .getByRole('button', { name: 'Personas' }));
  expect(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Редактор/ })).toBeInTheDocument();

  await openSettingsNavigation();
  await user.click(screen.getByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));
  expect(await screen.findByText('Guide · speaking as Редактор')).toBeInTheDocument();

  await user.click(screen.getByRole('button', { name: 'PlanningThe Lobby' }));
  expect(await screen.findByLabelText('Current chat context')).toHaveTextContent('From: Редактор');
});

it('replaces persona Markdown from the compact file action', async () => {
  let detail = personaDetailFixture;
  const getPersona = vi.fn(async () => detail);
  const updatePersona = vi.fn(async (_personaId, update) => {
    detail = { ...detail, ...update };
    return detail;
  });
  const { container } = render(<App client={fixtureClient({ getPersona, updatePersona })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
  fireEvent.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  await screen.findByRole('button', { name: 'Replace persona description from file' });
  const file = new File(['# Replacement\n\nFresh text.'], 'persona.md', {
    type: 'text/markdown',
  });
  Object.defineProperty(file, 'text', {
    value: vi.fn(async () => '# Replacement\n\nFresh text.'),
  });
  fireEvent.change(container.querySelector('input[type="file"]') as HTMLInputElement, {
    target: { files: [file] },
  });

  await waitFor(() => expect(updatePersona).toHaveBeenCalledWith('reader', {
    persona_markdown: '# Replacement\n\nFresh text.',
  }));
  expect(await screen.findByRole('heading', { name: 'Replacement' })).toBeInTheDocument();
  expect(screen.getByText('Fresh text.')).toBeInTheDocument();
});

it('edits persona Markdown as pasted text and cancels without saving', async () => {
  const user = userEvent.setup();
  let detail = personaDetailFixture;
  const getPersona = vi.fn(async () => detail);
  const updatePersona = vi.fn(async (_personaId, update) => {
    detail = { ...detail, ...update };
    return detail;
  });
  render(<App client={fixtureClient({ getPersona, updatePersona })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Personas' }));
  await user.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  const edit = await screen.findByRole('button', { name: 'Edit persona profile' });
  await user.click(edit);
  let editor = screen.getByRole('textbox', { name: 'Edit persona profile text' });
  await waitFor(() => expect(editor).toHaveValue(personaDetailFixture.persona_markdown));
  await user.click(screen.getByRole('button', { name: 'Clear' }));
  expect(editor).toHaveValue('');
  expect(screen.getByRole('button', { name: 'Clear' })).toBeDisabled();
  await user.type(editor, '# Discarded');
  await user.click(screen.getByRole('button', { name: 'Cancel' }));
  expect(updatePersona).not.toHaveBeenCalled();
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();

  await user.click(edit);
  editor = screen.getByRole('textbox', { name: 'Edit persona profile text' });
  await waitFor(() => expect(editor).toBeEnabled());
  await user.clear(editor);
  await user.type(editor, '# ');
  await user.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
  await user.type(editor, 'Privet');
  await user.click(screen.getByRole('button', { name: 'Save' }));

  await waitFor(() => expect(updatePersona).toHaveBeenCalledWith('reader', {
    persona_markdown: '# Привет',
  }));
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  expect(await screen.findByRole('heading', { name: 'Привет' })).toBeInTheDocument();
  expect(getPersona).toHaveBeenCalledOnce();
});

it('deletes a persona from the skull action beside upload after confirmation', async () => {
  const user = userEvent.setup();
  const deletePersona = vi.fn(async () => undefined);
  render(<App client={fixtureClient({ deletePersona })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Personas' }));
  await user.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  const upload = await screen.findByRole('button', {
    name: 'Replace persona description from file',
  });
  const remove = screen.getByRole('button', { name: 'Delete Reader' });
  expect(upload.parentElement?.parentElement).toBe(remove.parentElement);
  await user.click(remove);
  expect(screen.getByRole('dialog')).toHaveTextContent(
    'Delete “Reader”? This permanently removes its profile. This cannot be undone.',
  );
  await user.click(screen.getByRole('button', { name: 'Delete persona' }));

  await waitFor(() => expect(deletePersona).toHaveBeenCalledWith('reader'));
  const personas = await screen.findByLabelText('Personas navigation');
  expect(within(personas).queryByRole('button', { name: /Reader/ })).not.toBeInTheDocument();
});

it('reports a persona with no PERSONA.md rather than an empty screen', async () => {
  const getPersona = vi.fn(async () => ({ ...personaDetailFixture, persona_markdown: '' }));
  render(<App client={fixtureClient({ getPersona })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
  fireEvent.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  expect(await screen.findByText('This persona has no PERSONA.md description.'))
    .toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('retries a failed persona-detail request without exposing implementation details', async () => {
  const getPersona = vi.fn()
    .mockRejectedValueOnce(new ChaError('command_timeout', 'The request timed out.'))
    .mockResolvedValueOnce(personaDetailFixture);
  render(<App client={fixtureClient({ getPersona })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
  fireEvent.click(within(screen.getByLabelText('Personas navigation'))
    .getByRole('button', { name: /Reader/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('The request timed out.');
  fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
  expect(await screen.findByRole('heading', { name: 'Reader notes' })).toBeInTheDocument();
  expect(getPersona).toHaveBeenCalledTimes(2);
});

it('opens the file list with settings before displaying a selected character file', async () => {
  const getCharacterFile = vi.fn(async (_id, filename) => ({
    filename, content: characterDetailFixture.editable_markdown, writable: true,
  }));
  render(<App client={fixtureClient({ getCharacterFile })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  const file = await screen.findByRole('button', { name: 'CHARACTER.md' });
  expect(getCharacterFile).not.toHaveBeenCalled();
  expect(screen.queryByRole('heading', { name: 'Guide dossier' })).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Rename Guide' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Settings' })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Edit character file' })).not.toBeInTheDocument();
  fireEvent.click(file);
  expect(await screen.findByRole('heading', { name: 'Guide dossier' })).toBeInTheDocument();
  expect(getCharacterFile).toHaveBeenCalledWith('guide', 'CHARACTER.md');
  expect(screen.getByText('careful').tagName).toBe('STRONG');
  expect(screen.getByRole('heading', { name: 'CHARACTER.md' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Edit character file' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Replace character file content from file' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Delete CHARACTER.md' })).toBeInTheDocument();
  fireEvent.click(within(screen.getByLabelText('Character file navigation'))
    .getByRole('button', { name: 'Guide' }));
  expect(await screen.findByRole('button', { name: 'CHARACTER.md' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Settings' })).toBeInTheDocument();
});

it('loads a character editor from the unexpanded editable source', async () => {
  const user = userEvent.setup();
  const getCharacter = vi.fn(async () => ({
    ...characterDetailFixture,
    character_markdown: '# Expanded Guide',
    editable_markdown: '# Source\n\n$${character.display_name}',
  }));
  render(<App client={fixtureClient({ getCharacter })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', { name: /Guide/ }));
  await user.click(await screen.findByRole('button', { name: 'CHARACTER.md' }));
  await user.click(await screen.findByRole('button', { name: 'Edit character file' }));

  const editor = screen.getByRole('textbox', { name: 'Edit character file text' });
  await waitFor(() => expect(editor).toHaveValue(
    '# Source\n\n$${character.display_name}',
  ));
});

it('saves the selected character file and returns to its list after deleting an optional file', async () => {
  const user = userEvent.setup();
  let content = '# Notes';
  let names = ['CHARACTER.md', 'NOTES.md'];
  const getCharacter = vi.fn(async () => ({ ...characterDetailFixture, markdown_files: names }));
  const getCharacterFile = vi.fn(async (_id, filename) => ({ filename, content, writable: true }));
  const updateCharacterFile = vi.fn(async (_id, filename, value) => {
    content = value;
    return { filename, content, writable: true };
  });
  const deleteCharacterFile = vi.fn(async () => { names = ['CHARACTER.md']; });
  const deleteCharacter = vi.fn();
  render(<App client={fixtureClient({ getCharacter, getCharacterFile,
    updateCharacterFile, deleteCharacterFile, deleteCharacter })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', { name: /Guide/ }));
  await user.click(await screen.findByRole('button', { name: 'NOTES.md' }));
  await user.click(screen.getByRole('button', { name: 'Edit character file' }));
  const editor = await screen.findByRole('textbox', { name: 'Edit character file text' });
  await waitFor(() => expect(editor).toHaveValue('# Notes'));
  await user.clear(editor);
  await user.type(editor, '# Updated notes');
  await user.click(screen.getByRole('button', { name: 'Save' }));
  await waitFor(() => expect(updateCharacterFile).toHaveBeenCalledWith('guide', 'NOTES.md', '# Updated notes'));
  expect(await screen.findByRole('heading', { name: 'Updated notes' })).toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: 'Delete NOTES.md' }));
  expect(screen.getByRole('dialog')).toHaveTextContent('NOTES.md');
  await user.click(screen.getByRole('button', { name: 'Delete file' }));
  expect(await screen.findByRole('button', { name: 'CHARACTER.md' })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'NOTES.md' })).not.toBeInTheDocument();
  expect(deleteCharacterFile).toHaveBeenCalledWith('guide', 'NOTES.md');
  expect(deleteCharacter).not.toHaveBeenCalled();
});

it('adds a character file and opens the saved content', async () => {
  const user = userEvent.setup();
  let content = '';
  const createCharacterFile = vi.fn(async (_id, filename, value) => {
    content = value;
    return { filename, content, writable: true };
  });
  const getCharacterFile = vi.fn(async (_id, filename) => ({ filename, content, writable: true }));
  render(<App client={fixtureClient({ createCharacterFile, getCharacterFile })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', { name: /Guide/ }));
  await user.click(await screen.findByRole('button', { name: 'New file' }));
  await user.type(screen.getByRole('textbox', { name: 'Filename' }), 'NOTES');
  await user.type(screen.getByRole('textbox', { name: 'Content' }), '# New notes');
  await user.click(screen.getByRole('button', { name: 'Add file' }));
  expect(await screen.findByRole('heading', { name: 'New notes' })).toBeInTheDocument();
  expect(createCharacterFile).toHaveBeenCalledWith('guide', 'NOTES.md', '# New notes');
});

it('preserves the basename when adding content from a dotfile', async () => {
  const user = userEvent.setup();
  const createCharacterFile = vi.fn(async (_id, filename, content) => ({ filename, content, writable: true }));
  render(<App client={fixtureClient({ createCharacterFile })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', { name: /Guide/ }));
  await user.click(await screen.findByRole('button', { name: 'New file' }));
  const file = new File(['Notes'], '.gitignore', { type: 'text/plain' });
  Object.defineProperty(file, 'text', { value: async () => 'Notes' });
  fireEvent.change(screen.getByLabelText('Upload content'), { target: { files: [file] } });
  await waitFor(() => expect(screen.getByRole('textbox', { name: 'Filename' })).toHaveValue('.gitignore.md'));
  await user.click(screen.getByRole('button', { name: 'Add file' }));
  expect(createCharacterFile).toHaveBeenCalledWith('guide', '.gitignore.md', 'Notes');
});

it('keeps a character on screen and shows the server message when deletion is refused', async () => {
  const user = userEvent.setup();
  const deleteCharacter = vi.fn(async () => {
    throw new ChaError(
      'invalid_argument',
      'This character is still used by one or more forums.',
    );
  });
  render(<App client={fixtureClient({ deleteCharacter })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', { name: /Guide/ }));
  await user.click(await screen.findByRole('button', { name: 'Delete Guide' }));
  await user.click(screen.getByRole('button', { name: 'Delete character' }));

  expect(await screen.findByRole('alert')).toHaveTextContent(
    'This character is still used by one or more forums.',
  );
  expect(screen.getByRole('button', { name: 'Rename Guide' })).toBeInTheDocument();
});

it('creates a providerless character draft and adds it to the roster immediately', async () => {
  const user = userEvent.setup();
  const created: CharacterDetail = {
    ...characterDetailFixture,
    id: 'character_1',
    display_name: 'Cheburashka',
    description: 'A little furry animal with big ears.',
    character_markdown: '',
    provider: null,
    style: null,
  };
  const createCharacter = vi.fn(async () => created);
  const getCharacter = vi.fn(async (characterId: string) => (
    characterId === created.id ? created : characterDetailFixture
  ));
  render(<App client={fixtureClient({ createCharacter, getCharacter })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', {
    name: 'New character',
  }));

  const create = screen.getByRole('button', { name: 'Create character' });
  expect(screen.getByRole('heading', { name: 'New character' })).toBeInTheDocument();
  expect(create).toBeDisabled();
  await user.type(screen.getByRole('textbox', { name: 'Name' }), '  Cheburashka  ');
  await user.type(
    screen.getByRole('textbox', { name: 'Description' }),
    '  A little furry animal with big ears.  ',
  );
  await user.click(create);

  await waitFor(() => expect(createCharacter).toHaveBeenCalledWith({
    display_name: 'Cheburashka',
    description: 'A little furry animal with big ears.',
  }));
  expect(await screen.findByRole('button', { name: 'Rename Cheburashka' }))
    .toBeInTheDocument();
  expect(await screen.findByRole('button', { name: 'CHARACTER.md' })).toBeInTheDocument();
  await user.click(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Characters' }));
  expect(within(screen.getByLabelText('Characters navigation'))
    .getByRole('button', { name: /Cheburashka/ })).toHaveTextContent(
      'A little furry animal with big ears.',
    );
});

it('renames a writable character in place and updates the roster immediately', async () => {
  const user = userEvent.setup();
  const updateCharacterDefinition = vi.fn(async (_characterId, update) => ({
    ...characterDetailFixture,
    ...update,
  }));
  render(<App client={fixtureClient({ updateCharacterDefinition })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));

  await user.click(await screen.findByRole('button', { name: 'Rename Guide' }));
  const input = screen.getByRole('textbox', { name: 'Character name' });
  await user.clear(input);
  await user.type(input, 'Mentor');
  await user.click(screen.getByRole('button', { name: 'Save character name' }));

  await waitFor(() => expect(updateCharacterDefinition).toHaveBeenCalledWith(
    'guide', { display_name: 'Mentor' },
  ));
  expect(await screen.findByRole('button', { name: 'Rename Mentor' })).toBeInTheDocument();
  fireEvent.click(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Characters' }));
  expect(within(screen.getByLabelText('Characters navigation'))
    .getByRole('button', { name: /Mentor/ })).toBeInTheDocument();
});

it('replaces only the selected character file from the detail upload action', async () => {
  let content = '# Profile';
  const getCharacter = vi.fn(async () => ({ ...characterDetailFixture,
    markdown_files: ['CHARACTER.md', 'PROFILE.md'] }));
  const getCharacterFile = vi.fn(async (_id, filename) => ({ filename, content, writable: true }));
  const updateCharacterFile = vi.fn(async (_id, filename, replacement) => {
    content = replacement;
    return { filename, content, writable: true };
  });
  const { container } = render(<App client={fixtureClient({
    getCharacter, getCharacterFile, updateCharacterFile,
  })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  fireEvent.click(await screen.findByRole('button', { name: 'PROFILE.md' }));
  await screen.findByRole('button', { name: 'Replace character file content from file' });
  const file = new File(['# Replacement\n\nFresh voice.'], 'local.md', { type: 'text/markdown' });
  Object.defineProperty(file, 'text', { value: vi.fn(async () => '# Replacement\n\nFresh voice.') });
  fireEvent.change(container.querySelector('.cha-definition-upload input[type="file"]') as HTMLInputElement,
    { target: { files: [file] } });
  await waitFor(() => expect(updateCharacterFile).toHaveBeenCalledWith('guide', 'PROFILE.md', '# Replacement\n\nFresh voice.'));
  expect(await screen.findByRole('heading', { name: 'Replacement' })).toBeInTheDocument();
  expect(screen.getByText('Fresh voice.')).toBeInTheDocument();
});

it('retries a failed character-detail request without exposing implementation details', async () => {
  const getCharacter = vi.fn()
    .mockRejectedValueOnce(new ChaError('command_timeout', 'The request timed out.'))
    .mockResolvedValueOnce({
      ...characterDetailFixture,
      character_markdown: '# Guide dossier',
    });
  render(<App client={fixtureClient({ getCharacter })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('The request timed out.');
  fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
  expect(await screen.findByRole('button', { name: 'CHARACTER.md' })).toBeInTheDocument();
  expect(getCharacter).toHaveBeenCalledTimes(2);
});

it('creates a forum with its selected persona and opens the new forum', async () => {
  const user = userEvent.setup();
  let detail = forumDetailFixture;
  const createForum = vi.fn(async ({ display_name, persona_id }) => (detail = {
    ...forumDetailFixture,
    id: 'forum_1',
    display_name,
    default_character_id: 'assistant',
    default_persona_id: persona_id,
    default_persona_display_name: 'Reader',
    members: [bootstrapFixture.characters[0]],
    forum_markdown: '',
  }));
  render(<App client={fixtureClient({ createForum, getForum: async () => detail })} />);

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', {
    name: 'New forum',
  }));
  expect(screen.getByRole('heading', { name: 'New forum' })).toBeInTheDocument();
  await user.type(screen.getByRole('textbox', { name: 'Name' }), 'Brain Trust');
  await user.selectOptions(screen.getByRole('combobox', { name: 'Persona' }), 'reader');
  await user.click(screen.getByRole('button', { name: 'Create forum' }));

  await waitFor(() => expect(createForum).toHaveBeenCalledWith({
    display_name: 'Brain Trust',
    persona_id: 'reader',
  }));
  expect(await screen.findByRole('button', { name: 'Rename Brain Trust' }))
    .toBeInTheDocument();
  expect(screen.getByText('Assistant · speaking as Reader')).toBeInTheDocument();
});

it('prefers a forum’s configured description to its membership on the roster row', async () => {
  const described = structuredClone(bootstrapFixture);
  described.forums[1].description = 'Where the big questions get argued out';
  render(<App client={fixtureClient({ getBootstrap: async () => described })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));

  const forums = within(screen.getByLabelText('Forums navigation'));
  expect(forums.getByRole('button', { name: 'The LobbyWhere the big questions get argued out' }))
    .toBeInTheDocument();
  // A forum configuring none still names its cast rather than showing a bare row.
  expect(forums.getByRole('button', { name: 'EntranceAssistant' })).toBeInTheDocument();
});

it('opens the forum file list with members before displaying a selected file', async () => {
  const getForum = vi.fn(async () => forumDetailFixture);
  const getForumFile = vi.fn(async (_id, filename) => ({ filename, content: forumDetailFixture.forum_markdown, writable: true }));
  render(<App client={fixtureClient({ getForum, getForumFile })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  const sessions = within(screen.getByLabelText('Forum sessions navigation'));
  fireEvent.click(sessions.getByRole('button', { name: 'The LobbyGuide' }));
  expect(await screen.findByRole('button', { name: 'FORUM.md' })).toBeInTheDocument();
  expect(getForumFile).not.toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Members' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Rename The Lobby' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Delete The Lobby' })).toBeInTheDocument();
  expect(screen.getByText('Guide · speaking as Reader')).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Edit forum file' })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'FORUM.md' }));
  expect(await screen.findByRole('heading', { name: 'House rules' })).toBeInTheDocument();
  expect(screen.getByText('deliberate').tagName).toBe('STRONG');
  expect(getForumFile).toHaveBeenCalledWith('lobby', 'FORUM.md');
  expect(screen.getByRole('button', { name: 'Replace forum file content from file' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Edit forum file' })).toBeInTheDocument();
  fireEvent.click(within(screen.getByLabelText('Forum file navigation'))
    .getByRole('button', { name: 'The Lobby' }));
  fireEvent.click(within(screen.getByLabelText('Forum detail navigation'))
    .getByRole('button', { name: 'Sessions' }));
  expect(screen.getByRole('heading', { name: 'Sessions' })).toBeInTheDocument();
});

it('deletes a forum and removes its sessions from navigation after confirmation', async () => {
  const user = userEvent.setup();
  const deleteForum = vi.fn(async () => undefined);
  render(<App client={fixtureClient({ deleteForum })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'Delete The Lobby' }));

  expect(screen.getByRole('dialog')).toHaveTextContent(
    'Delete “The Lobby”? This permanently removes the forum and all of its sessions.',
  );
  await user.click(screen.getByRole('button', { name: 'Delete forum' }));

  await waitFor(() => expect(deleteForum).toHaveBeenCalledWith('lobby'));
  const forums = await screen.findByLabelText('Forums navigation');
  expect(within(forums).queryByRole('button', { name: /The Lobby/ })).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: /^Planning/ })).not.toBeInTheDocument();
});

it('renames a writable forum in place and updates its navigation immediately', async () => {
  const user = userEvent.setup();
  const updateForum = vi.fn(async (_forumId, update) => ({
    ...forumDetailFixture,
    ...update,
  }));
  render(<App client={fixtureClient({ updateForum })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  await user.click(await screen.findByRole('button', { name: 'Rename The Lobby' }));
  const input = screen.getByRole('textbox', { name: 'Forum name' });
  await user.clear(input);
  await user.type(input, 'Brain Trust');
  await user.click(screen.getByRole('button', { name: 'Save forum name' }));

  await waitFor(() => expect(updateForum).toHaveBeenCalledWith(
    'lobby', { display_name: 'Brain Trust' },
  ));
  expect(await screen.findByRole('button', { name: 'Rename Brain Trust' }))
    .toBeInTheDocument();
  await user.click(within(screen.getByLabelText('Forum detail navigation'))
    .getByRole('button', { name: 'Sessions' }));
  expect(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'Brain TrustGuide' })).toBeInTheDocument();
});

it('edits a forum’s members and persona with one Save action', async () => {
  const user = userEvent.setup();
  const bootstrap = structuredClone(bootstrapFixture);
  const critic = {
    ...bootstrap.characters[1],
    id: 'critic',
    display_name: 'Critic',
    description: 'Questions assumptions',
  };
  bootstrap.characters.push(critic);
  const updateForumMembers = vi.fn(async (_forumId, update) => ({
    ...forumDetailFixture,
    default_character_id: update.character_ids[0],
    default_persona_id: update.persona_id,
    default_persona_display_name: bootstrap.personas.find(
      ({ id }) => id === update.persona_id,
    )?.display_name ?? update.persona_id,
    members: bootstrap.characters.filter(({ id }) => update.character_ids.includes(id)),
  }));
  render(<App client={fixtureClient({
    getBootstrap: async () => bootstrap,
    updateForumMembers,
  })} />);

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'Members' }));

  expect(screen.getByRole('heading', { name: 'Members' })).toBeInTheDocument();
  expect(screen.getByRole('checkbox', { name: 'Guide' })).toBeChecked();
  expect(screen.getByRole('checkbox', { name: 'Critic' })).not.toBeChecked();
  const persona = screen.getByRole('combobox', { name: 'Persona' });
  expect(persona).toHaveValue('reader');
  const save = screen.getByRole('button', { name: 'Save' });
  expect(save).toBeDisabled();
  expect(save.parentElement).toHaveClass('cha-forum-members-actions');

  await user.selectOptions(persona, 'guest');
  expect(save).toBeEnabled();
  await user.selectOptions(persona, 'reader');
  expect(save).toBeDisabled();
  await user.click(screen.getByRole('checkbox', { name: 'Guide' }));
  await user.click(screen.getByRole('checkbox', { name: 'Critic' }));
  await user.selectOptions(persona, 'guest');
  expect(save).toBeEnabled();
  await user.click(save);

  await waitFor(() => expect(updateForumMembers).toHaveBeenCalledWith(
    'lobby', { character_ids: ['critic'], persona_id: 'guest' },
  ));
  expect(save).toBeDisabled();
  await user.click(within(screen.getByLabelText('Forum members navigation'))
    .getByRole('button', { name: 'The Lobby' }));
  expect(await screen.findByText('Critic · speaking as Guest')).toBeInTheDocument();
});

it('saves the selected forum file and returns to its list after deleting an optional file', async () => {
  const user = userEvent.setup();
  let content = '# Notes';
  let names = ['FORUM.md', 'NOTES.md'];
  const getForum = vi.fn(async () => ({ ...forumDetailFixture, markdown_files: names }));
  const getForumFile = vi.fn(async (_id, filename) => ({ filename, content, writable: true }));
  const updateForumFile = vi.fn(async (_id, filename, value) => {
    content = value;
    return { filename, content, writable: true };
  });
  const deleteForumFile = vi.fn(async () => { names = ['FORUM.md']; });
  const deleteForum = vi.fn();
  render(<App client={fixtureClient({ getForum, getForumFile,
    updateForumFile, deleteForumFile, deleteForum })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'NOTES.md' }));
  await user.click(screen.getByRole('button', { name: 'Edit forum file' }));
  const editor = await screen.findByRole('textbox', { name: 'Edit forum file text' });
  await waitFor(() => expect(editor).toHaveValue('# Notes'));
  await user.clear(editor);
  await user.type(editor, '# Updated notes');
  await user.click(screen.getByRole('button', { name: 'Save' }));
  await waitFor(() => expect(updateForumFile).toHaveBeenCalledWith('lobby', 'NOTES.md', '# Updated notes'));
  expect(await screen.findByRole('heading', { name: 'Updated notes' })).toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: 'Delete NOTES.md' }));
  expect(screen.getByRole('dialog')).toHaveTextContent('NOTES.md');
  await user.click(screen.getByRole('button', { name: 'Delete file' }));
  expect(await screen.findByRole('button', { name: 'FORUM.md' })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'NOTES.md' })).not.toBeInTheDocument();
  expect(deleteForumFile).toHaveBeenCalledWith('lobby', 'NOTES.md');
  expect(deleteForum).not.toHaveBeenCalled();
});

it('adds a forum file and opens the saved content', async () => {
  const user = userEvent.setup();
  let content = '';
  const createForumFile = vi.fn(async (_id, filename, value) => {
    content = value;
    return { filename, content, writable: true };
  });
  const getForumFile = vi.fn(async (_id, filename) => ({ filename, content, writable: true }));
  render(<App client={fixtureClient({ createForumFile, getForumFile })} />);
  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New file' }));
  await user.type(screen.getByRole('textbox', { name: 'Filename' }), 'NOTES');
  await user.type(screen.getByRole('textbox', { name: 'Content' }), '# New notes');
  await user.click(screen.getByRole('button', { name: 'Add file' }));
  expect(await screen.findByRole('heading', { name: 'New notes' })).toBeInTheDocument();
  expect(createForumFile).toHaveBeenCalledWith('lobby', 'NOTES.md', '# New notes');
});

it('replaces forum Markdown from the detail file action', async () => {
  let content = forumDetailFixture.forum_markdown;
  const getForumFile = vi.fn(async (_id, filename) => ({ filename, content, writable: true }));
  const updateForumFile = vi.fn(async (_id, filename, replacement) => {
    content = replacement;
    return { filename, content, writable: true };
  });
  const { container } = render(<App client={fixtureClient({ getForumFile, updateForumFile })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  fireEvent.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  fireEvent.click(await screen.findByRole('button', { name: 'FORUM.md' }));
  await screen.findByRole('button', { name: 'Replace forum file content from file' });
  const file = new File(['# New forum\n\nFresh rules.'], 'FORUM.md', {
    type: 'text/markdown',
  });
  Object.defineProperty(file, 'text', {
    value: vi.fn(async () => '# New forum\n\nFresh rules.'),
  });
  fireEvent.change(container.querySelector(
    '.cha-definition-upload input[type="file"]',
  ) as HTMLInputElement, { target: { files: [file] } });

  await waitFor(() => expect(updateForumFile).toHaveBeenCalledWith('lobby', 'FORUM.md', '# New forum\n\nFresh rules.'));
  expect(await screen.findByRole('heading', { name: 'New forum' })).toBeInTheDocument();
  expect(screen.getByText('Fresh rules.')).toBeInTheDocument();
});

it('reports an empty forum file rather than an empty screen', async () => {
  const getForum = vi.fn(async () => ({ ...forumDetailFixture, forum_markdown: '' }));
  render(<App client={fixtureClient({ getForum })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  fireEvent.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  fireEvent.click(await screen.findByRole('button', { name: 'FORUM.md' }));
  expect(await screen.findByText('This file is empty.'))
    .toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('retries a failed forum-detail request without exposing implementation details', async () => {
  const getForum = vi.fn()
    .mockRejectedValueOnce(new ChaError('command_timeout', 'The request timed out.'))
    .mockResolvedValueOnce(forumDetailFixture);
  render(<App client={fixtureClient({ getForum })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  fireEvent.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  expect(await screen.findByRole('alert')).toHaveTextContent('The request timed out.');
  fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
  expect(await screen.findByRole('button', { name: 'FORUM.md' })).toBeInTheDocument();
  expect(getForum).toHaveBeenCalledTimes(2);
});

it('says a forum has no sessions rather than showing an empty panel', async () => {
  render(<App client={fixtureClient({ listSessions: async () => [] })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));

  expect(await screen.findByText(/No sessions in this forum yet/)).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'New session' }))
    .toBeInTheDocument();

  // The built-in forum cannot be given new sessions, so it must explain itself
  // without pointing at an action that is not there.
  await openSettingsNavigation();
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

  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  const session = await screen.findByRole('button', { name: 'Planning2h' });
  expect(session).toHaveTextContent('Planning');
  expect(session).toHaveTextContent('2h');

  fireEvent.click(session);
  fireEvent.click(session);
  await waitFor(() => expect(open).toHaveBeenCalledWith('lobby', 'planning'));
  expect(open.mock.calls.filter(([, sessionId]) => sessionId === 'planning')).toHaveLength(1);
  await waitFor(() => expect(window.location.hash).toBe('#/s/lobby/planning/'));
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

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New session' }));

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
  expect(window.location.hash).toBe('#/s/lobby/created/');
  expect(screen.getByLabelText('Current chat context')).toHaveTextContent(
    'The LobbyFrom: ReaderTo: Guide',
  );
  expect(screen.getByRole('button', { name: 'Architecture reviewThe Lobby' }))
    .toHaveAttribute('aria-current', 'page');
});

it('transliterates Latin typing to Russian in a human-facing name field', async () => {
  const user = userEvent.setup();
  const createSession = vi.fn(async (_forumId: string, label: string) => ({
    id: 'created',
    label,
  }));
  render(<App
    client={fixtureClient({ listSessions: async () => [], createSession })}
    connectSessionEvents={inertSessionEvents}
  />);

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New session' }));

  const name = screen.getByRole('textbox', { name: 'Session name' });
  const toggle = screen.getByRole('button', { name: 'Latin to Russian transliteration' });
  await user.click(toggle);
  expect(name).toHaveFocus();
  await user.type(name, 'Obzor arhitektury');
  await user.click(screen.getByRole('button', { name: 'Start session' }));

  await waitFor(() => expect(createSession).toHaveBeenCalledWith(
    'lobby', 'Обзор архитектуры',
  ));
});

it('toggles Russian transliteration application-wide with Ctrl+Shift+Y', async () => {
  const user = userEvent.setup();
  render(<App client={fixtureClient()} connectSessionEvents={inertSessionEvents} />);

  await screen.findByRole('textbox', { name: 'Message' });
  let toggle = screen.getByRole('button', { name: 'Latin to Russian transliteration' });
  expect(toggle).toHaveAttribute('aria-pressed', 'false');
  expect(fireEvent.keyDown(window, {
    code: 'KeyY',
    ctrlKey: true,
    key: 'Y',
    shiftKey: true,
  })).toBe(false);
  await waitFor(() => expect(screen.getByRole('button', {
    name: 'Latin to Russian transliteration',
  })).toHaveAttribute('aria-pressed', 'true'));

  await openSettingsNavigation();
  await user.click(screen.getByRole('button', { name: 'Personas' }));
  await user.click(screen.getByRole('button', { name: 'New persona' }));
  const name = screen.getByRole('textbox', { name: 'Persona name' });
  toggle = screen.getByRole('button', { name: 'Latin to Russian transliteration' });
  expect(toggle).toHaveAttribute('aria-pressed', 'true');
  await user.type(name, 'Redaktor');
  expect(name).toHaveValue('Редактор');

  fireEvent.keyDown(window, {
    code: 'KeyY',
    ctrlKey: true,
    key: 'Y',
    shiftKey: true,
  });
  await waitFor(() => expect(toggle).toHaveAttribute('aria-pressed', 'false'));
  await user.type(name, ' Test');
  expect(name).toHaveValue('Редактор Test');
});

it('refreshes Recent and an open forum catalog after a sidebar rename', async () => {
  const user = userEvent.setup();
  let renamed = false;
  const refreshed = structuredClone(bootstrapFixture);
  refreshed.recent_sessions = refreshed.recent_sessions.map((session) => (
    session.session_id === 'planning'
      ? { ...session, session_label: 'Architecture review' }
      : session
  ));
  const getBootstrap = vi.fn()
    .mockResolvedValueOnce(bootstrapFixture)
    .mockResolvedValue(refreshed);
  const listSessions = vi.fn(async () => [{
    id: 'planning',
    label: renamed ? 'Architecture review' : 'Planning',
    live: false,
    updated_at: 1,
  }]);
  const renameSession = vi.fn(async (_forumId: string, sessionId: string, label: string) => {
    renamed = true;
    return { id: sessionId, label };
  });
  render(<App
    client={fixtureClient({ getBootstrap, listSessions, renameSession })}
    connectSessionEvents={inertSessionEvents}
  />);

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  expect(await within(screen.getByLabelText('Forum sessions navigation'))
    .findByRole('button', { name: /^Planning/ })).toBeInTheDocument();

  await user.click(screen.getByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
  const name = screen.getByLabelText('Session name');
  await user.clear(name);
  await user.type(name, 'Architecture review');
  await user.click(screen.getByRole('button', { name: 'Rename' }));

  await waitFor(() => expect(renameSession).toHaveBeenCalledWith(
    'lobby', 'planning', 'Architecture review',
  ));
  await waitFor(() => expect(getBootstrap).toHaveBeenCalledTimes(2));
  await waitFor(() => expect(listSessions.mock.calls.length).toBeGreaterThanOrEqual(2));
  expect(screen.getByRole('button', { name: 'Architecture reviewThe Lobby' }))
    .toBeInTheDocument();
  expect(sessionRow(/^Architecture review/)).toBeInTheDocument();
});

it('opens a save dialog and downloads the selected recent session', async () => {
  const user = userEvent.setup();
  const write = vi.fn(async () => undefined);
  const picker = vi.fn(async () => ({
    createWritable: async () => ({ write, close: async () => undefined }),
  }));
  Object.defineProperty(window, 'showSaveFilePicker', {
    configurable: true,
    value: picker,
  });
  const downloadSession = vi.fn(async () => '# Planning\n');
  const openSession = vi.fn(async (forum_id: string, session_id: string) => ({ forum_id, session_id }));
  render(<App
    client={fixtureClient({ downloadSession, openSession })}
    connectSessionEvents={inertSessionEvents}
  />);

  await screen.findByLabelText('Actions for Planning');
  await user.click(screen.getByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Download' }));

  await waitFor(() => expect(downloadSession).toHaveBeenCalledWith('lobby', 'planning'));
  expect(picker).toHaveBeenCalledWith(expect.objectContaining({ suggestedName: 'Planning.md' }));
  expect(write).toHaveBeenCalledWith('# Planning\n');
  expect(openSession).not.toHaveBeenCalledWith('lobby', 'planning');
  Reflect.deleteProperty(window, 'showSaveFilePicker');
});

it('clears the selected session audio cache from its menu without opening it', async () => {
  const user = userEvent.setup();
  const clearSessionAudioCache = vi.fn(async () => undefined);
  const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
    forum_id: forumId, session_id: sessionId,
  }));
  render(<App
    client={storedPlanningClient({ clearSessionAudioCache, openSession })}
    connectSessionEvents={drivableSessionEvents().connect}
  />);

  await user.click(await screen.findByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Clear audio cache' }));

  await waitFor(() => expect(clearSessionAudioCache).toHaveBeenCalledWith('lobby', 'planning'));
  expect(openSession.mock.calls.some(([forumId]) => forumId === 'lobby')).toBe(false);
  expect(screen.queryByRole('menu')).not.toBeInTheDocument();
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  expect(screen.getByLabelText('Actions for Planning')).toHaveFocus();
});

it('deleting the active session replaces its URL and returns to Welcome', async () => {
  const user = userEvent.setup();
  const events = drivableSessionEvents();
  const refreshed = structuredClone(bootstrapFixture);
  refreshed.recent_sessions = refreshed.recent_sessions.filter(
    ({ session_id }) => session_id !== 'planning',
  );
  const getBootstrap = vi.fn()
    .mockResolvedValueOnce(bootstrapFixture)
    .mockResolvedValueOnce(bootstrapFixture)
    .mockResolvedValue(refreshed);
  const deleteSession = vi.fn(async () => undefined);
  render(<App
    client={storedPlanningClient({ getBootstrap, deleteSession })}
    connectSessionEvents={events.connect}
  />);

  await waitFor(() => expect(events.connections[0]?.key).toBe('entrance/welcome'));
  act(() => events.handlers[0].onSnapshot(snapshotFixture));
  await user.click(screen.getByRole('button', { name: /^Planning/ }));
  await waitFor(() => expect(events.connections.some(({ key }) => key === 'lobby/planning')).toBe(true));
  const planning = events.connections.findIndex(({ key }) => key === 'lobby/planning');
  act(() => events.handlers[planning].onSnapshot(lobbySnapshot()));
  await waitFor(() => expect(window.location.hash).toBe('#/s/lobby/planning/'));

  await user.click(screen.getByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));

  await waitFor(() => expect(deleteSession).toHaveBeenCalledWith('lobby', 'planning'));
  await waitFor(() => expect(window.location.hash).toBe('#/'));
  await waitFor(() => expect(
    events.connections.filter(({ key }) => key === 'entrance/welcome'),
  ).toHaveLength(2));
  const welcome = events.connections.map(({ key }) => key).lastIndexOf('entrance/welcome');
  act(() => events.handlers[welcome].onSnapshot(snapshotFixture));
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(screen.queryByLabelText('Actions for Planning')).not.toBeInTheDocument();
});

it('refreshes the startup session before returning after deleting it', async () => {
  const user = userEvent.setup();
  const events = drivableSessionEvents();
  const starting = {
    ...bootstrapFixture,
    initial_forum_id: 'lobby',
    initial_session_id: 'planning',
  };
  const refreshed = {
    ...bootstrapFixture,
    recent_sessions: bootstrapFixture.recent_sessions.filter(
      ({ session_id }) => session_id !== 'planning',
    ),
  };
  let deleted = false;
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    if (deleted && forumId === 'lobby' && sessionId === 'planning') {
      throw new ChaError('not_found', 'The requested session could not be opened.');
    }
    return { forum_id: forumId, session_id: sessionId };
  });
  const deleteSession = vi.fn(async () => { deleted = true; });
  render(<App
    client={storedPlanningClient({
      getBootstrap: async () => (deleted ? refreshed : starting),
      openSession,
      deleteSession,
    })}
    connectSessionEvents={events.connect}
  />);

  await waitFor(() => expect(events.connections[0]?.key).toBe('lobby/planning'));
  act(() => events.handlers[0].onSnapshot(lobbySnapshot()));
  await user.click(screen.getByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));

  await waitFor(() => expect(deleteSession).toHaveBeenCalledWith('lobby', 'planning'));
  await waitFor(() => expect(events.connections.some(({ key }) => key === 'entrance/welcome')).toBe(true));
  const welcome = events.connections.findIndex(({ key }) => key === 'entrance/welcome');
  act(() => events.handlers[welcome].onSnapshot(snapshotFixture));
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(window.location.hash).toBe('#/');
  expect(openSession.mock.calls.filter(([forumId, sessionId]) => (
    forumId === 'lobby' && sessionId === 'planning'
  ))).toHaveLength(1);
  expect(screen.queryByRole('heading', { name: 'Session unavailable' })).not.toBeInTheDocument();
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

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New session' }));
  await user.type(screen.getByRole('textbox', { name: 'Session name' }), 'Architecture review');
  await user.click(screen.getByRole('button', { name: 'Start session' }));
  await waitFor(() => expect(createSession).toHaveBeenCalled());
  const listedBeforeCancel = getBootstrap.mock.calls.length;

  expect(screen.getByRole('status')).toHaveTextContent('Creating session');
  await openSettingsNavigation();
  await user.click(screen.getByRole('button', { name: 'Characters' }));
  expect(screen.getByLabelText('Characters navigation')).toBeInTheDocument();
  await act(async () => {
    finishCreate({ id: 'created', label: 'Architecture review' });
  });

  expect(openSession).not.toHaveBeenCalledWith('lobby', 'created');
  expect(screen.getByLabelText('Characters navigation')).toBeInTheDocument();
  expect(screen.queryByText('Creating session…')).not.toBeInTheDocument();
  await waitFor(() => expect(getBootstrap.mock.calls.length).toBe(listedBeforeCancel + 1));
});

it('lets the reader browse forums when a deep-linked session cannot open', async () => {
  window.history.replaceState(null, '', '/#/s/lobby/planning/');
  const client = fixtureClient({
    openSession: async (forumId, sessionId) => {
      if (sessionId === 'planning') {
        throw new ChaError('internal_error', 'Planning could not be opened.');
      }
      return { forum_id: forumId, session_id: sessionId };
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('could not be opened');
  fireEvent.click(screen.getByRole('button', { name: 'Browse sessions' }));
  expect(screen.getByLabelText('Forums navigation')).toBeInTheDocument();
});

it('does not reopen a failed start session when browsing forums', async () => {
  const bootstrap = {
    ...bootstrapFixture,
    initial_forum_id: 'lobby',
    initial_session_id: 'planning',
  };
  const openSession = vi.fn(async () => {
    throw new ChaError('not_found', 'Planning could not be opened.');
  });
  render(<App client={fixtureClient({
    getBootstrap: async () => bootstrap,
    openSession,
  })} />);

  expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Browse sessions' }));
  expect(screen.getByLabelText('Forums navigation')).toBeInTheDocument();
  expect(openSession).toHaveBeenCalledTimes(1);
});

it('opens and snapshots a session-shaped deep link before showing Chat', async () => {
  window.history.replaceState(null, '', '/#/s/lobby/planning/');
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
  expect(window.location.hash).toBe('#/s/lobby/planning/');
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

it.each(['session_stopping', 'session_open_timeout'] as const)(
  'offers Retry when open fails with %s',
  async (code) => {
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    render(
      <App
        client={fixtureClient({
          openSession: async () => {
            throw new ChaError(code, `Retryable ${code}`);
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
  await waitFor(() => expect(window.location.hash).toBe('#/s/lobby/planning/'));
  const planning = events.connections.find(({ key }) => key === 'lobby/planning');
  expect(planning).toBeDefined();

  goBackTo('/');

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(planning?.close).toHaveBeenCalled();
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
  expect(window.location.hash).toBe('#/s/lobby/planning/');
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

it('does not abandon an in-flight session open when merge refreshes bootstrap', async () => {
  const user = userEvent.setup();
  const refresh = deferred();
  const opened = deferred();
  let bootstraps = 0;
  const getBootstrap = vi.fn(async () => {
    bootstraps += 1;
    if (bootstraps > 1) await refresh.promise;
    return bootstrapFixture;
  });
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    if (sessionId === 'planning') await opened.promise;
    return { forum_id: forumId, session_id: sessionId };
  });
  const events = recordingSessionEvents();
  render(
    <App
      client={storedPlanningClient({
        getBootstrap,
        openSession,
        listVaults: async () => [
          {
            display_name: 'Personal',
            protected: false,
            data_path: '/data/personal.sqlite3',
            mirror_path: null,
            modify_path: '/work/personal',
            active: true,
            can_delete: false,
          },
          {
            display_name: 'Projects',
            protected: false,
            data_path: '/data/projects.sqlite3',
            mirror_path: null,
            modify_path: null,
            active: false,
            can_delete: true,
          },
        ],
      })}
      connectSessionEvents={events.connect}
    />,
  );

  await user.click(await screen.findByLabelText('Settings'));
  await user.click(await screen.findByRole('button', { name: /Vaults/ }));
  await user.click(await screen.findByRole('button', { name: /Personal/ }));
  await user.click(await screen.findByRole('button', { name: 'Merge' }));
  await user.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
  await user.click(screen.getByRole('button', { name: 'Merge' }));
  await user.click(within(await screen.findByRole('dialog')).getByRole('button', { name: 'Merge' }));
  await waitFor(() => expect(getBootstrap).toHaveBeenCalledTimes(2));

  await user.click(screen.getByRole('button', { name: /^Planning/ }));
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Opening session'));

  refresh.settle();
  opened.settle();

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('The Lobby'));
  expect(events.connections.filter(({ key }) => key === 'lobby/planning'))
    .toEqual([expect.objectContaining({ key: 'lobby/planning' })]);
});

it('clears a live chat and reopens its current vault snapshot after Settings download', async () => {
  const requests: NativeRequest[] = [];
  const bridge = createEnvelopeNativeBridge({
    connectionId: 'view-test',
    post: (message) => {
      if ('method' in (message as object)) requests.push(message as NativeRequest);
    },
  });
  bridge.setContextEpoch(1);
  const events = drivableSessionEvents();
  const bootstrap = {
    ...bootstrapFixture,
    capabilities: { can_modify: true, can_transfer_r2: true },
  };
  let replaced = false;
  const getBootstrap = vi.fn(async () => replaced
    ? { ...bootstrap, vault_name: 'personal', vaults: ['personal', 'Projects'] }
    : bootstrap);
  const getSessionSnapshot = vi.fn(async () => ({
    ...snapshotFixture,
    session_label: replaced ? 'Fresh vault' : 'Old vault',
  }));
  const client = fixtureClient({
    getBootstrap,
    getSessionSnapshot,
    listVaults: async () => [{
      display_name: replaced ? 'personal' : 'Personal', protected: false,
      data_path: '/data/personal.sqlite3', mirror_path: null,
      modify_path: '/work/personal', active: true, can_delete: false,
    }],
    downloadVault: async () => (await bridge.invoke<{ byte_count: number }>(
      'vault.download', {},
    )).byte_count,
  });
  render(<App client={client} contextEvents={bridge} connectSessionEvents={events.connect} />);

  await waitFor(() => expect(events.connections).toHaveLength(1));
  act(() => events.handlers[0].onSnapshot({
    ...snapshotFixture, session_label: 'Old vault',
  }));
  expect(getSessionSnapshot).toHaveBeenCalledOnce();
  expect(screen.getByText('Old vault')).toBeInTheDocument();

  await userEvent.click(screen.getByLabelText('Settings'));
  await userEvent.click(await screen.findByRole('button', { name: /Vaults/ }));
  await userEvent.click(await screen.findByRole('button', { name: /Personal/ }));
  await userEvent.click(await screen.findByRole('button', { name: 'Download' }));
  await userEvent.click(within(await screen.findByRole('dialog')).getByRole(
    'button', { name: 'Download' },
  ));
  await waitFor(() => expect(requests.some(({ method }) => method === 'vault.download')).toBe(true));
  const request = requests.find(({ method }) => method === 'vault.download')!;
  replaced = true;
  act(() => bridge.receive({
    connection_id: 'view-test', delivery_id: 1,
    messages: [
      { connection_id: 'view-test', event: 'app.contextChanged', context_epoch: 2,
        state: 'running', causing_request_id: request.id },
      { connection_id: 'view-test', id: request.id, context_epoch: 2, ok: true,
        result: { byte_count: 34, context_epoch: 2 } },
    ],
  }));

  await waitFor(() => expect(events.connections[0].close).toHaveBeenCalledOnce());
  await waitFor(() => expect(getBootstrap.mock.calls.length).toBeGreaterThan(1));
  expect(await screen.findByRole('status')).toHaveTextContent('Downloaded 34 bytes.');
  await waitFor(() => expect(screen.getByRole('region', { name: 'Vault settings' }))
    .toHaveTextContent('personal'));
  await userEvent.click(screen.getByRole('button', { name: /^Welcome/ }));
  await waitFor(() => expect(getSessionSnapshot).toHaveBeenCalledTimes(2));
  await waitFor(() => expect(events.connections).toHaveLength(2));
  expect(screen.getByText('Fresh vault')).toBeInTheDocument();
  expect(screen.queryByText('Old vault')).not.toBeInTheDocument();
  bridge.dispose();
});

it('offers bootstrap retry when discovery refresh fails after Settings download', async () => {
  const requests: NativeRequest[] = [];
  const bridge = createEnvelopeNativeBridge({
    connectionId: 'refresh-test',
    post: (message) => {
      if ('method' in (message as object)) requests.push(message as NativeRequest);
    },
  });
  bridge.setContextEpoch(1);
  const bootstrap = {
    ...bootstrapFixture,
    capabilities: { can_modify: true, can_transfer_r2: true },
  };
  const getBootstrap = vi.fn()
    .mockResolvedValueOnce(bootstrap)
    .mockRejectedValueOnce(new ChaError('application_unavailable', 'Discovery failed.'))
    .mockResolvedValue(bootstrap);
  const client = fixtureClient({
    getBootstrap,
    listVaults: async () => [{
      display_name: 'Personal', protected: false,
      data_path: '/data/personal.sqlite3', mirror_path: null,
      modify_path: '/work/personal', active: true, can_delete: false,
    }],
    downloadVault: async () => (await bridge.invoke<{ byte_count: number }>(
      'vault.download', {},
    )).byte_count,
  });
  render(<App client={client} contextEvents={bridge} connectSessionEvents={inertSessionEvents} />);

  await userEvent.click(await screen.findByLabelText('Settings'));
  await userEvent.click(await screen.findByRole('button', { name: /Vaults/ }));
  await userEvent.click(await screen.findByRole('button', { name: /Personal/ }));
  await userEvent.click(await screen.findByRole('button', { name: 'Download' }));
  await userEvent.click(within(await screen.findByRole('dialog')).getByRole(
    'button', { name: 'Download' },
  ));
  await waitFor(() => expect(requests.some(({ method }) => method === 'vault.download')).toBe(true));
  const request = requests.find(({ method }) => method === 'vault.download')!;
  act(() => bridge.receive({
    connection_id: 'refresh-test', delivery_id: 1,
    messages: [
      { connection_id: 'refresh-test', event: 'app.contextChanged', context_epoch: 2,
        state: 'running', causing_request_id: request.id },
      { connection_id: 'refresh-test', id: request.id, context_epoch: 2, ok: true,
        result: { byte_count: 34, context_epoch: 2 } },
    ],
  }));

  expect(await screen.findByText('Application API unavailable')).toBeInTheDocument();
  expect(screen.getByText('Discovery failed.')).toBeInTheDocument();
  await userEvent.click(screen.getByRole('button', { name: 'Retry' }));
  await waitFor(() => expect(getBootstrap).toHaveBeenCalledTimes(3));
  expect(screen.queryByText('Application API unavailable')).not.toBeInTheDocument();
  expect(await screen.findByRole('button', { name: /^Welcome/ })).toBeInTheDocument();
  bridge.dispose();
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
  expect(events.connections.filter(({ close }) => !close.mock.calls.length))
    .toEqual([expect.objectContaining({ key: 'entrance/welcome' })]);
  expect(window.location.hash).toBe('#/s/entrance/welcome/');
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
  await waitFor(() => expect(
    events.connections.some(({ key }) => key === 'lobby/planning'),
  ).toBe(true));
  const planning = events.connections.filter(({ key }) => key === 'lobby/planning').at(-1);
  const previousConnectionCount = events.connections.length;

  goBackTo('/s/entrance/welcome/');
  await waitFor(() => expect(events.connections.length).toBeGreaterThan(previousConnectionCount));
  const successor = events.connections.slice(previousConnectionCount)
    .find(({ key }) => key === 'entrance/welcome');
  held.settle();

  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(planning?.close).toHaveBeenCalled();
  expect(successor?.close).not.toHaveBeenCalled();
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

  await openSettingsNavigation();
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

it.each(['internal_error', 'session_open_timeout'] as const)(
  'reports a failed create in chat without retrying an earlier session (%s)', async (code) => {
  const user = userEvent.setup();
  const client = fixtureClient({
    listSessions: async () => [],
    createSession: async () => {
      throw new ChaError(code, 'The session could not be created.');
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  await openSettingsNavigation();
  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New session' }));
  await user.type(screen.getByRole('textbox', { name: 'Session name' }), 'Architecture review');
  await user.click(screen.getByRole('button', { name: 'Start session' }));

  expect(await screen.findByRole('alert')).toHaveTextContent('The session could not be created.');
  expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
  expect(screen.getByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  expect(screen.queryByRole('textbox', { name: 'Session name' })).not.toBeInTheDocument();
  await openSettingsNavigation();
  await user.click(screen.getByRole('button', { name: 'Forums' }));
  expect(screen.getByLabelText('Forums navigation')).toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('reports a failed open in chat and allows returning to the sessions list', async () => {
  const client = storedPlanningClient({
    openSession: async () => {
      throw new ChaError('session_stopping', 'Planning is still stopping.');
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);
  await openPlanningFromTheLobby();

  expect(await screen.findByRole('alert')).toHaveTextContent('still stopping');
  expect(screen.getByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  expect(await within(screen.getByLabelText('Forum sessions navigation')).findByRole('button', { name: /^Planning/ })).toBeEnabled();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(window.location.pathname).toBe('/');
});

it('moves from a navigation screen to chat to report a Recent open failure', async () => {
  const client = fixtureClient({
    openSession: async (forumId, sessionId) => {
      if (sessionId === 'planning') {
        throw new ChaError('session_stopping', 'Planning is still stopping.');
      }
      return { forum_id: forumId, session_id: sessionId };
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);
  await screen.findByLabelText('Current chat context');

  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  const recent = within(screen.getByLabelText('Recent sessions'));
  fireEvent.click(recent.getByRole('button', { name: /^Planning/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('Planning is still stopping.');
  expect(screen.queryByLabelText('Characters navigation')).not.toBeInTheDocument();
  expect(screen.getByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Browse sessions' })).toBeInTheDocument();
});

it('shows an invalid session address reached from Settings and lets the reader leave', async () => {
  render(<App client={fixtureClient()} connectSessionEvents={inertSessionEvents} />);
  await screen.findByLabelText('Chat area');
  fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
  expect(await screen.findByRole('region', { name: 'Configuration' })).toBeInTheDocument();

  act(() => {
    window.history.replaceState(null, '', '/#/invalid');
    window.dispatchEvent(new HashChangeEvent('hashchange'));
  });
  expect(await screen.findByRole('alert')).toHaveTextContent('This address does not identify a CHA session.');
  expect(screen.getByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Personas' }));
  expect(screen.getByLabelText('Personas navigation')).toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('opens recent forums in Sessions and offers New session only for a stored forum', async () => {
  render(<App client={fixtureClient()} connectSessionEvents={inertSessionEvents} />);

  const forums = within(screen.getByRole('navigation', { name: 'Recent forums' }));
  const lobby = await forums.findByRole('button', { name: 'The Lobby' });
  fireEvent.click(lobby);
  expect(await screen.findByRole('button', { name: 'New session' }))
    .toBeInTheDocument();
  expect(lobby).toHaveAttribute('aria-current', 'page');
  expect(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' })).toHaveClass('is-current');

  expect(forums.queryByRole('button', { name: 'Entrance' })).not.toBeInTheDocument();
  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'EntranceAssistant' }));
  await waitFor(() => expect(screen.getByRole('heading', { name: 'Sessions' })).toBeInTheDocument());
  expect(screen.queryByRole('button', { name: /New session/ })).not.toBeInTheDocument();
  expect(lobby).not.toHaveAttribute('aria-current');
});

it('offers New session when the initial conversation belongs to a stored forum', async () => {
  const bootstrap = {
    ...bootstrapFixture,
    initial_forum_id: 'lobby',
    initial_session_id: 'planning',
  };
  render(<App client={fixtureClient({ getBootstrap: async () => bootstrap })} connectSessionEvents={inertSessionEvents} />);

  const forums = within(screen.getByRole('navigation', { name: 'Recent forums' }));
  fireEvent.click(await forums.findByRole('button', { name: 'The Lobby' }));
  expect(await screen.findByRole('button', { name: 'New session' })).toBeInTheDocument();

  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'EntranceAssistant' }));
  expect(screen.queryByRole('button', { name: /New session/ })).not.toBeInTheDocument();
});

it('replaces a failed stream without reopening the session', async () => {
  const events = drivableSessionEvents();
  const client = storedPlanningClient();
  const getSessionSnapshot = vi.spyOn(client, 'getSessionSnapshot');
  render(
    <App
      client={client}
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
  expect(getSessionSnapshot.mock.calls.filter(([forumId, sessionId]) => (
    forumId === 'lobby' && sessionId === 'planning'
  ))).toHaveLength(1);

  const reconnected = events.connections.map(({ key }) => key).lastIndexOf('lobby/planning');
  act(() => events.handlers[reconnected].onSnapshot({
    ...lobbySnapshot(), session_label: 'Recovered planning',
  }));
  await waitFor(() => expect(screen.queryByText(/Reconnecting live updates/)).not.toBeInTheDocument());
  expect(within(screen.getByLabelText('Chat area')).getByText('Recovered planning')).toBeInTheDocument();
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
    .mockRejectedValueOnce(new Error('read /private/customer/.env: CHA_R2_SECRET_ACCESS_KEY=secret'))
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

it('opens Settings from the gear, fetches OpenAI status, and keeps the conversation', async () => {
  const getOpenAiAuth = vi.fn(async () => ({ status: 'signed_out' as const }));
  render(
    <App
      client={fixtureClient({ getOpenAiAuth })}
      connectSessionEvents={inertSessionEvents}
    />,
  );
  await screen.findByLabelText('Current chat context');

  expect(screen.getByRole('combobox', { name: 'Choose message target' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
  expect(screen.queryByRole('button', { name: 'Personas' })).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'OpenAI' })).not.toBeInTheDocument();
  fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
  expect(await screen.findByRole('heading', { name: 'Settings' })).toBeInTheDocument();
  expect(screen.getByRole('heading', { name: 'OpenAI' })).toBeInTheDocument();
  expect(await screen.findByRole('button', { name: 'Connect ChatGPT' })).toBeEnabled();
  expect(getOpenAiAuth).toHaveBeenCalledTimes(1);

  fireEvent.click(screen.getByRole('button', { name: 'WelcomeEntrance' }));
  expect(await screen.findByLabelText('Current chat context')).toHaveTextContent('Entrance');
  expect(screen.queryByRole('button', { name: 'Connect ChatGPT' })).not.toBeInTheDocument();
});

it('does not let a late OpenAI status replace a view selected after Settings', async () => {
  let finish!: (snapshot: typeof waitingAuth) => void;
  const getOpenAiAuth = vi.fn(() => new Promise<typeof waitingAuth>((resolve) => {
    finish = resolve;
  }));
  render(
    <App
      client={fixtureClient({ getOpenAiAuth })}
      connectSessionEvents={inertSessionEvents}
    />,
  );
  fireEvent.click(await within(screen.getByLabelText('Sidebar')).findByRole('button', { name: 'Settings' }));
  expect(await screen.findByText('Loading ChatGPT connection…')).toBeInTheDocument();

  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  expect(await screen.findByLabelText('Characters navigation')).toBeInTheDocument();
  await act(async () => { finish(waitingAuth); });

  expect(screen.getByLabelText('Characters navigation')).toBeInTheDocument();
  expect(screen.queryByText('TEST-ONLY')).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Connect ChatGPT' })).not.toBeInTheDocument();
});

it('shows the settings row only after a writable character detail loads', async () => {
  let finish!: (detail: CharacterDetail) => void;
  const getCharacter = vi.fn(() => new Promise<CharacterDetail>((resolve) => {
    finish = resolve;
  }));
  render(<App client={fixtureClient({ getCharacter })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));

  expect(await screen.findByText('Loading character…')).toBeInTheDocument();
  expect(screen.getByRole('heading', { name: 'Guide' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Rename Guide' })).not.toBeInTheDocument();

  await act(async () => { finish(characterDetailFixture); });
  expect(await within(screen.getByLabelText('Character detail navigation'))
    .findByRole('button', { name: 'Settings' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Rename Guide' })).toBeInTheDocument();
  expect(screen.queryByRole('heading', { name: 'Guide' })).not.toBeInTheDocument();
});

it('omits the settings row for a character that is not writable', async () => {
  render(<App client={fixtureClient({
    getCharacter: async () => ({
      ...characterDetailFixture, settings_writable: false, writable: false,
    }),
  })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  expect(await screen.findByRole('button', { name: 'CHARACTER.md' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
  expect(screen.getByRole('heading', { name: 'Guide' })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Rename Guide' })).not.toBeInTheDocument();
});

it('keeps a late character detail from lending its settings row to the next character', async () => {
  let finishGuide!: (detail: CharacterDetail) => void;
  const getCharacter = vi.fn((characterId: string) => {
    if (characterId === 'guide') {
      return new Promise<CharacterDetail>((resolve) => { finishGuide = resolve; });
    }
    // Assistant settings are available, but its built-in definition stays read-only.
    return Promise.resolve({
      ...characterDetailFixture,
      id: 'assistant',
      display_name: 'Assistant',
      settings_writable: true,
      writable: false,
    });
  });
  render(<App client={fixtureClient({ getCharacter })} />);
  await openSettingsNavigation();
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));

  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  fireEvent.click(document.querySelector('.cha-back-row') as HTMLElement);
  fireEvent.click(screen.getByRole('button', { name: /Assistant/ }));
  expect(await screen.findByRole('button', { name: 'CHARACTER.md' })).toBeInTheDocument();

  await act(async () => { finishGuide({ ...characterDetailFixture, writable: true }); });

  expect(screen.getByRole('heading', { name: 'Assistant' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Settings' })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Rename Assistant' })).not.toBeInTheDocument();
});

function planningVoiceSnapshot(appearance: CharacterAppearance): SessionSnapshot {
  return {
    ...lobbySnapshot(),
    characters: [{ ...bootstrapFixture.characters[1], appearance }],
    transcript: [{
      id: 1,
      kind: 'character',
      participant_id: 'guide',
      display_name: 'Guide',
      addressed_to: 'guest',
      addressed_to_name: 'Guest',
      text: 'A considered answer',
      status: 'complete',
      created_at: null,
    }],
  };
}

async function openGuideSettingsFromPlanning(
  events: ReturnType<typeof drivableSessionEvents>,
  snapshot: SessionSnapshot,
) {
  await openPlanningFromTheLobby();
  await waitFor(() => expect(events.connections.some(({ key }) => key === 'lobby/planning')).toBe(true));
  const planning = events.connections.findIndex(({ key }) => key === 'lobby/planning');
  act(() => events.handlers[planning].onSnapshot(snapshot));
  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  fireEvent.click(await within(screen.getByLabelText('Character detail navigation'))
    .findByRole('button', { name: 'Settings' }));
  expect(await screen.findByRole('heading', { name: 'Settings' })).toBeInTheDocument();
  return planning;
}

it('refreshes appearance on the existing subscription without leaving settings', async () => {
  const user = userEvent.setup();
  const events = drivableSessionEvents();
  const previous = planningVoiceSnapshot(serifItalicVoice);
  const next = planningVoiceSnapshot(monoLargeVoice);
  const getSessionSnapshot = vi.fn(async (forumId: string) => (
    forumId === 'lobby' ? previous : snapshotFixture
  ));
  const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
    forum_id: forumId,
    session_id: sessionId,
  }));
  const updateCharacter = vi.fn(async () => ({
    ...characterDetailFixture,
    style: 'mono-large',
  }));
  render(<App
    client={storedPlanningClient({ getSessionSnapshot, openSession, updateCharacter })}
    connectSessionEvents={events.connect}
    retryDelays={[0]}
  />);

  const planning = await openGuideSettingsFromPlanning(events, previous);
  await user.selectOptions(await screen.findByLabelText('Style'), 'mono-large');
  await user.click(screen.getByRole('button', { name: 'Save' }));
  await waitFor(() => expect(updateCharacter).toHaveBeenCalledWith('guide', {
    provider: 'terra',
    reasoning_effort: null,
    style: 'mono-large',
    voice_id: null,
    web_search: null, web_search_tool: null,
  }));

  act(() => events.handlers[planning].onSnapshot(next));
  expect(events.connections.filter(({ key }) => key === 'lobby/planning')).toHaveLength(1);
  expect(events.connections[planning].close).not.toHaveBeenCalled();

  expect(document.querySelector('main')).toHaveAttribute('data-view', 'character-settings');
  expect(openSession.mock.calls.filter(([, sessionId]) => sessionId === 'planning')).toHaveLength(1);

  fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(screen.getByText('A considered answer')).toHaveClass(
    'cha-message-text', 'cha-font-mono', 'cha-scale-large',
  );
});

it('reopens from a native reload snapshot and retries until the old owner stops', async () => {
  const requests: NativeRequest[] = [];
  const bridge = createEnvelopeNativeBridge({
    connectionId: 'view-test',
    post: (message) => {
      if ('method' in (message as object)) requests.push(message as NativeRequest);
    },
  });
  bridge.setContextEpoch(1);
  const connect = createNativeSessionEvents(bridge, {
    connectionId: 'view-test', contextEpoch: () => bridge.contextEpoch(),
  });
  const subscriptions = () => requests.filter((request) => (
    request.method === 'session.subscribe' && request.params?.session_id === 'planning'
  ));
  let delivery = 0;
  const publish = (snapshot: SessionSnapshot, seq: number) => {
    const request = subscriptions().at(-1)!;
    bridge.receive({
      connection_id: 'view-test', delivery_id: ++delivery,
      messages: [{
        connection_id: 'view-test', context_epoch: 1,
        subscription_id: request.params!.subscription_id,
        event: 'session.snapshot', forum_id: 'lobby', session_id: 'planning',
        seq, payload: snapshot,
      }],
    });
  };
  let opens = 0;
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    if (sessionId === 'planning' && ++opens === 2) {
      throw new ChaError('session_stopping', 'Still stopping');
    }
    return { forum_id: forumId, session_id: sessionId };
  });
  const view = render(<App
    client={storedPlanningClient({ openSession })}
    connectSessionEvents={connect}
    retryDelays={[0]}
  />);
  await openPlanningFromTheLobby();
  await waitFor(() => expect(subscriptions()).toHaveLength(1));
  act(() => publish(lobbySnapshot(), 0));
  await openSettingsNavigation();
  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  act(() => publish({
    ...lobbySnapshot(), lifecycle: 'stopping', shutdown_reason: 'reloading',
  }, 1));

  await waitFor(() => expect(subscriptions()).toHaveLength(2));
  expect(opens).toBe(3);
  expect(document.querySelector('main')).toHaveAttribute('data-view', 'characters');
  // Another save can stop the replacement before its successful attach has
  // finished settling. Its terminal event must supersede that recovery too.
  act(() => {
    publish(lobbySnapshot(), 0);
    publish({ ...lobbySnapshot(), lifecycle: 'stopping', shutdown_reason: 'reloading' }, 1);
  });
  await waitFor(() => expect(subscriptions()).toHaveLength(3));
  expect(opens).toBe(4);
  act(() => publish(lobbySnapshot(), 0));
  fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(screen.queryByText('Applying settings…')).not.toBeInTheDocument();
  expect(screen.queryByText(/Reconnecting live updates/)).not.toBeInTheDocument();
  expect(screen.getByLabelText('Current chat context')).toHaveTextContent('The Lobby');
  view.unmount();
  bridge.dispose();
});

it('clears the conversation route before reloading after a vault switch', async () => {
  const user = userEvent.setup();
  const switchVault = vi.fn(async () => undefined);
  const reload = vi.fn();
  render(
    <App
      client={fixtureClient({ switchVault })}
      connectSessionEvents={inertSessionEvents}
      reload={reload}
    />,
  );

  const vault = await screen.findByLabelText('Vault');
  window.history.replaceState(null, '', '/#/s/entrance/welcome/');
  await user.selectOptions(vault, 'Projects');
  await waitFor(() => expect(switchVault).toHaveBeenCalledWith('Projects', undefined));
  expect(window.location.hash).toBe('#/');
  expect(reload).toHaveBeenCalledOnce();
});

it('refreshes a switched vault without reloading the native document', async () => {
  const requests: NativeRequest[] = [];
  const bridge = createEnvelopeNativeBridge({
    connectionId: 'view-test',
    post: (message) => {
      if ('method' in (message as object)) requests.push(message as NativeRequest);
    },
  });
  bridge.setContextEpoch(1);
  let switched = false;
  const getBootstrap = vi.fn(async () => switched
    ? { ...bootstrapFixture, vault_name: 'Projects' }
    : bootstrapFixture);
  const reload = vi.fn();
  render(
    <App
      client={fixtureClient({
        getBootstrap,
        switchVault: async (name) => {
          expect(name).toBe('Projects');
          await bridge.invoke('vault.switch', { vault_name: name });
        },
      })}
      contextEvents={bridge}
      connectSessionEvents={inertSessionEvents}
      reload={reload}
    />,
  );

  const vault = await screen.findByLabelText('Vault');
  window.history.replaceState(null, '', '/#/s/entrance/welcome/');
  await userEvent.selectOptions(vault, 'Projects');
  await waitFor(() => expect(requests.some(({ method }) => method === 'vault.switch')).toBe(true));
  const request = requests.find(({ method }) => method === 'vault.switch')!;
  switched = true;
  act(() => bridge.receive({
    connection_id: 'view-test', delivery_id: 1,
    messages: [
      { connection_id: 'view-test', event: 'app.contextChanged', context_epoch: 2,
        state: 'running', causing_request_id: request.id },
      { connection_id: 'view-test', id: request.id, context_epoch: 2, ok: true,
        result: { state: 'running', context_epoch: 2 } },
    ],
  }));

  await waitFor(() => expect(screen.getByLabelText('Vault')).toHaveValue('Projects'));
  expect(window.location.hash).toBe('#/');
  expect(reload).not.toHaveBeenCalled();
  expect(getBootstrap.mock.calls.length).toBeGreaterThan(1);
  bridge.dispose();
});
