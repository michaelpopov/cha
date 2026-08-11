import type {
  Bootstrap,
  ChaClient,
  CharacterAppearance,
  CharacterDetail,
  SessionSnapshot,
} from '../api/client';

// A character that configures nothing still carries an appearance on the wire.
export const plainVoice: CharacterAppearance = {
  font: 'sans', style: 'normal', weight: 'normal', size: 'normal',
};

export const bootstrapFixture: Bootstrap = {
  initial_persona_id: 'guest',
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
      members: [{ id: 'assistant', display_name: 'Assistant', description: 'CHA application guide', appearance: plainVoice }],
    },
    {
      id: 'lobby',
      display_name: 'The Lobby',
      default_character_id: 'guide',
      default_persona_id: 'guest',
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

export const characterDetailFixture: CharacterDetail = {
  id: 'guide',
  display_name: 'Guide',
  description: 'A deterministic test character',
  appearance: plainVoice,
  character_markdown: '# Guide dossier\n\nA **careful** guide.\n\n- Listen\n- Respond',
};

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
    listSessions: async () => [],
    createSession: async (_forumId, label) => ({ id: 'created', label }),
    openSession: async (forumId, sessionId) => ({ forum_id: forumId, session_id: sessionId }),
    getSessionSnapshot: async () => snapshotFixture,
    submitInput: async () => ({ clear_input: true }),
    stopGeneration: async () => ({ clear_input: false }),
    setDefaultCharacter: async () => ({ clear_input: false }),
    ...overrides,
  };
}
