import { describe, expect, it } from 'vitest';

import { bootstrapFixture, snapshotFixture } from '../test/fixtures';
import {
  appReducer,
  initialAppState,
  navigationTitle,
  type AppAction,
  type AppState,
} from './view';

function readyState(): AppState {
  return appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap: bootstrapFixture });
}

describe('application navigation reducer', () => {
  it('notifies an already-uncached active conversation when its session cache is cleared', () => {
    const state = { ...readyState(), sessionSnapshot: snapshotFixture };
    const clear: AppAction = { type: 'session-audio-cache', forumId: 'entrance', sessionId: 'welcome', cached: false };
    const cleared = appReducer(state, clear);
    expect(cleared.audioCacheClearCount).toBe(state.audioCacheClearCount + 1);
    expect(appReducer(cleared, clear).audioCacheClearCount).toBe(state.audioCacheClearCount + 2);
    expect(appReducer(cleared, { ...clear, sessionId: 'other' }).audioCacheClearCount).toBe(cleared.audioCacheClearCount);
    expect(appReducer(cleared, { ...clear, entryId: 2 }).audioCacheClearCount).toBe(cleared.audioCacheClearCount);
  });
  it('preserves the active conversation while navigating between forum views', () => {
    let state = readyState();
    state = appReducer(state, { type: 'toggle-sidebar' });
    expect(state.sidebarOpen).toBe(false);

    const conversation = state.activeConversation;
    const actions: AppAction[] = [
      { type: 'show-characters' },
      { type: 'show-new-character' },
      { type: 'inspect-character', characterId: 'guide' },
      { type: 'show-characters' },
      { type: 'show-forums' },
      { type: 'select-forum', forumId: 'lobby' },
      { type: 'show-new-session' },
      { type: 'show-settings' },
      { type: 'show-chat' },
    ];

    for (const action of actions) {
      state = appReducer(state, action);
      expect(state.sidebarOpen).toBe(false);
      expect(state.activeConversation).toEqual(conversation);
    }
    expect(state.currentForumId).toBe('lobby');
  });

  it('changes active conversation and default character only from authoritative snapshots', () => {
    let state = readyState();
    expect(state.currentForumId).toBe('entrance');
    expect(state.activeConversation).toEqual({ forumId: 'entrance', sessionId: 'welcome' });
    expect(state.currentDefaultCharacterId).toBe('assistant');
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

  it('replaces snapshots and appends entry text', () => {
    const streamingSnapshot = {
      ...snapshotFixture,
      transcript: [{
        id: 4,
        kind: 'character' as const,
        participant_id: 'assistant',
        display_name: 'Assistant',
        addressed_to: 'guest',
        addressed_to_name: 'Guest',
        text: 'Hello',
        status: 'streaming' as const,
        request_id: 7,
        created_at: null,
      }],
    };
    let state = readyState();
    state = appReducer(state, { type: 'conversation-opened', snapshot: streamingSnapshot });
    state = appReducer(state, {
      type: 'session-append',
      forumId: 'entrance',
      sessionId: 'welcome',
      event: { target: { kind: 'entry', entry_id: 4 }, text: ' there', seq: 0 },
    });
    expect(state.sessionSnapshot?.transcript[0].text).toBe('Hello there');

    const replacement = { ...streamingSnapshot, transcript: [], generation: snapshotFixture.generation };
    state = appReducer(state, { type: 'session-snapshot', snapshot: replacement });
    expect(state.sessionSnapshot).toEqual(replacement);
  });

  it('ignores append events tagged for a different session', () => {
    const snapshot = {
      ...snapshotFixture,
      transcript: [{
        id: 4,
        kind: 'character' as const,
        participant_id: 'assistant',
        display_name: 'Assistant',
        addressed_to: 'guest',
        addressed_to_name: 'Guest',
        text: 'Unchanged',
        status: 'streaming' as const,
        request_id: 7,
        created_at: null,
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
    expect(state.currentForumId).toBe('lobby');
    expect(state.bootstrap?.recent_sessions[0].session_id).toBe('created');

    state = appReducer(state, { type: 'show-initial-conversation' });
    expect(state.activeConversation).toEqual({ forumId: 'entrance', sessionId: 'welcome' });
    expect(state.currentForumId).toBe('entrance');
    expect(state.mainView).toBe('chat');
  });

  it('clears an old vault snapshot while keeping Settings open, then adopts fresh discovery', () => {
    const inSettings = {
      ...readyState(),
      mainView: 'settings-vault' as const,
      sessionSnapshot: snapshotFixture,
    };
    const cleared = appReducer(inSettings, { type: 'vault-context-reset' });
    expect(cleared.mainView).toBe('settings-vault');
    expect(cleared.sessionSnapshot).toBeNull();
    expect(cleared.activeConversation).toBeNull();

    const refreshed = { ...bootstrapFixture, initial_session_id: 'replacement' };
    const restored = appReducer(cleared, {
      type: 'vault-context-refreshed', bootstrap: refreshed,
    });
    expect(restored.mainView).toBe('settings-vault');
    expect(restored.activeConversation).toEqual({
      forumId: 'entrance', sessionId: 'replacement',
    });

    const opened = appReducer(cleared, {
      type: 'conversation-opened',
      snapshot: { ...snapshotFixture, session_id: 'chosen' },
    });
    const preserved = appReducer(opened, {
      type: 'vault-context-refreshed', bootstrap: refreshed,
    });
    expect(preserved.activeConversation?.sessionId).toBe('chosen');
    expect(preserved.sessionSnapshot?.session_id).toBe('chosen');
  });

  it('tracks character-settings availability only for the inspected character', () => {
    let state = readyState();
    state = appReducer(state, { type: 'inspect-character', characterId: 'guide' });
    expect(state.inspectedCharacter.settingsWritable).toBe(false);
    expect(navigationTitle(state)).toBeNull();

    state = appReducer(state, {
      type: 'character-detail-loaded', characterId: 'guide', settingsWritable: true, writable: true,
    });
    expect(state.inspectedCharacter.settingsWritable).toBe(true);

    state = appReducer(state, { type: 'show-character-settings' });
    expect(state.mainView).toBe('character-settings');
    expect(navigationTitle(state)).toBe('Settings');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);

    state = appReducer(state, { type: 'inspect-character', characterId: 'guide' });
    expect(state.mainView).toBe('character-detail');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);

    state = appReducer(state, { type: 'inspect-character', characterId: 'assistant' });
    expect(state.inspectedCharacter.settingsWritable).toBe(false);
    expect(state.inspectedCharacter.id).toBe('assistant');

    state = appReducer(state, {
      type: 'character-detail-loaded', characterId: 'guide', settingsWritable: true, writable: true,
    });
    expect(state.inspectedCharacter.settingsWritable).toBe(false);

    state = appReducer(state, {
      type: 'character-detail-loaded', characterId: 'assistant', settingsWritable: true, writable: false,
    });
    expect(state.inspectedCharacter.settingsWritable).toBe(true);
    expect(state.inspectedCharacter.writable).toBe(false);
  });

  it('opens forum files and ignores a file for another forum', () => {
    let state = appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap: bootstrapFixture });
    state = appReducer(state, { type: 'select-forum', forumId: 'lobby' });
    state = appReducer(state, { type: 'inspect-forum-file', forumId: 'lobby', filename: 'RULES.md' });
    expect(state.mainView).toBe('forum-file');
    expect(state.inspectedForum.file).toBe('RULES.md');
    expect(appReducer(state, { type: 'show-new-forum-file' })).toBe(state);
    state = appReducer(state, { type: 'forum-detail-loaded', forumId: 'lobby', writable: true });
    expect(appReducer(state, { type: 'show-new-forum-file' }).mainView).toBe('new-forum-file');
    state = appReducer(state, { type: 'select-forum', forumId: 'entrance' });
    expect(state.inspectedForum.file).toBeNull();
    expect(appReducer(state, { type: 'inspect-forum-file', forumId: 'lobby', filename: 'NOTES.md' })).toBe(state);
  });

  it('opens character files without losing settings and ignores a file for another character', () => {
    let state = appReducer(readyState(), { type: 'inspect-character', characterId: 'guide' });
    state = appReducer(state, { type: 'character-detail-loaded', characterId: 'guide', settingsWritable: true, writable: true });
    state = appReducer(state, { type: 'inspect-character-file', characterId: 'guide', filename: 'PROFILE.md' });
    expect(state.mainView).toBe('character-file');
    expect(state.inspectedCharacter.file).toBe('PROFILE.md');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);
    state = appReducer(state, { type: 'inspect-character', characterId: 'assistant' });
    expect(state.inspectedCharacter.file).toBeNull();
    expect(appReducer(state, { type: 'inspect-character-file', characterId: 'guide', filename: 'NOTES.md' })).toBe(state);
  });

  it('adds a created draft character and opens its detail', () => {
    const character = {
      ...bootstrapFixture.characters[1],
      markdown_files: ['CHARACTER.md', 'PROFILE.md'],
      character_markdown: '',
      editable_markdown: '',
      provider: null,
      style: null,
      voice_id: null,
      reasoning_effort: null,
      web_search: null,
      available_providers: [],
      available_styles: [],
      available_voices: [],
      settings_writable: true,
      writable: true,
      id: 'character_1',
      display_name: 'Mentor',
      description: 'A thoughtful guide.',
    };
    const state = appReducer(readyState(), { type: 'character-created', character });

    expect(state.mainView).toBe('character-detail');
    expect(state.inspectedCharacter.id).toBe('character_1');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);
    expect(state.bootstrap?.characters).toContainEqual(expect.objectContaining({
      id: 'character_1',
      display_name: 'Mentor',
      description: 'A thoughtful guide.',
    }));
  });

  it('preserves conversation and character-settings state on the Settings page', () => {
    let state = readyState();
    state = appReducer(state, { type: 'inspect-character', characterId: 'guide' });
    state = appReducer(state, {
      type: 'character-detail-loaded', characterId: 'guide', settingsWritable: true, writable: true,
    });
    state = appReducer(state, { type: 'show-character-settings' });
    const conversation = state.activeConversation;

    state = appReducer(state, { type: 'show-settings' });
    expect(state.mainView).toBe('settings');
    expect(navigationTitle(state)).toBe('Settings');
    expect(state.activeConversation).toEqual(conversation);
    expect(state.inspectedCharacter.id).toBe('guide');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);

    state = appReducer(state, { type: 'show-chat' });
    expect(state.mainView).toBe('chat');
    expect(state.activeConversation).toEqual(conversation);
    expect(state.inspectedCharacter.id).toBe('guide');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);
  });

  it('remembers each inspection independently and ignores a superseded detail reply', () => {
    let state = appReducer(readyState(), {
      type: 'inspect-provider', providerId: 'first', providerName: 'First',
    });
    state = appReducer(state, {
      type: 'provider-detail-loaded', providerId: 'first', providerName: 'First provider', writable: true,
    });
    const provider = state.inspectedProvider;
    state = appReducer(state, {
      type: 'inspect-style', styleId: 'serif', styleName: 'Serif',
    });
    state = appReducer(state, {
      type: 'style-detail-loaded', styleId: 'serif', styleName: 'Serif italic', writable: false,
    });
    const style = state.inspectedStyle;
    state = appReducer(state, { type: 'show-chat' });
    expect(state.inspectedProvider).toEqual(provider);
    expect(state.inspectedStyle).toEqual(style);

    state = appReducer(state, {
      type: 'inspect-provider', providerId: 'first', providerName: 'First provider',
    });
    expect(state.inspectedProvider.writable).toBe(true);
    expect(navigationTitle(state)).toBeNull();
    state = appReducer(state, {
      type: 'inspect-provider', providerId: 'second', providerName: 'Second',
    });
    expect(state.inspectedProvider.writable).toBe(false);
    expect(appReducer(state, {
      type: 'provider-detail-loaded', providerId: 'first', providerName: 'Old reply', writable: true,
    })).toBe(state);
    expect(state.inspectedStyle).toEqual(style);
  });

  it('updates the vault selector without activating a newly created vault', () => {
    let state = readyState();
    state = appReducer(state, {
      type: 'vault-created',
      vault: {
        display_name: 'Archive',
        protected: false,
        data_path: '/data/archive.sqlite3',
        mirror_path: null,
        modify_path: null,
        active: false,
        can_delete: true,
      },
    });
    expect(state.bootstrap?.vault_name).toBe('Personal');
    expect(state.bootstrap?.vaults).toEqual(['Archive', 'Personal', 'Projects']);
    expect(state.mainView).toBe('settings-vault');

    state = appReducer(state, {
      type: 'vault-updated',
      previousName: 'Personal',
      vault: {
        display_name: 'Home',
        protected: false,
        data_path: '/data/personal.sqlite3',
        mirror_path: null,
        modify_path: null,
        active: true,
        can_delete: false,
      },
    });
    expect(state.bootstrap?.vault_name).toBe('Home');
    expect(state.bootstrap?.vaults).toEqual(['Archive', 'Home', 'Projects']);

    state = appReducer(state, { type: 'vault-deleted', vaultName: 'Archive' });
    expect(state.bootstrap?.vaults).toEqual(['Home', 'Projects']);
  });

  it('opens Merge from Vaults and returns to the vault list', () => {
    let state = readyState();
    state = appReducer(state, { type: 'show-settings-vaults' });
    expect(state.mainView).toBe('settings-vaults');
    expect(navigationTitle(state)).toBe('Vaults');

    state = appReducer(state, { type: 'show-settings-merge-vault' });
    expect(state.mainView).toBe('settings-merge-vault');
    expect(navigationTitle(state)).toBe('Merge vault');
    expect(state.bootstrap?.vault_name).toBe('Personal');

    state = appReducer(state, { type: 'show-settings-vaults' });
    expect(state.mainView).toBe('settings-vaults');
    expect(navigationTitle(state)).toBe('Vaults');
  });

  it('keeps the R2 list open after downloading a vault', () => {
    let state = readyState();
    state = appReducer(state, { type: 'show-settings-download-vault' });
    expect(state.mainView).toBe('settings-download-vault');
    expect(navigationTitle(state)).toBe('Download vault');

    state = appReducer(state, {
      type: 'vault-downloaded',
      vault: {
        display_name: 'Archive',
        protected: false,
        data_path: '/data/Archive.sqlite3',
        mirror_path: null,
        modify_path: null,
        active: false,
        can_delete: true,
      },
    });
    expect(state.mainView).toBe('settings-download-vault');
    expect(state.bootstrap?.vault_name).toBe('Personal');
    expect(state.bootstrap?.vaults).toEqual(['Archive', 'Personal', 'Projects']);
  });
});
