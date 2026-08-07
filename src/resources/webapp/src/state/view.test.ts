import { describe, expect, it } from 'vitest';

import { bootstrapFixture, snapshotFixture } from '../test/fixtures';
import { appReducer, initialAppState, type AppAction, type AppState } from './view';

function readyState(): AppState {
  return appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap: bootstrapFixture });
}

describe('application navigation reducer', () => {
  it('selects all three server-provided startup IDs without hard-coding them', () => {
    const state = readyState();
    expect(state.currentPersonaId).toBe('guest');
    expect(state.currentForumId).toBe('entrance');
    expect(state.activeConversation).toEqual({ forumId: 'entrance', sessionId: 'welcome' });
    expect(state.currentDefaultCharacterId).toBe('assistant');
  });

  it('preserves sidebar and conversation state across discovery navigation', () => {
    let state = readyState();
    state = appReducer(state, { type: 'toggle-sidebar' });
    expect(state.sidebarOpen).toBe(false);

    const conversation = state.activeConversation;
    const actions: AppAction[] = [
      { type: 'show-personas' },
      { type: 'select-persona', personaId: 'reader' },
      { type: 'show-characters' },
      { type: 'inspect-character', characterId: 'guide' },
      { type: 'show-characters' },
      { type: 'show-forums' },
      { type: 'select-forum', forumId: 'lobby' },
      { type: 'show-new-session' },
      { type: 'show-chat' },
    ];

    for (const action of actions) {
      state = appReducer(state, action);
      expect(state.sidebarOpen).toBe(false);
      expect(state.activeConversation).toEqual(conversation);
    }
    expect(state.currentPersonaId).toBe('reader');
    expect(state.currentForumId).toBe('lobby');
  });

  it('changes active conversation and default character only from authoritative snapshots', () => {
    let state = readyState();
    const lobbySnapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'planning',
      session_label: 'Planning',
      characters: [bootstrapFixture.characters[1]],
      default_character_id: 'guide',
    };
    state = appReducer(state, { type: 'conversation-opened', snapshot: lobbySnapshot });
    expect(state.activeConversation).toEqual({ forumId: 'lobby', sessionId: 'planning' });
    expect(state.mainView).toBe('chat');
    expect(state.currentDefaultCharacterId).toBe('guide');

    state = appReducer(state, {
      type: 'session-snapshot',
      snapshot: { ...lobbySnapshot, default_character_id: 'assistant' },
    });
    expect(state.currentDefaultCharacterId).toBe('assistant');
    expect(state.activeConversation).toEqual({ forumId: 'lobby', sessionId: 'planning' });
  });

  it('replaces snapshots and appends entry and reasoning stream targets', () => {
    const streamingSnapshot = {
      ...snapshotFixture,
      transcript: [{
        id: 4,
        kind: 'agent' as const,
        participant_id: 'assistant',
        display_name: 'Assistant',
        addressed_to: 'guest',
        addressed_to_name: 'Guest',
        text: 'Hello',
        status: 'streaming' as const,
        request_id: 7,
      }],
      generation: {
        active: true,
        request_id: 7,
        agent_id: 'assistant',
        agent_name: 'Assistant',
        phase: 'reasoning' as const,
        reasoning_text: 'Think',
      },
    };
    let state = readyState();
    state = appReducer(state, { type: 'conversation-opened', snapshot: streamingSnapshot });
    state = appReducer(state, {
      type: 'session-append',
      forumId: 'entrance',
      sessionId: 'welcome',
      event: { target: { kind: 'entry', entry_id: 4 }, text: ' there', seq: 0 },
    });
    state = appReducer(state, {
      type: 'session-append',
      forumId: 'entrance',
      sessionId: 'welcome',
      event: { target: { kind: 'reasoning', request_id: 7 }, text: ' carefully', seq: 1 },
    });

    expect(state.sessionSnapshot?.transcript[0].text).toBe('Hello there');
    expect(state.sessionSnapshot?.generation.reasoning_text).toBe('Think carefully');

    const replacement = { ...streamingSnapshot, transcript: [], generation: snapshotFixture.generation };
    state = appReducer(state, { type: 'session-snapshot', snapshot: replacement });
    expect(state.sessionSnapshot).toEqual(replacement);
  });

  it('ignores append events tagged for a different session', () => {
    const snapshot = {
      ...snapshotFixture,
      transcript: [{
        id: 4,
        kind: 'agent' as const,
        participant_id: 'assistant',
        display_name: 'Assistant',
        addressed_to: 'guest',
        addressed_to_name: 'Guest',
        text: 'Unchanged',
        status: 'streaming' as const,
        request_id: 7,
      }],
    };
    let state = appReducer(readyState(), { type: 'conversation-opened', snapshot });

    state = appReducer(state, {
      type: 'session-append',
      forumId: 'lobby',
      sessionId: 'planning',
      event: { target: { kind: 'entry', entry_id: 4 }, text: ' wrong', seq: 0 },
    });

    expect(state.sessionSnapshot?.transcript[0].text).toBe('Unchanged');
  });

  it('refreshes Recent without resetting current navigation and returns to startup defaults', () => {
    let state = readyState();
    state = appReducer(state, { type: 'select-persona', personaId: 'reader' });
    state = appReducer(state, { type: 'select-forum', forumId: 'lobby' });
    const refreshed = structuredClone(bootstrapFixture);
    refreshed.recent_sessions = [
      {
        forum_id: 'lobby',
        session_id: 'created',
        session_label: 'Created',
        updated_at: 3,
      },
      ...refreshed.recent_sessions,
    ];

    state = appReducer(state, { type: 'bootstrap-refreshed', bootstrap: refreshed });
    expect(state.currentPersonaId).toBe('reader');
    expect(state.currentForumId).toBe('lobby');
    expect(state.bootstrap?.recent_sessions[0].session_id).toBe('created');

    state = appReducer(state, { type: 'show-initial-conversation' });
    expect(state.activeConversation).toEqual({ forumId: 'entrance', sessionId: 'welcome' });
    expect(state.currentForumId).toBe('entrance');
    expect(state.mainView).toBe('chat');
  });
});
