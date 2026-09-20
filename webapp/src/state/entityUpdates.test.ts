import { describe, expect, it } from 'vitest';

import {
  bootstrapFixture,
  characterDetailFixture,
  forumDetailFixture,
  personaDetailFixture,
  snapshotFixture,
} from '../test/fixtures';
import { appReducer, initialAppState, type AppState } from './view';

function stateWithConversation(forumIndex: number | null): AppState {
  const state = appReducer(initialAppState, {
    type: 'bootstrap-loaded', bootstrap: structuredClone(bootstrapFixture),
  });
  if (forumIndex === null) return state;
  const forum = state.bootstrap!.forums[forumIndex];
  return appReducer(state, {
    type: 'conversation-opened',
    snapshot: {
      ...snapshotFixture,
      forum,
      characters: forum.members,
      generation: {
        ...snapshotFixture.generation,
        character_id: forum.default_character_id,
        character_display_name: forum.members[0].display_name,
      },
      transcript: [{
        id: 1,
        kind: 'character',
        participant_id: 'guide',
        display_name: 'Guide',
        addressed_to: 'reader',
        addressed_to_name: 'Reader',
        text: 'A saved reply.',
        status: 'complete',
        created_at: null,
      }],
    },
  });
}

describe.each([
  { context: 'a matching conversation', forumIndex: 1 },
  { context: 'an unrelated conversation', forumIndex: 0 },
  { context: 'no loaded conversation', forumIndex: null },
])('entity edits with $context', ({ forumIndex }) => {
  it('propagates a persona rename without changing unrelated entities or history', () => {
    const state = stateWithConversation(forumIndex);
    const before = structuredClone(state);
    const updated = appReducer(state, {
      type: 'persona-updated',
      persona: { ...personaDetailFixture, display_name: 'Editor' },
    });

    expect(updated.bootstrap!.personas[1].display_name).toBe('Editor');
    expect(updated.bootstrap!.forums[1].default_persona_display_name).toBe('Editor');
    expect(updated.bootstrap!.personas[0]).toEqual(before.bootstrap!.personas[0]);
    expect(updated.bootstrap!.forums[0]).toEqual(before.bootstrap!.forums[0]);
    expect(updated.bootstrap!.characters).toEqual(before.bootstrap!.characters);
    if (forumIndex === 1) {
      expect(updated.sessionSnapshot!.forum.default_persona_display_name).toBe('Editor');
      expect(updated.sessionSnapshot!.transcript).toEqual(before.sessionSnapshot!.transcript);
    } else {
      expect(updated.sessionSnapshot).toEqual(before.sessionSnapshot);
    }
    expect(state).toEqual(before);
  });

  it('propagates a character rename to members and generation without rewriting history', () => {
    const state = stateWithConversation(forumIndex);
    const before = structuredClone(state);
    const updated = appReducer(state, {
      type: 'character-updated',
      character: { ...characterDetailFixture, display_name: 'Mentor' },
    });

    expect(updated.bootstrap!.characters[1].display_name).toBe('Mentor');
    expect(updated.bootstrap!.forums[1].members[0].display_name).toBe('Mentor');
    expect(updated.bootstrap!.characters[0]).toEqual(before.bootstrap!.characters[0]);
    expect(updated.bootstrap!.forums[0]).toEqual(before.bootstrap!.forums[0]);
    expect(updated.bootstrap!.personas).toEqual(before.bootstrap!.personas);
    if (forumIndex === 1) {
      expect(updated.sessionSnapshot!.characters[0].display_name).toBe('Mentor');
      expect(updated.sessionSnapshot!.forum.members[0].display_name).toBe('Mentor');
      expect(updated.sessionSnapshot!.generation.character_display_name).toBe('Mentor');
      expect(updated.sessionSnapshot!.transcript).toEqual(before.sessionSnapshot!.transcript);
    } else {
      expect(updated.sessionSnapshot).toEqual(before.sessionSnapshot);
    }
    expect(state).toEqual(before);
  });

  it('propagates a forum rename while preserving other forums and the conversation', () => {
    const state = stateWithConversation(forumIndex);
    const before = structuredClone(state);
    const updated = appReducer(state, {
      type: 'forum-updated',
      forum: { ...forumDetailFixture, display_name: 'Brain Trust' },
    });

    expect(updated.bootstrap!.forums[1].display_name).toBe('Brain Trust');
    expect(updated.bootstrap!.forums[0]).toEqual(before.bootstrap!.forums[0]);
    expect(updated.bootstrap!.characters).toEqual(before.bootstrap!.characters);
    expect(updated.bootstrap!.personas).toEqual(before.bootstrap!.personas);
    expect(updated.activeConversation).toEqual(before.activeConversation);
    if (forumIndex === 1) {
      expect(updated.sessionSnapshot!.forum.display_name).toBe('Brain Trust');
      expect(updated.sessionSnapshot!.transcript).toEqual(before.sessionSnapshot!.transcript);
    } else {
      expect(updated.sessionSnapshot).toEqual(before.sessionSnapshot);
    }
    expect(state).toEqual(before);
  });
});
