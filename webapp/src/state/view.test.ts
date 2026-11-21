import type { NoteImportSource } from '../notes/importSource';
import type { AnnotationDestination } from '../notes/annotationSource';
import { describe, expect, it } from 'vitest';

import { bootstrapFixture, snapshotFixture } from '../test/fixtures';
import { welcomeSessionId } from './route';
import {
  appReducer,
  initialAppState,
  navigationTitle,
  workspaceVisibility,
  type AppAction,
  type AppState,
} from './view';

function readyState(): AppState {
  return appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap: bootstrapFixture });
}

const destination: AnnotationDestination = { entryType: 'highlight', ordinal: 1, prefix: 'A highlight' };

describe('application navigation reducer', () => {
  it.each<NoteImportSource>([
    { kind: 'page', url: 'https://example.org' }, { kind: 'file', path: '/plan.md' },
  ])('carries a $kind source with its opening request and clears it with the selection', importSource => {
    const state = appReducer(readyState(), { type: 'open-note', noteId: 4, importSource });
    expect(state.noteImportSource).toEqual(importSource);
    expect(state.noteRequest).toBe(1);
    for (const action of [
      { type: 'open-note', noteId: 4 }, { type: 'open-note', noteId: 5 },
      { type: 'reload-note' }, { type: 'close-note' },
      { type: 'vault-context-reset' }, { type: 'show-settings' },
    ] satisfies AppAction[]) {
      expect(appReducer(state, action).noteImportSource).toBeUndefined();
    }
  });

  it('reloads the open note without switching the active split panel', () => {
    let state = appReducer(readyState(), { type: 'set-layout', mode: 'split' });
    state = appReducer(state, { type: 'open-note', noteId: 42 });
    state = appReducer(state, { type: 'show-chat' });
    expect(appReducer(state, { type: 'reload-note' })).toEqual({
      ...state,
      noteRequest: state.noteRequest + 1,
      noteAutoFocus: false,
    });
  });

  it('retains a hidden note across layouts, Settings, and Events', () => {
    let state = appReducer(readyState(), { type: 'set-layout', mode: 'split' });
    state = appReducer(state, { type: 'open-note', noteId: 42, destination, importSource: { kind: 'page', url: 'https://example.org' } });
    state = appReducer(state, { type: 'show-chat' });
    expect(state.noteImportSource).toEqual({ kind: 'page', url: 'https://example.org' });
    expect(state.noteDestination).toBe(destination);
    expect(workspaceVisibility(state)).toEqual({ notesVisible: true, chatVisible: true });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'set-layout', mode: 'single' });
    expect(state.noteImportSource).toBeUndefined();
    expect(state.noteDestination).toBeUndefined();
    state = appReducer(state, { type: 'show-chat-tab' });
    state = appReducer(state, { type: 'stream-state', status: 'connected' });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'set-layout', mode: 'split' });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'set-layout', mode: 'single' });
    state = appReducer(state, { type: 'show-settings' });
    expect(state.activeNoteId).toBe(42);
    expect(workspaceVisibility(state)).toEqual({ notesVisible: false, chatVisible: false });
    expect(state.layoutMode).toBe('single');
    state = appReducer(state, { type: 'show-settings-events' });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'show-notes-tab' });
    expect(workspaceVisibility(state).notesVisible).toBe(true);
    expect(state.activeNoteId).toBe(42);
  });

  it('keeps the selected note when opening a conversation in Single View', () => {
    let state = appReducer(readyState(), { type: 'open-note', noteId: 42 });
    state = appReducer(state, { type: 'show-chat' });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'set-layout', mode: 'split' });
    state = appReducer(state, { type: 'open-note', noteId: 42 });
    state = appReducer(state, { type: 'close-note' });
    state = appReducer(state, { type: 'set-layout', mode: 'single' });
    state = appReducer(state, { type: 'set-layout', mode: 'split' });
    expect(state.activeNoteId).toBeNull();
  });

  it('keeps a pending chat operation when opening a note in Split View', () => {
    let state = appReducer(readyState(), { type: 'set-layout', mode: 'split' });
    state = appReducer(state, { type: 'session-operation-started', message: 'Opening session…' });
    state = appReducer(state, { type: 'open-note', noteId: 42 });
    expect(state.sessionOperation).toBe('pending');
    expect(state.sessionOperationMessage).toBe('Opening session…');
  });

  it.each([
    { name: 'empty', recent: [] },
    { name: 'populated', recent: [...bootstrapFixture.recent_sessions].reverse() },
  ])(
    'returns to Welcome after discarding the initial session with $name Recent', ({ recent }) => {
      const bootstrap = {
        ...bootstrapFixture,
        initial_forum_id: 'lobby',
        initial_session_id: 'unused',
        recent_sessions: recent,
      };
      let state = appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap });
      state = appReducer(state, { type: 'session-discarded', forumId: 'lobby', sessionId: 'unused' });
      expect(state.bootstrap?.initial_forum_id).toBe(bootstrap.entrance_forum_id);
      expect(state.bootstrap?.initial_session_id).toBe(welcomeSessionId);
      state = appReducer(state, { type: 'show-initial-conversation' });
      expect(state.activeConversation).toEqual({ forumId: bootstrap.entrance_forum_id, sessionId: welcomeSessionId });
    },
  );

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
      snapshot: { ...lobbySnapshot, default_character_id: 'assistant', session_label: 'Garden planning' },
    });
    expect(state.currentDefaultCharacterId).toBe('assistant');
    expect(state.activeConversation).toEqual({ forumId: 'lobby', sessionId: 'planning' });
    expect(state.activeConversationLabel).toBe('Garden planning');
    expect(state.bootstrap?.recent_sessions.find((session) => session.session_id === 'planning')
      ?.session_label).toBe('Garden planning');
  });

  it('repairs a stale Recent label even when the active conversation label is current', () => {
    const temporaryLabel = 'New session';
    const staleBootstrap = {
      ...bootstrapFixture,
      recent_sessions: bootstrapFixture.recent_sessions.map((session) => (
        session.forum_id === 'lobby' && session.session_id === 'planning'
          ? { ...session, session_label: temporaryLabel }
          : session
      )),
    };
    const snapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'planning',
      session_label: temporaryLabel,
    };
    let state = appReducer(readyState(), { type: 'conversation-opened', snapshot });
    const namedSnapshot = { ...snapshot, session_label: 'Garden planning' };
    state = appReducer(state, { type: 'session-snapshot', snapshot: namedSnapshot });
    state = appReducer(state, { type: 'bootstrap-refreshed', bootstrap: staleBootstrap });
    expect(state.activeConversationLabel).toBe('Garden planning');
    expect(state.bootstrap?.recent_sessions[1].session_label).toBe(temporaryLabel);

    state = appReducer(state, { type: 'session-snapshot', snapshot: namedSnapshot });
    expect(state.bootstrap?.recent_sessions[1].session_label).toBe('Garden planning');
    expect(state.bootstrap?.recent_sessions[0]).toBe(staleBootstrap.recent_sessions[0]);
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

    const generation = { ...snapshotFixture.generation, active: true, request_id: 7,
      progress: { searches_completed: 1, pages_read: 2, activities: [] } };
    const transcript = state.sessionSnapshot?.transcript;
    state = appReducer(state, { type: 'session-generation', forumId: 'entrance', sessionId: 'welcome', generation });
    expect(state.sessionSnapshot?.generation).toEqual(generation);
    expect(state.sessionSnapshot?.transcript).toBe(transcript);
    state = appReducer(state, {
      type: 'session-generation', forumId: 'lobby', sessionId: 'planning',
      generation: { ...generation, active: false },
    });
    expect(state.sessionSnapshot?.generation).toEqual(generation);

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
    expect(navigationTitle(state)).toBe('Character settings');
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
    state = appReducer(state, { type: 'show-character-settings' });
    state = appReducer(state, { type: 'show-character-files' });
    expect(state.mainView).toBe('character-files');
    state = appReducer(state, { type: 'inspect-character-file', characterId: 'guide', filename: 'PROFILE.md' });
    expect(state.mainView).toBe('character-file');
    expect(state.inspectedCharacter.file).toBe('PROFILE.md');
    expect(state.inspectedCharacter.settingsWritable).toBe(true);
    state = appReducer(state, { type: 'show-character-files' });
    expect(state.mainView).toBe('character-files');
    expect(state.inspectedCharacter.file).toBeNull();
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
      web_search: null, web_search_tool: null,
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

describe('workspace tabs', () => {
  it('switches the sidebar and main view together while keeping the note and conversation', () => {
    const showNotes = { type: 'show-notes-tab' } as const;
    const showChat = { type: 'show-chat-tab' } as const;
    let notes = appReducer(readyState(), showNotes);
    expect(notes).toMatchObject({ sidebarTab: 'notes', mainView: 'notes', activeNoteId: null });
    expect(appReducer(notes, showNotes)).toBe(notes);
    notes = appReducer(notes, {
      type: 'open-note', noteId: 4, mode: 'l2', destination,
    });
    const chat = appReducer(notes, showChat);
    expect(chat).toEqual({
      ...notes, sidebarTab: 'chat', mainView: 'chat', noteDestination: undefined,
    });
    expect(appReducer(chat, showChat)).toBe(chat);
    expect(appReducer(chat, showNotes)).toEqual({ ...notes, noteDestination: undefined });
  });

  it('keeps both selections when leaving Notes', () => {
    let state = readyState();
    const conversation = state.activeConversation;
    state = appReducer(state, { type: 'open-note', noteId: 4 });
    expect(state.mainView).toBe('notes');
    expect(state.activeNoteId).toBe(4);
    expect(state.noteRequest).toBe(1);
    expect(state.activeConversation).toEqual(conversation);
    expect(navigationTitle(state)).toBeNull();
    state = appReducer(state, { type: 'open-note', noteId: 4 });
    expect(state.noteRequest).toBe(2);
    state = appReducer(state, { type: 'show-settings' });
    expect(state.mainView).toBe('settings');
    expect(state.activeNoteId).toBe(4);
    expect(state.activeConversation).toEqual(conversation);
  });

  it('clears the open note and advances the notes epoch on vault replacement', () => {
    let state = appReducer(readyState(), { type: 'open-note', noteId: 4 });
    state = appReducer(state, { type: 'show-notes-tab' });
    state = appReducer(state, { type: 'vault-context-reset' });
    expect(state.mainView).toBe('notes');
    expect(state.sidebarTab).toBe('notes');
    expect(state.activeNoteId).toBeNull();
    expect(state.notesEpoch).toBe(1);
    expect(state.activeConversation).toBeNull();
  });

  it('restores the note from startup and refreshed bootstrap data', () => {
    const bootstrap = { ...bootstrapFixture, current_note_id: 42 };
    let state = appReducer(readyState(), { type: 'bootstrap-loaded', bootstrap });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'vault-context-reset' });
    expect(state.activeNoteId).toBeNull();
    state = appReducer(state, { type: 'vault-context-refreshed', bootstrap });
    expect(state.activeNoteId).toBe(42);
    state = appReducer(state, { type: 'vault-context-refreshed', bootstrap: bootstrapFixture });
    expect(state.activeNoteId).toBeNull();
  });

  it.each(['compressed', 'l1', 'l2'] as const)('keeps %s mode for the app session and only reloads when it changes', mode => {
    const initial = readyState();
    expect(initial.noteTextMode).toBe('full_text');
    let state = appReducer(initial, {
      type: 'open-note', noteId: 4, importSource: { kind: 'file', path: '/tmp/note.md' },
    });
    const change = { type: 'set-note-text-mode', mode } as const;
    state = appReducer(state, change);
    expect(state).toMatchObject({ noteTextMode: mode, noteRequest: 2, activeNoteId: 4, notesEpoch: 0 });
    expect(state.noteImportSource).toBeUndefined();
    expect(appReducer(state, change)).toBe(state);
    state = appReducer(state, { type: 'show-settings' });
    expect(state.noteTextMode).toBe(mode);
    state = appReducer(state, { type: 'vault-context-reset' });
    expect(state.noteTextMode).toBe(mode);
    expect(state.activeNoteId).toBeNull();
  });
});

