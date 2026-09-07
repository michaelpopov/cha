import type {
  Bootstrap,
  ChaClient,
  CharacterAppearance,
  CharacterDetail,
  ForumDetail,
  OpenAiAuth,
  PersonaDetail,
  SessionSnapshot,
} from '../api/client';

// A character that configures nothing still carries an appearance on the wire.
export const plainVoice: CharacterAppearance = {
  font: 'sans', style: 'normal', weight: 'normal', size: 'normal', text_color: 'normal',
};

export const bootstrapFixture: Bootstrap = {
  vault_name: 'Personal',
  vaults: ['Personal', 'Projects'],
  initial_forum_id: 'entrance',
  initial_session_id: 'welcome',
  personas: [
    { id: 'guest', display_name: 'Guest', description: 'The built-in visitor persona' },
    { id: 'reader', display_name: 'Reader', description: 'Thoughtful, curious, and concise' },
  ],
  characters: [
    { id: 'assistant', display_name: 'Assistant', description: 'CHA application guide', appearance: plainVoice },
    { id: 'guide', display_name: 'Guide', description: 'A deterministic test character', appearance: plainVoice },
  ],
  forums: [
    {
      id: 'entrance',
      display_name: 'Entrance',
      default_character_id: 'assistant',
      default_persona_id: 'guest',
      default_persona_display_name: 'Guest',
      members: [{ id: 'assistant', display_name: 'Assistant', description: 'CHA application guide', appearance: plainVoice }],
    },
    {
      id: 'lobby',
      display_name: 'The Lobby',
      default_character_id: 'guide',
      // Deliberately not Entrance's persona: the two forums differ so that a
      // screen reading the wrong one is visible in the assertions.
      default_persona_id: 'reader',
      default_persona_display_name: 'Reader',
      members: [{ id: 'guide', display_name: 'Guide', description: 'A deterministic test character', appearance: plainVoice }],
    },
  ],
  recent_sessions: [
    {
      forum_id: 'entrance',
      session_id: 'welcome',
      session_label: 'Welcome',
      updated_at: 2,
    },
    {
      forum_id: 'lobby',
      session_id: 'planning',
      session_label: 'Planning',
      updated_at: 1,
    },
  ],
};

export const serifItalicVoice: CharacterAppearance = {
  font: 'serif', style: 'italic', weight: 'normal', size: 'normal', text_color: 'normal',
};

export const monoLargeVoice: CharacterAppearance = {
  font: 'mono', style: 'normal', weight: 'normal', size: 'large', text_color: 'normal',
};

export const characterDetailFixture: CharacterDetail = {
  id: 'guide',
  display_name: 'Guide',
  description: 'A deterministic test character',
  appearance: serifItalicVoice,
  character_markdown: '# Guide dossier\n\nA **careful** guide.\n\n- Listen\n- Respond',
  editable_markdown: '# Guide dossier\n\nA **careful** guide.\n\n- Listen\n- Respond',
  provider: 'terra',
  style: 'serif-italic',
  reasoning_effort: null,
  web_search: null,
  available_providers: [
    { id: 'sol-high', label: 'Sol high' },
    { id: 'terra', label: 'Terra' },
  ],
  available_styles: [
    { id: 'mono-large', label: 'Mono large', appearance: monoLargeVoice },
    { id: 'serif-italic', label: 'Serif italic', appearance: serifItalicVoice },
  ],
  writable: true,
};

// The Markdown heading deliberately differs from the display name the topbar
// shows, so a test asserting the rendered description cannot be satisfied by
// the title this screen already has from bootstrap.
export const personaDetailFixture: PersonaDetail = {
  id: 'reader',
  display_name: 'Reader',
  description: 'Thoughtful, curious, and concise',
  persona_markdown: '# Reader notes\n\nA **thoughtful** reader.',
  writable: true,
};

// As with the persona fixture, the Markdown heading differs from the display
// name the topbar already shows, so a test asserting the rendered FORUM.md
// cannot pass on the title alone.
export const forumDetailFixture: ForumDetail = {
  id: 'lobby',
  display_name: 'The Lobby',
  default_character_id: 'guide',
  default_persona_id: 'reader',
  default_persona_display_name: 'Reader',
  members: [{ id: 'guide', display_name: 'Guide', description: 'A deterministic test character', appearance: plainVoice }],
  forum_markdown: '# House rules\n\nA **deliberate** place to talk.\n\n- Ask one thing\n- Start a session per question',
  writable: true,
};

export const signedOutAuth: OpenAiAuth = { status: 'signed_out' };

