import { describe, expect, it } from 'vitest';

import { bootstrapFixture } from '../test/fixtures';
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

  it('changes active conversation and authoritative default only on their actions', () => {
    let state = readyState();
    state = appReducer(state, {
      type: 'activate-conversation',
      forumId: 'lobby',
      sessionId: 'planning',
    });
    expect(state.activeConversation).toEqual({ forumId: 'lobby', sessionId: 'planning' });
    expect(state.mainView).toBe('chat');

    state = appReducer(state, { type: 'set-default-character', characterId: 'guide' });
    expect(state.currentDefaultCharacterId).toBe('guide');
    expect(state.activeConversation).toEqual({ forumId: 'lobby', sessionId: 'planning' });
  });
});