describe('session activity', () => {
  const generating = { ...snapshotFixture, generation: { ...snapshotFixture.generation, active: true } };
  const planning = { ...snapshotFixture, forum: bootstrapFixture.forums[1], session_id: 'planning' };
  const welcome = { forumId: 'entrance', sessionId: 'welcome' };

  it('keeps generation in two sessions and clears a finished marker only on opening its conversation', () => {
    let state = appReducer(readyState(), { type: 'conversation-opened', snapshot: generating });
    state = appReducer(state, { type: 'conversation-opened', snapshot: {
      ...planning, generation: generating.generation,
    } });
    expect(state.sessionActivity).toEqual([
      { ...welcome, status: 'generating' },
      { forumId: 'lobby', sessionId: 'planning', status: 'generating' },
    ]);
    state = appReducer(state, { type: 'session-activity-ended', ...welcome });
    expect(state.sessionActivity[0].status).toBe('finished');
    state = appReducer(state, { type: 'show-sessions' });
    expect(state.sessionActivity[0].status).toBe('finished');
    state = appReducer(state, { type: 'conversation-opened', snapshot: snapshotFixture });
    expect(state.sessionActivity).toEqual([{ forumId: 'lobby', sessionId: 'planning', status: 'generating' }]);
  });

  it.each(['open-note', 'show-settings'] as const)('keeps an unseen completion after %s', (type) => {
    let state = appReducer(readyState(), { type: 'conversation-opened', snapshot: generating });
    state = appReducer(state, type === 'open-note' ? { type, noteId: 4 } : { type });
    state = appReducer(state, { type: 'session-generation', ...welcome, generation: snapshotFixture.generation });
    expect(state.sessionActivity).toEqual([{ ...welcome, status: 'finished' }]);
    state = appReducer(state, { type: 'show-chat-tab' });
    expect(state.sessionActivity).toEqual([]);
  });

  it('does not mark a response unseen while its conversation is visible', () => {
    let state = appReducer(readyState(), { type: 'conversation-opened', snapshot: generating });
    state = appReducer(state, { type: 'session-snapshot', snapshot: snapshotFixture });
    expect(state.sessionActivity).toEqual([]);
  });

  it('clears a completion found by a background check while its conversation is visible', () => {
    let state = appReducer(readyState(), { type: 'conversation-opened', snapshot: generating });
    state = appReducer(state, { type: 'stream-state', status: 'retry' });
    state = appReducer(state, { type: 'session-activity-ended', ...welcome });
    expect(state.sessionActivity).toEqual([]);
  });

  it('removes deleted sessions and forums from activity', () => {
    const state = appReducer(readyState(), { type: 'conversation-opened', snapshot: generating });
    expect(appReducer(state, { type: 'session-deleted', ...welcome }).sessionActivity).toEqual([]);
    expect(appReducer(state, { type: 'forum-deleted', forumId: 'entrance' }).sessionActivity).toEqual([]);
  });
});
