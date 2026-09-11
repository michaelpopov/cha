import { StrictMode } from 'react';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  ChaUnavailableError,
  type Bootstrap,
  type CharacterAppearance,
  type CharacterDetail,
  type SessionSnapshot,
} from '../api/client';
import type { SessionEventHandlers } from '../api/events';
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

it('names the window after the active vault', async () => {
  render(<App client={fixtureClient()} />);

  await waitFor(() => expect(document.title).toBe('CHA: Personal'));
});

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

it('lists every persona and renders its Markdown', async () => {
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
  expect(screen.getByRole('button', { name: 'Rename Reader' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Persona detail navigation'))
    .getByRole('button', { name: 'Personas' }));
  expect(screen.getByRole('heading', { name: 'Personas' })).toBeInTheDocument();
});

it('creates a named persona and adds it to the roster immediately', async () => {
  const user = userEvent.setup();
  const created = {
    id: 'persona_1',
    display_name: 'Project manager',
    persona_markdown: '',
    writable: true,
  };
  const createPersona = vi.fn(async () => created);
  const getPersona = vi.fn(async (personaId: string) => (
    personaId === created.id ? created : personaDetailFixture
  ));
  render(<App client={fixtureClient({ createPersona, getPersona })} />);
  await user.click(await screen.findByRole('button', { name: 'Personas' }));
  await user.click(screen.getByRole('button', {
    name: 'New personaEnter a name to begin',
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

it('renames a writable persona in place and updates the roster immediately', async () => {
  const user = userEvent.setup();
  const updatePersona = vi.fn(async (_personaId, update) => ({
    ...personaDetailFixture,
    ...update,
  }));
  render(<App client={fixtureClient({ updatePersona })} />);
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
});

it('replaces persona Markdown from the compact file action', async () => {
  let detail = personaDetailFixture;
  const getPersona = vi.fn(async () => detail);
  const updatePersona = vi.fn(async (_personaId, update) => {
    detail = { ...detail, ...update };
    return detail;
  });
  const { container } = render(<App client={fixtureClient({ getPersona, updatePersona })} />);
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
});

it('deletes a persona from the skull action beside upload after confirmation', async () => {
  const user = userEvent.setup();
  const deletePersona = vi.fn(async () => undefined);
  render(<App client={fixtureClient({ deletePersona })} />);
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
  expect(screen.getByRole('button', { name: 'Rename Guide' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Settings' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Replace character definition from file' }))
    .toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Edit character definition' }))
    .toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Delete Guide' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Characters' }));
  expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
});

it('loads a character editor from the unexpanded editable source', async () => {
  const user = userEvent.setup();
  const getCharacter = vi.fn(async () => ({
    ...characterDetailFixture,
    character_markdown: '# Expanded Guide',
    editable_markdown: '# Source\n\n$${character.display_name}',
  }));
  render(<App client={fixtureClient({ getCharacter })} />);
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', { name: /Guide/ }));
  await user.click(await screen.findByRole('button', { name: 'Edit character definition' }));

  const editor = screen.getByRole('textbox', { name: 'Edit character definition text' });
  await waitFor(() => expect(editor).toHaveValue(
    '# Source\n\n$${character.display_name}',
  ));
});

it('keeps a character on screen and shows the server message when deletion is refused', async () => {
  const user = userEvent.setup();
  const deleteCharacter = vi.fn(async () => {
    throw new ChaError(
      409,
      'bad_request',
      'This character is still used by one or more forums.',
    );
  });
  render(<App client={fixtureClient({ deleteCharacter })} />);
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
  await user.click(await screen.findByRole('button', { name: 'Characters' }));
  await user.click(screen.getByRole('button', {
    name: 'New characterEnter a name to begin',
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
  expect(screen.getByText('This character has no definition yet.'))
    .toBeInTheDocument();
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

it('replaces character Markdown from the topbar file action', async () => {
  let detail = characterDetailFixture;
  const getCharacter = vi.fn(async () => detail);
  const updateCharacterDefinition = vi.fn(async (_characterId, update) => {
    detail = { ...detail, ...update };
    return detail;
  });
  const { container } = render(<App client={fixtureClient({
    getCharacter,
    updateCharacterDefinition,
  })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));

  await screen.findByRole('button', { name: 'Replace character definition from file' });
  const file = new File(['# Replacement\n\nFresh voice.'], 'CHARACTER.md', {
    type: 'text/markdown',
  });
  Object.defineProperty(file, 'text', {
    value: vi.fn(async () => '# Replacement\n\nFresh voice.'),
  });
  fireEvent.change(container.querySelector(
    '.cha-definition-topbar-action input[type="file"]',
  ) as HTMLInputElement, { target: { files: [file] } });

  await waitFor(() => expect(updateCharacterDefinition).toHaveBeenCalledWith('guide', {
    character_markdown: '# Replacement\n\nFresh voice.',
  }));
  expect(await screen.findByRole('heading', { name: 'Replacement' })).toBeInTheDocument();
  expect(screen.getByText('Fresh voice.')).toBeInTheDocument();
});

it('retries a failed character-detail request without exposing implementation details', async () => {
  const getCharacter = vi.fn()
    .mockRejectedValueOnce(new ChaUnavailableError())
    .mockResolvedValueOnce({
      ...characterDetailFixture,
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

it('creates a forum with its selected persona and opens the new forum', async () => {
  const user = userEvent.setup();
  const createForum = vi.fn(async ({ display_name, persona_id }) => ({
    ...forumDetailFixture,
    id: 'forum_1',
    display_name,
    default_character_id: 'assistant',
    default_persona_id: persona_id,
    default_persona_display_name: 'Reader',
    members: [bootstrapFixture.characters[0]],
    forum_markdown: '',
  }));
  render(<App client={fixtureClient({ createForum })} />);

  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', {
    name: 'New forumEnter a name to begin',
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
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));

  const forums = within(screen.getByLabelText('Forums navigation'));
  expect(forums.getByRole('button', { name: 'The LobbyWhere the big questions get argued out' }))
    .toBeInTheDocument();
  // A forum configuring none still names its cast rather than showing a bare row.
  expect(forums.getByRole('button', { name: 'EntranceAssistant' })).toBeInTheDocument();
});

it('names the forum above its sessions and opens its FORUM.md description', async () => {
  const getForum = vi.fn(async () => forumDetailFixture);
  render(<App client={fixtureClient({ getForum })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));

  // Sessions is titled Sessions, so the header is the only thing naming the
  // forum whose list this is.
  const sessions = within(screen.getByLabelText('Forum sessions navigation'));
  const header = sessions.getByRole('button', { name: 'The LobbyGuide' });
  fireEvent.click(header);

  expect(await screen.findByRole('heading', { name: 'House rules' })).toBeInTheDocument();
  expect(screen.getByText('deliberate').tagName).toBe('STRONG');
  expect(getForum).toHaveBeenCalledWith('lobby');
  // The topbar names the forum from bootstrap while its description loads, and
  // the cast is shown without a request of its own.
  expect(screen.getByRole('button', { name: 'Rename The Lobby' })).toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Replace forum definition from file' }))
    .toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Edit forum description' }))
    .toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Delete The Lobby' })).toBeInTheDocument();
  expect(screen.getByText('Guide · speaking as Reader')).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Forum detail navigation'))
    .getByRole('button', { name: 'Sessions' }));
  expect(screen.getByRole('heading', { name: 'Sessions' })).toBeInTheDocument();
});

it('deletes a forum and removes its sessions from navigation after confirmation', async () => {
  const user = userEvent.setup();
  const deleteForum = vi.fn(async () => undefined);
  render(<App client={fixtureClient({ deleteForum })} />);
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

it('replaces forum Markdown from the topbar file action', async () => {
  let detail = forumDetailFixture;
  const getForum = vi.fn(async () => detail);
  const updateForum = vi.fn(async (_forumId, update) => {
    detail = { ...detail, ...update };
    return detail;
  });
  const { container } = render(<App client={fixtureClient({ getForum, updateForum })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  fireEvent.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  await screen.findByRole('button', { name: 'Replace forum definition from file' });
  const file = new File(['# New forum\n\nFresh rules.'], 'FORUM.md', {
    type: 'text/markdown',
  });
  Object.defineProperty(file, 'text', {
    value: vi.fn(async () => '# New forum\n\nFresh rules.'),
  });
  fireEvent.change(container.querySelector(
    '.cha-definition-topbar-action input[type="file"]',
  ) as HTMLInputElement, { target: { files: [file] } });

  await waitFor(() => expect(updateForum).toHaveBeenCalledWith('lobby', {
    forum_markdown: '# New forum\n\nFresh rules.',
  }));
  expect(await screen.findByRole('heading', { name: 'New forum' })).toBeInTheDocument();
  expect(screen.getByText('Fresh rules.')).toBeInTheDocument();
});

it('reports a forum with no FORUM.md rather than an empty screen', async () => {
  const getForum = vi.fn(async () => ({ ...forumDetailFixture, forum_markdown: '' }));
  render(<App client={fixtureClient({ getForum })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  fireEvent.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  expect(await screen.findByText('This forum has no FORUM.md description.'))
    .toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('retries a failed forum-detail request without exposing implementation details', async () => {
  const getForum = vi.fn()
    .mockRejectedValueOnce(new ChaUnavailableError())
    .mockResolvedValueOnce(forumDetailFixture);
  render(<App client={fixtureClient({ getForum })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  fireEvent.click(within(screen.getByLabelText('Forum sessions navigation'))
    .getByRole('button', { name: 'The LobbyGuide' }));

  expect(await screen.findByRole('alert')).toHaveTextContent('application API is unavailable');
  fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
  expect(await screen.findByRole('heading', { name: 'House rules' })).toBeInTheDocument();
  expect(getForum).toHaveBeenCalledTimes(2);
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

  await user.click(await screen.findByRole('button', { name: 'Forums' }));
  await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  await user.click(await screen.findByRole('button', { name: 'New sessionEnter a name to begin' }));

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

  await user.click(screen.getByRole('button', { name: 'Personas' }));
  await user.click(screen.getByRole('button', { name: 'New personaEnter a name to begin' }));
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
  render(<App
    client={fixtureClient({ downloadSession })}
    connectSessionEvents={inertSessionEvents}
  />);

  await screen.findByLabelText('Actions for Planning');
  await user.click(screen.getByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Download' }));

  await waitFor(() => expect(downloadSession).toHaveBeenCalledWith('lobby', 'planning'));
  expect(picker).toHaveBeenCalledWith(expect.objectContaining({ suggestedName: 'Planning.md' }));
  expect(write).toHaveBeenCalledWith('# Planning\n');
  Reflect.deleteProperty(window, 'showSaveFilePicker');
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
  await waitFor(() => expect(window.location.pathname).toBe('/s/lobby/planning/'));

  await user.click(screen.getByLabelText('Actions for Planning'));
  await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));

  await waitFor(() => expect(deleteSession).toHaveBeenCalledWith('lobby', 'planning'));
  await waitFor(() => expect(window.location.pathname).toBe('/'));
  await waitFor(() => expect(
    events.connections.filter(({ key }) => key === 'entrance/welcome'),
  ).toHaveLength(2));
  const welcome = events.connections.map(({ key }) => key).lastIndexOf('entrance/welcome');
  act(() => events.handlers[welcome].onSnapshot(snapshotFixture));
  await waitFor(() => expect(screen.getByLabelText('Current chat context'))
    .toHaveTextContent('Entrance'));
  expect(screen.queryByLabelText('Actions for Planning')).not.toBeInTheDocument();
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
        throw new ChaError(500, 'internal_error', 'Planning could not be opened.');
      }
      return { forum_id: forumId, session_id: sessionId };
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);

  expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('could not be opened');
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

it.each(['session_stopping', 'session_open_timeout'] as const)(
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
      throw new ChaError(409, 'session_stopping', 'Planning is still stopping.');
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);
  await openPlanningFromTheLobby();

  expect(await screen.findByRole('alert')).toHaveTextContent('still stopping');
  expect(sessionRow(/^Planning/)).toBeEnabled();
  expect(window.location.pathname).toBe('/');
});

// Recent is reachable from every screen, so the screen the user happens to be
// looking at is where the failure has to appear. Reporting it only on the two
// screens that start an open themselves loses it silently.
it('reports a Recent open failure on whichever navigation screen is showing', async () => {
  const client = fixtureClient({
    openSession: async (forumId, sessionId) => {
      if (sessionId === 'planning') {
        throw new ChaError(409, 'session_stopping', 'Planning is still stopping.');
      }
      return { forum_id: forumId, session_id: sessionId };
    },
  });
  render(<App client={client} connectSessionEvents={inertSessionEvents} />);
  await screen.findByLabelText('Current chat context');

  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  const recent = within(screen.getByLabelText('Recent sessions'));
  fireEvent.click(recent.getByRole('button', { name: /^Planning/ }));

  expect(await screen.findByRole('alert')).toHaveTextContent('Planning is still stopping.');
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

  expect(screen.queryByRole('button', { name: 'OpenAI' })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
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
  fireEvent.click(await screen.findByRole('button', { name: 'Settings' }));
  expect(await screen.findByText('Loading ChatGPT connection…')).toBeInTheDocument();

  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  expect(await screen.findByLabelText('Characters navigation')).toBeInTheDocument();
  await act(async () => { finish(waitingAuth); });

  expect(screen.getByLabelText('Characters navigation')).toBeInTheDocument();
  expect(screen.queryByText('TEST-ONLY')).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Connect ChatGPT' })).not.toBeInTheDocument();
});

it('contains the main navigation and a Settings gear instead of an OpenAI row', async () => {
  renderAt(1280);
  expect(await screen.findByRole('combobox', { name: 'Choose message target' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Settings' })).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'OpenAI' })).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Personas' })).toBeInTheDocument();
});

it('shows the settings row only after a writable character detail loads', async () => {
  let finish!: (detail: CharacterDetail) => void;
  const getCharacter = vi.fn(() => new Promise<CharacterDetail>((resolve) => {
    finish = resolve;
  }));
  render(<App client={fixtureClient({ getCharacter })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));

  expect(await screen.findByText('Loading character…')).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
  expect(screen.getByRole('heading', { name: 'Guide' })).toBeInTheDocument();

  await act(async () => { finish(characterDetailFixture); });
  expect(await within(screen.getByLabelText('Character detail navigation'))
    .findByRole('button', { name: 'Settings' })).toBeInTheDocument();
});

it('omits the settings row for a character that is not writable', async () => {
  render(<App client={fixtureClient({
    getCharacter: async () => ({ ...characterDetailFixture, writable: false }),
  })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  expect(await screen.findByRole('heading', { name: 'Guide dossier' })).toBeInTheDocument();
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
    // The built-in Assistant is the character this feature must never offer.
    return Promise.resolve({
      ...characterDetailFixture, id: 'assistant', display_name: 'Assistant', writable: false,
    });
  });
  render(<App client={fixtureClient({ getCharacter })} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));

  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  fireEvent.click(document.querySelector('.cha-back-row') as HTMLElement);
  fireEvent.click(screen.getByRole('button', { name: /Assistant/ }));
  expect(await screen.findByRole('heading', { name: 'Guide dossier' })).toBeInTheDocument();

  await act(async () => { finishGuide({ ...characterDetailFixture, writable: true }); });

  expect(screen.getByRole('heading', { name: 'Assistant' })).toBeInTheDocument();
  expect(within(screen.getByLabelText('Character detail navigation'))
    .queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
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
  fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  fireEvent.click(await within(screen.getByLabelText('Character detail navigation'))
    .findByRole('button', { name: 'Settings' }));
  expect(await screen.findByRole('heading', { name: 'Settings' })).toBeInTheDocument();
  return planning;
}

it('reopens a live conversation after a settings save without leaving the settings screen', async () => {
  const user = userEvent.setup();
  const events = drivableSessionEvents();
  const previous = planningVoiceSnapshot(serifItalicVoice);
  const next = planningVoiceSnapshot(monoLargeVoice);
  let lobbySnapshots = 0;
  const getSessionSnapshot = vi.fn(async (forumId: string) => {
    if (forumId !== 'lobby') return snapshotFixture;
    lobbySnapshots += 1;
    if (lobbySnapshots === 1) return previous;
    if (lobbySnapshots === 2) {
      throw new ChaError(409, 'session_not_live', 'Session is not live.');
    }
    return next;
  });
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
    web_search: null,
  }));

  act(() => events.handlers[planning].onSnapshot({
    ...previous,
    lifecycle: 'stopping',
    shutdown_reason: 'reloading',
  }));
  expect(screen.getByRole('heading', { name: 'Settings' })).toBeInTheDocument();
  act(() => events.handlers[planning].onError({ kind: 'stream_failure' }));

  await waitFor(() => expect(
    events.connections.filter(({ key }) => key === 'lobby/planning'),
  ).toHaveLength(2));
  const reattached = events.connections.findIndex((connection, index) => (
    index > planning && connection.key === 'lobby/planning'
  ));
  act(() => events.handlers[reattached].onSnapshot(next));

  expect(document.querySelector('main')).toHaveAttribute('data-view', 'character-settings');
  expect(openSession.mock.calls.filter(([, sessionId]) => sessionId === 'planning')).toHaveLength(2);

  fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(screen.getByText('A considered answer')).toHaveClass(
    'cha-message-text', 'cha-font-mono', 'cha-scale-large',
  );
});

it('retries a settings reload when the first reopen meets a stopping session', async () => {
  const user = userEvent.setup();
  const events = drivableSessionEvents();
  const previous = planningVoiceSnapshot(serifItalicVoice);
  const next = planningVoiceSnapshot(monoLargeVoice);
  let planningOpens = 0;
  const openSession = vi.fn(async (forumId: string, sessionId: string) => {
    if (sessionId === 'planning') {
      planningOpens += 1;
      if (planningOpens === 2) {
        throw new ChaError(409, 'session_stopping', 'Session is stopping.');
      }
    }
    return { forum_id: forumId, session_id: sessionId };
  });
  let lobbySnapshots = 0;
  const getSessionSnapshot = vi.fn(async (forumId: string) => {
    if (forumId !== 'lobby') return snapshotFixture;
    lobbySnapshots += 1;
    if (lobbySnapshots === 1) return previous;
    if (lobbySnapshots === 2 || lobbySnapshots === 3) {
      throw new ChaError(409, 'session_not_live', 'Session is not live.');
    }
    return next;
  });
  render(<App
    client={storedPlanningClient({
      getSessionSnapshot,
      openSession,
      updateCharacter: async () => ({ ...characterDetailFixture, style: 'mono-large' }),
    })}
    connectSessionEvents={events.connect}
    retryDelays={[0, 0]}
  />);

  const planning = await openGuideSettingsFromPlanning(events, previous);
  await user.selectOptions(await screen.findByLabelText('Style'), 'mono-large');
  await user.click(screen.getByRole('button', { name: 'Save' }));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled());

  act(() => events.handlers[planning].onSnapshot({
    ...previous,
    lifecycle: 'stopping',
    shutdown_reason: 'reloading',
  }));
  act(() => events.handlers[planning].onError({ kind: 'stream_failure' }));

  await waitFor(() => expect(
    events.connections.filter(({ key }) => key === 'lobby/planning'),
  ).toHaveLength(2));
  expect(openSession.mock.calls.filter(([, sessionId]) => sessionId === 'planning')).toHaveLength(3);
  expect(document.querySelector('main')).toHaveAttribute('data-view', 'character-settings');
});

it('reloads after switching vaults', async () => {
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

  await user.selectOptions(await screen.findByLabelText('Vault'), 'Projects');
  await waitFor(() => expect(switchVault).toHaveBeenCalledWith('Projects'));
  expect(reload).toHaveBeenCalledOnce();
});
