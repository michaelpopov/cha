import type {
  Bootstrap,
  ChaClient,
  CharacterDetail,
  SessionSnapshot,
} from '../api/client';

export const bootstrapFixture: Bootstrap = {
  initial_persona_id: 'guest',
  initial_forum_id: 'entrance',
  initial_session_id: 'welcome',
  personas: [
    { id: 'guest', display_name: 'Guest', description: 'The built-in visitor persona' },
    { id: 'reader', display_name: 'Reader', description: 'Thoughtful, curious, and concise' },
  ],
  characters: [
    { id: 'assistant', display_name: 'Assistant', description: 'CHA application guide' },
    { id: 'guide', display_name: 'Guide', description: 'A deterministic test character' },
  ],
  forums: [
    {
      id: 'entrance',
      display_name: 'Entrance',
      default_character_id: 'assistant',
      members: [{ id: 'assistant', display_name: 'Assistant', description: 'CHA application guide' }],
    },
    {
      id: 'lobby',
      display_name: 'The Lobby',
      default_character_id: 'guide',
      members: [{ id: 'guide', display_name: 'Guide', description: 'A deterministic test character' }],
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
    agent_id: '',
    agent_name: '',
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