export const waitingAuth: OpenAiAuth = {
  status: 'waiting',
  user_code: 'TEST-ONLY',
  verification_url: 'https://auth.openai.com/codex/device',
  attempt_expires_at: 1700000900,
  next_poll_delay_ms: 1000,
};

export const connectedAuth: OpenAiAuth = { status: 'connected' };

export const snapshotFixture: SessionSnapshot = {
  forum: bootstrapFixture.forums[0],
  session_id: 'welcome',
  session_label: 'Welcome',
  characters: [bootstrapFixture.characters[0]],
  default_character_id: 'assistant',
  transcript: [],
  generation: {
    active: false,
    character_id: '',
    character_display_name: '',
    phase: 'waiting',
    reasoning_text: '',
  },
  lifecycle: 'running',
};

export function fixtureClient(overrides: Partial<ChaClient> = {}): ChaClient {
  return {
    getBootstrap: async () => bootstrapFixture,
    getCharacter: async () => characterDetailFixture,
    createCharacter: async ({ display_name, description }) => ({
      ...characterDetailFixture,
      id: 'character_1',
      display_name,
      description,
      character_markdown: '',
      provider: null,
      style: null,
    }),
    updateCharacter: async (_characterId, settings) => ({
      ...characterDetailFixture,
      ...settings,
    }),
    updateCharacterDefinition: async (_characterId, update) => ({
      ...characterDetailFixture,
      ...update,
    }),
    deleteCharacter: async () => undefined,
    getPersona: async () => personaDetailFixture,
    createPersona: async ({ display_name }) => ({
      id: 'persona_1',
      display_name,
      persona_markdown: '',
      writable: true,
    }),
    updatePersona: async (_personaId, update) => ({
      ...personaDetailFixture,
      ...update,
    }),
    deletePersona: async () => undefined,
    getForum: async () => forumDetailFixture,
    createForum: async ({ display_name, persona_id }) => ({
      ...forumDetailFixture,
      id: 'forum_1',
      display_name,
      default_persona_id: persona_id,
      default_persona_display_name: bootstrapFixture.personas.find(
        ({ id }) => id === persona_id,
      )?.display_name ?? persona_id,
      members: [bootstrapFixture.characters[0]],
      default_character_id: bootstrapFixture.characters[0].id,
      forum_markdown: '',
    }),
    updateForum: async (_forumId, update) => ({
      ...forumDetailFixture,
      ...update,
    }),
    deleteForum: async () => undefined,
    updateForumMembers: async (_forumId, update) => ({
      ...forumDetailFixture,
      members: bootstrapFixture.characters.filter(({ id }) => (
        update.character_ids.includes(id)
      )),
    }),
    listSessions: async () => [],
    createSession: async (_forumId, label) => ({ id: 'created', label }),
    renameSession: async (_forumId, sessionId, label) => ({ id: sessionId, label }),
    deleteSession: async () => undefined,
    downloadSession: async () => '# Session\n',
    openSession: async (forumId, sessionId) => ({ forum_id: forumId, session_id: sessionId }),
    getSessionSnapshot: async () => snapshotFixture,
    submitInput: async () => ({ clear_input: true }),
    stopGeneration: async () => ({ clear_input: false }),
    setDefaultCharacter: async () => ({ clear_input: false }),
    getOpenAiAuth: async () => signedOutAuth,
    startOpenAiAuth: async () => signedOutAuth,
    pollOpenAiAuth: async () => signedOutAuth,
    disconnectOpenAiAuth: async () => signedOutAuth,
    listProviders: async () => [],
    createProvider: async () => { throw new Error('No provider fixture'); },
    getProvider: async () => { throw new Error('No provider fixture'); },
    updateProvider: async () => { throw new Error('No provider fixture'); },
    deleteProvider: async () => undefined,
    listStyles: async () => [],
    createStyle: async () => { throw new Error('No style fixture'); },
    updateStyle: async () => { throw new Error('No style fixture'); },
    deleteStyle: async () => undefined,
    listApiKeys: async () => [],
    createApiKey: async ({ display_name }) => ({
      id: 'api_key_1', display_name, has_value: true, used_by: [],
    }),
    renameApiKey: async (id, display_name) => ({
      id, display_name, has_value: true, used_by: [],
    }),
    replaceApiKeyValue: async (id) => ({
      id, display_name: 'API key', has_value: true, used_by: [],
    }),
    deleteApiKey: async () => undefined,
    switchVault: async () => undefined,
    ...overrides,
  };
}
