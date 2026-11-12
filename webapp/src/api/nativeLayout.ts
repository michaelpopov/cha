import { maxNotesFontSize, minNotesFontSize, type ViewPreferences } from '../state/view';

export function readInitialNativeViewState(): Partial<ViewPreferences> {
  const value = window.__CHA_NATIVE_INITIAL_VIEW_STATE__;
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {};
  const fields = value as Record<string, unknown>;
  const result: Partial<ViewPreferences> = {};
  if (typeof fields.split === 'boolean') result.layoutMode = fields.split ? 'split' : 'single';
  if (typeof fields.sidebar === 'boolean') result.sidebarOpen = fields.sidebar;
  if (typeof fields.pictures === 'boolean') result.pictureOpen = fields.pictures;
  if (typeof fields.toolbar === 'boolean') result.notesToolbarVisible = fields.toolbar;
  const fontSize = fields.notes_font_size;
  if (typeof fontSize === 'number' && Number.isInteger(fontSize)
      && fontSize >= minNotesFontSize && fontSize <= maxNotesFontSize) {
    result.notesFontSize = fontSize;
  }
  if (typeof fields.chat_toolbar === 'boolean') result.chatToolbarVisible = fields.chat_toolbar;
  if (typeof fields.notes_prompt === 'boolean') result.notesPromptVisible = fields.notes_prompt;
  if (typeof fields.russian === 'boolean') result.russianVisible = fields.russian;
  return result;
}

// Native menus request a change; the renderer reports the applied layout.
export function installNativeLayoutHandler(handler: (mode: unknown) => void) {
  window.__CHA_NATIVE_SET_LAYOUT__ = handler;
  return () => {
    if (window.__CHA_NATIVE_SET_LAYOUT__ === handler) delete window.__CHA_NATIVE_SET_LAYOUT__;
  };
}

export function reportNativeLayout(view: ViewPreferences) {
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
  if (!connectionId) return;
  window.__CHA_NATIVE_POST__?.(JSON.stringify({
    native_action: 'view_state', connection_id: connectionId,
    split: view.layoutMode === 'split', sidebar: view.sidebarOpen, pictures: view.pictureOpen,
    toolbar: view.notesToolbarVisible, notes_font_size: view.notesFontSize,
    notes_prompt: view.notesPromptVisible, chat_toolbar: view.chatToolbarVisible, russian: view.russianVisible,
  }));
}

declare global {
  interface Window {
    __CHA_NATIVE_INITIAL_VIEW_STATE__?: unknown;
    __CHA_NATIVE_SET_LAYOUT__?: (mode: unknown) => void;
  }
}
