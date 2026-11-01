import type { ViewPreferences } from '../state/view';

export function readInitialNativeViewState(): Partial<ViewPreferences> {
  const value = window.__CHA_NATIVE_INITIAL_VIEW_STATE__;
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {};
  const fields = value as Record<string, unknown>;
  const result: Partial<ViewPreferences> = {};
  if (typeof fields.split === 'boolean') result.layoutMode = fields.split ? 'split' : 'single';
  if (typeof fields.sidebar === 'boolean') result.sidebarOpen = fields.sidebar;
  if (typeof fields.pictures === 'boolean') result.pictureOpen = fields.pictures;
  if (typeof fields.toolbar === 'boolean') result.notesToolbarVisible = fields.toolbar;
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

export function reportNativeLayout(split: boolean, sidebar: boolean, pictures: boolean, toolbar: boolean, notesPrompt: boolean, chatToolbar: boolean, russian: boolean) {
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
  if (!connectionId) return;
  window.__CHA_NATIVE_POST__?.(JSON.stringify({
    native_action: 'view_state', connection_id: connectionId, split, sidebar, pictures, toolbar, notes_prompt: notesPrompt, chat_toolbar: chatToolbar, russian,
  }));
}

declare global {
  interface Window {
    __CHA_NATIVE_INITIAL_VIEW_STATE__?: unknown;
    __CHA_NATIVE_SET_LAYOUT__?: (mode: unknown) => void;
  }
}
