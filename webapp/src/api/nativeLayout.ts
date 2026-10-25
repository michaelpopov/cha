// Native menus request a change; the renderer reports the applied layout.
export function installNativeLayoutHandler(handler: (mode: unknown) => void) {
  window.__CHA_NATIVE_SET_LAYOUT__ = handler;
  return () => {
    if (window.__CHA_NATIVE_SET_LAYOUT__ === handler) delete window.__CHA_NATIVE_SET_LAYOUT__;
  };
}

export function reportNativeLayout(split: boolean, sidebar: boolean, pictures: boolean, toolbar: boolean, notesPrompt: boolean) {
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
  if (!connectionId) return;
  window.__CHA_NATIVE_POST__?.(JSON.stringify({
    native_action: 'view_state', connection_id: connectionId, split, sidebar, pictures, toolbar, notes_prompt: notesPrompt,
  }));
}

declare global {
  interface Window {
    __CHA_NATIVE_SET_LAYOUT__?: (mode: unknown) => void;
  }
}
