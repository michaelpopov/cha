import type { Bootstrap } from '../api/client';

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function hasIdentity(
  value: unknown,
): value is { id: string; display_name: string; description?: string } {
  return isRecord(value)
    && typeof value.id === 'string'
    && value.id.length > 0
    && typeof value.display_name === 'string'
    && (value.description === undefined || typeof value.description === 'string');
}

function hasForumIdentity(
  value: unknown,
): value is {
  id: string;
  display_name: string;
  default_character_id: string;
  default_persona_id: string;
  default_persona_display_name: string;
  members: Array<{ id: string; display_name: string; description?: string }>;
} {
  return isRecord(value)
    && typeof value.default_character_id === 'string'
    && value.default_character_id.length > 0
    && typeof value.default_persona_id === 'string'
    && value.default_persona_id.length > 0
    && typeof value.default_persona_display_name === 'string'
    && value.default_persona_display_name.length > 0
    && Array.isArray(value.members)
    && value.members.every(hasIdentity)
    && hasIdentity(value);
}

export function validateBootstrap(value: unknown): Bootstrap {
  if (!isRecord(value)) throw new TypeError('Bootstrap must be an object.');

  const requiredIds = ['initial_forum_id', 'initial_session_id'] as const;
  for (const field of requiredIds) {
    if (typeof value[field] !== 'string' || value[field].length === 0) {
      throw new TypeError(`Bootstrap is missing ${field}.`);
    }
  }

  const { personas, characters, forums, recent_sessions: recentSessions } = value;
  if (!Array.isArray(personas)) throw new TypeError('Bootstrap is missing personas.');
  if (!Array.isArray(characters)) throw new TypeError('Bootstrap is missing characters.');
  if (!Array.isArray(forums)) throw new TypeError('Bootstrap is missing forums.');
  if (!Array.isArray(recentSessions)) throw new TypeError('Bootstrap is missing recent_sessions.');

  if (!personas.every(hasIdentity) || !characters.every(hasIdentity)) {
    throw new TypeError('Bootstrap contains an invalid discovery summary.');
  }
  if (!forums.every(hasForumIdentity)) {
    throw new TypeError('Bootstrap contains an invalid forum summary.');
  }
  if (!recentSessions.every((session) => isRecord(session)
      && typeof session.forum_id === 'string' && session.forum_id.length > 0
      && typeof session.session_id === 'string' && session.session_id.length > 0
      && typeof session.session_label === 'string'
      && typeof session.updated_at === 'number' && Number.isFinite(session.updated_at))) {
    throw new TypeError('Bootstrap contains an invalid recent session.');
  }

  const bootstrap = value as unknown as Bootstrap;
  if (!bootstrap.forums.some(({ id }) => id === bootstrap.initial_forum_id)) {
    throw new TypeError('Bootstrap initial forum is absent from forums.');
  }
  for (const forum of bootstrap.forums) {
    if (!bootstrap.characters.some(({ id }) => id === forum.default_character_id)) {
      throw new TypeError('Bootstrap forum default character is absent from characters.');
    }
    if (!bootstrap.personas.some(({ id }) => id === forum.default_persona_id)) {
      throw new TypeError('Bootstrap forum default persona is absent from personas.');
    }
  }

  // The checks above cover values the UI goes on to resolve against a roster.
  // Whether the initial session also appears in recent_sessions is deliberately
  // not checked: the contract promises only that the list holds every stored
  // session, and the startup conversation is taken from initial_session_id
  // directly, so a Recent list without it stays usable.
  return bootstrap;
}
