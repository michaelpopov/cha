import { describe, expect, it } from 'vitest';

import { bootstrapFixture } from '../test/fixtures';
import { validateBootstrap } from './bootstrap';

describe('validateBootstrap', () => {
  it('accepts the documented response', () => {
    expect(validateBootstrap(structuredClone(bootstrapFixture)))
      .toEqual(bootstrapFixture);
  });

  it('accepts a Recent list that omits the initial session', () => {
    const response = structuredClone(bootstrapFixture);
    response.recent_sessions = response.recent_sessions.filter(
      ({ session_id }) => session_id !== response.initial_session_id,
    );

    const validated = validateBootstrap(response);

    expect(validated.initial_session_id).toBe(bootstrapFixture.initial_session_id);
    expect(validated.recent_sessions).not.toContainEqual(
      expect.objectContaining({ session_id: bootstrapFixture.initial_session_id }),
    );
  });

  it('accepts an empty Recent list', () => {
    const response = structuredClone(bootstrapFixture);
    response.recent_sessions = [];

    expect(validateBootstrap(response).recent_sessions).toEqual([]);
  });

  it.each([
    ['initial_persona_id', (value: typeof bootstrapFixture) => {
      value.initial_persona_id = 'absent';
    }],
    ['initial_forum_id', (value: typeof bootstrapFixture) => {
      value.initial_forum_id = 'absent';
    }],
    ['default character', (value: typeof bootstrapFixture) => {
      value.characters = value.characters.filter(({ id }) => id !== 'assistant');
    }],
    ['default persona', (value: typeof bootstrapFixture) => {
      value.personas = value.personas.filter(({ id }) => id !== 'guest');
    }],
  ])('rejects a response whose %s cannot be resolved', (_name, corrupt) => {
    const response = structuredClone(bootstrapFixture);
    corrupt(response);

    expect(() => validateBootstrap(response)).toThrow(TypeError);
  });

  it('rejects a malformed recent session', () => {
    const response = structuredClone(bootstrapFixture) as unknown as {
      recent_sessions: unknown[];
    };
    response.recent_sessions = [{ forum_id: 'lobby', session_id: 'planning' }];

    expect(() => validateBootstrap(response)).toThrow(TypeError);
  });
});
