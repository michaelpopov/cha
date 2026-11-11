// Real host smoke test with the delayed local provider in test-note-compression.py.
async function nativeParity() {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const until = async (label, condition) => {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      const value = condition();
      if (value) return value;
      await wait(25);
    }
    throw new Error('Timed out: ' + label + ': ' + document.body.innerText.slice(-1200));
  };
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const button = (name, root = document) => [...root.querySelectorAll('button,[role="tab"]')]
    .find(node => node.getClientRects().length && (node.getAttribute('aria-label') ?? node.textContent).trim() === name);
  const click = async (name, root = document) => (await until(name, () => {
    const node = button(name, root); return node && !node.disabled && node;
  })).click();
  const row = name => [...document.querySelectorAll('[role="treeitem"]')].find(node => node.getAttribute('aria-label') === name);
  const editor = () => document.querySelector('.ProseMirror')?.chaEditor;
  const openActions = name => until('actions menu for ' + name, () => {
    const menu = document.querySelector('[role="menu"]');
    if (menu) return menu;
    const trigger = button('Actions for ' + name);
    if (trigger && !trigger.disabled) trigger.click();
  });
  const action = async (name, label) => {
    await click(label, await openActions(name));
    await until('menu closed', () => !document.querySelector('[role="menu"]'));
  };
  const completed = [];
  try {
    await until('startup', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    const emptyPane = await until('empty Notes pane in split view', () => {
      const pane = document.querySelector('.cha-main.is-chat.is-notes .cha-notes-screen');
      if (pane) return pane;
      window.__CHA_NATIVE_SET_LAYOUT__('split');
    });
    check(!emptyPane.querySelector('.cha-notes-toolbar'), 'An empty Notes pane showed a toolbar');
    check(!button('Note text mode', emptyPane), 'An empty Notes pane showed a standalone mode button');
    window.__CHA_NATIVE_SET_LAYOUT__('single');
    await until('single Chat view', () => !document.querySelector('.cha-notes-screen'));
    await click('Notes');
    (await until('Single', () => row('Single'))).click();
    await until('editor', () => editor()?.isEditable);
    const toolbar = document.querySelector('.cha-notes-toolbar');
    check(toolbar && button('Note text mode', toolbar) && button('Playback speed', toolbar),
      'The mode button and Playback speed must share the Notes toolbar');
    const dialog = () => document.querySelector('dialog[aria-label="Note compression"][open]');
    const modeMenu = () => document.querySelector('[role="menu"][aria-label="Note text mode"]');
    const mode = async name => {
      await click('Note text mode');
      await click(name, modeMenu());
    };
    const checkedMode = async () => {
      await click('Note text mode');
      const checked = modeMenu()?.querySelectorAll('[role="menuitemradio"][aria-checked="true"]');
      await click('Note text mode');
      check(checked?.length === 1, 'Expected exactly one checked mode');
      return checked[0].textContent.trim();
    };
    const modeTitle = () => button('Note text mode')?.title;
    const endCompression = () => until('dialog closed and editor restored', () => !dialog() && editor()?.isEditable);
    check(await checkedMode() === 'Full text', 'Startup mode is not Full text');
    await mode('Compressed');
    await until('full-text fallback', () => modeTitle() === 'Compressed mode; showing full text' && editor()?.isEditable);
    check(await checkedMode() === 'Compressed', 'Compressed mode is not checked');
    editor().commands.insertContent(' original edit');
    await action('Single', 'Compress');
    const first = await until('compression dialog', dialog);
    await until('compression started', () => first.innerText.includes('Compressing “Single”'));
    check(!editor()?.isEditable, 'Editor stayed editable during compression');
    check(document.activeElement === button('Cancel', first), 'Cancel did not receive focus');
    // Let the delayed provider receive the accepted request before cancelling it.
    await wait(250);
    await click('Cancel', first);
    await endCompression();
    check(modeTitle() === 'Compressed mode; showing full text', 'Cancellation changed the mode or fallback');
    check(editor().getText().endsWith(' original edit'), 'Compression did not save the current document');

    await action('Single', 'Compress');
    await endCompression();
    check(modeTitle() === 'Compressed mode; showing compressed', 'Dialog closed before the compressed editor reloaded');
    check(editor().getText() === 'Compressed café\n\n- Result', 'Compressed text did not round-trip');
    check(row('Single')?.querySelector('.cha-notes-compressed'), 'Compressed tree metadata did not refresh');
    await action('Single', 'Show compressed');
    await until('shortcut reopened the compressed editor', () => editor()?.isEditable && modeTitle() === 'Compressed mode; showing compressed');
    editor().chain().setHardBreak().insertContent('manual edit').run();
    await mode('Full text');
    await until('full text', () => editor()?.isEditable && editor().getText().endsWith(' original edit'));
    check(!editor().getText().includes('manual edit'), 'Compressed edit reached full text');
    await mode('Compressed');
    await until('reopened edited compression', () => editor()?.isEditable && editor().getText().endsWith('manual edit'));
    check(editor().getJSON().content[0].content.some(node => node.type === 'hardBreak'), 'Edited hard break did not survive reopening');
    check(editor().getText().includes('Compressed café'), 'Compressed content was lost');
    completed.push('blocking compression, fallback, Cancel, and independent native editor saves');

    (await until('Batch', () => row('Batch'))).click();
    (await until('Nested folder', () => row('Nested folder'))).click();
    await action('Batch', 'Compress');
    const folderDialog = await until('folder compression', dialog);
    await until('second folder target', () => folderDialog.innerText.includes('Note 2 of 2'));
    check(folderDialog.innerText.includes('Compressing “A slow”'), 'Folder progress did not identify its current note');
    check(!editor()?.isEditable, 'An unrelated editor stayed editable during folder compression');
    await wait(250);
    folderDialog.dispatchEvent(new Event('cancel', { cancelable: true }));
    await endCompression();
    await until('nested result retained', () => row('B nested')?.querySelector('.cha-notes-compressed'));
    check(!row('A slow')?.querySelector('.cha-notes-compressed'), 'Cancelled folder note has a result');
    completed.push('sequential folder progress, Escape cancellation, and committed result retained');
    await click('Settings', document.querySelector('aside'));
    await click('Events');
    await until('compression events', () => document.body.innerText.includes('Compressed version ready for “B nested”'));
    check(document.body.innerText.includes('Compression cancelled for “A slow”'), 'Cancellation event missing');
    completed.push('Events outcomes');
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error) }; }
}
