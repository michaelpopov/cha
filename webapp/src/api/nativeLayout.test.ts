import { afterEach, expect, it } from 'vitest';
import { readInitialNativeViewState } from './nativeLayout';
import { bootstrapFixture } from '../test/fixtures';
import { appReducer, createInitialAppState, initialAppState } from '../state/view';

afterEach(() => { delete window.__CHA_NATIVE_INITIAL_VIEW_STATE__; });

it('reads only valid View preferences and preserves false values', () => {
  window.__CHA_NATIVE_INITIAL_VIEW_STATE__ = {
    split: true, sidebar: false, pictures: true, toolbar: false,
    chat_toolbar: false, notes_prompt: false, russian: true, extra: true,
  };
  const prefs = readInitialNativeViewState();
  expect(prefs).toEqual({ layoutMode: 'split', sidebarOpen: false, pictureOpen: true,
    notesToolbarVisible: false, chatToolbarVisible: false, notesPromptVisible: false, russianVisible: true });
  expect(createInitialAppState(prefs)).toMatchObject(prefs);
  expect(initialAppState.sidebarOpen).toBe(true);
});

it('ignores invalid payloads and uses defaults for missing values', () => {
  window.__CHA_NATIVE_INITIAL_VIEW_STATE__ = [];
  expect(readInitialNativeViewState()).toEqual({});
  window.__CHA_NATIVE_INITIAL_VIEW_STATE__ = { sidebar: 0, russian: true, split: null };
  expect(readInitialNativeViewState()).toEqual({ russianVisible: true });
  expect(createInitialAppState({ sidebarOpen: undefined }).sidebarOpen).toBe(true);
});

it.each([undefined, null, false, 'saved', 1, []])('uses defaults for a non-object startup payload: %j', payload => {
  window.__CHA_NATIVE_INITIAL_VIEW_STATE__ = payload;
  expect(readInitialNativeViewState()).toEqual({});
  expect(createInitialAppState(readInitialNativeViewState())).toEqual(initialAppState);
});

it('preserves all saved View preferences through bootstrap and vault transitions', () => {
  const prefs = { layoutMode: 'split' as const, sidebarOpen: false, pictureOpen: true,
    notesToolbarVisible: false, chatToolbarVisible: false, notesPromptVisible: false, russianVisible: true };
  let state = createInitialAppState(prefs);
  expect(state).toEqual({ ...initialAppState, ...prefs });
  // A runtime object cannot override non-preference fields through the initializer.
  expect(createInitialAppState(Object.assign({ mainView: 'notes' }, prefs)).mainView).toBe('chat');
  for (const action of [
    { type: 'bootstrap-loaded', bootstrap: bootstrapFixture },
    { type: 'show-settings' },
    { type: 'bootstrap-refreshed', bootstrap: bootstrapFixture },
    { type: 'vault-context-reset' },
    { type: 'vault-context-refreshed', bootstrap: bootstrapFixture },
  ] as const) {
    state = appReducer(state, action);
    expect(state).toMatchObject(prefs);
  }
});
