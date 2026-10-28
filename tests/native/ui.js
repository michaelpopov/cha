// Run through the native host's custom parity-script hook. All changes belong
// to the runner's disposable vault; no external providers or keys are needed.
async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const check = (value, message) => { if (!value) throw new Error(message); };
  let step = 'startup';
  const completed = [];
  const until = async (label, condition, expectedError = false) => {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const error = document.querySelector('[role="alert"]');
      if (!expectedError) check(!error, error?.textContent);
      const value = condition();
      if (value) return value;
      await wait(25);
    }
    throw new Error('timed out waiting for ' + label);
  };
  const button = (name, root = document) => Array.from(
    root.querySelectorAll('button, [role="tab"]'),
  ).find((node) => node.getClientRects().length && (
    node.getAttribute('aria-label')
      ?? node.querySelector('.cha-primary-line')?.textContent
      ?? node.textContent
  ).trim() === name);
  const click = async (name, root = document) => {
    const node = await until(name, () => {
      const found = button(name, root);
      return found && !found.disabled && found;
    });
    node.scrollIntoView({ block: 'nearest' });
    node.click();
  };
  const screen = (name) => until(name, () => (
    document.querySelector(`main section[aria-label="${name}"]`)
  ));
  const field = (name, root) => {
    const label = Array.from(root.querySelectorAll('label')).find((node) => (
      node.firstChild?.textContent.trim() === name
    ));
    const node = label?.control;
    check(node, 'missing field: ' + name);
    return node;
  };
  const setValue = (node, value) => {
    const prototype = node.tagName === 'SELECT' ? HTMLSelectElement.prototype
      : node.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(prototype, 'value').set.call(node, value);
    node.dispatchEvent(new Event(node.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }));
  };
  const snapshot = async name => {
    await window.webkit?.messageHandlers?.chaTestSnapshot?.postMessage(`${name}-${innerWidth}`);
  };
  const bounds = (node) => node.getBoundingClientRect();
  const composerLayout = () => {
    const form = bounds(document.querySelector('.cha-composer'));
    const status = bounds(document.querySelector('.cha-chat-status'));
    const actions = bounds(document.querySelector('.cha-composer-actions'));
    check(form.left >= 0 && form.right <= innerWidth + 1, 'composer exceeds the window');
    check(status.top >= form.bottom, 'status buttons overlap the composer');
    check(status.bottom < innerHeight - 1, 'status buttons lack bottom spacing');
    check(actions.left >= form.left && actions.right <= form.right,
      'composer actions exceed the edit box');
    return form;
  };
  try {
    const message = await until('Chat composer', () => (
      document.querySelector('textarea[aria-label="Message"]:not(:disabled)')
    ));
    // Layout checks cover the settled state, not animation timing.
    document.querySelector('main').style.transition = 'none';
    check(!document.querySelector('.cha-picture'), 'picture starts closed');
    const chat = composerLayout();
    setValue(message, 'UI smoke draft\nSecond line');
    await until('Chat draft ready', () => button('Send message')?.disabled === false);
    check(message.scrollHeight <= message.clientHeight + 1, 'Chat draft is clipped');
    setValue(message, '');
    completed.push('Chat draft and layout');

    step = 'Notes';
    await click('Notes');
    await click('Actions for Test');
    await click('New note');
    const dialog = await until('New note dialog', () => (
      document.querySelector('dialog[aria-label="New note"]')
    ));
    setValue(field('Name', dialog), 'UI smoke note');
    await click('Create', dialog);
    let note = await screen('Note');
    const prompt = await until('Notes composer', () => note.querySelector('textarea[aria-label="Prompt"]'));
    await until('note editor', () => note.querySelector('.ProseMirror[contenteditable="true"]'));
    const notes = composerLayout();
    check(Math.abs(notes.left - chat.left) <= 1 && Math.abs(notes.width - chat.width) <= 1,
      'Notes composer does not align with Chat');
    const transliteration = bounds(note.querySelector('.cha-transliteration-toggle'));
    const status = bounds(note.querySelector('.cha-chat-status'));
    const statusPadding = parseFloat(getComputedStyle(note.querySelector('.cha-chat-status')).paddingRight);
    check(Math.abs(status.right - transliteration.right - statusPadding) <= 1,
      'Notes transliteration is not right-aligned');
    check(!button('Hide sidebar', note) && !button('Show sidebar', note), 'Notes sidebar toggle remains');
    window.__CHA_NATIVE_SET_LAYOUT__('toggle-sidebar');
    await until('sidebar closed', () => document.querySelector('.cha-app').dataset.sidebar === 'closed');
    composerLayout();
    window.__CHA_NATIVE_SET_LAYOUT__('toggle-sidebar');
    await until('sidebar open', () => document.querySelector('.cha-app').dataset.sidebar === 'open');
    setValue(prompt, 'Saved by the UI smoke test.\nSecond line.');
    await click('Send to UI smoke note', note);
    await until('prompt inserted into note', () => (
      prompt.value === '' && note.querySelector('.ProseMirror').textContent.includes('Second line.')
    ));
    completed.push('Notes creation, prompt insertion and layout');

    step = 'folder store menus, removal and upload choices';
    await click('Actions for Test');
    check(!button('Copy Ref ID', document.querySelector('[role="menu"]')), 'Synthetic root offers a ref');
    document.querySelector('[role="menu"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await click('Actions for UI store empty');
    await click('Remove from store');
    let storeDialog = await until('Remove dialog', () => document.querySelector('dialog[aria-label="Remove folder from store"]'));
    await until('Remove confirmation ready', () => button('Remove', storeDialog) && !button('Remove', storeDialog).disabled);
    check(storeDialog.textContent.includes('OpenAI'), 'Removal omits provider');
    await snapshot('store-remove');
    await click('Remove', storeDialog);
    await until('local association removed', () => !document.querySelector('dialog'));
    check(document.querySelector('.ProseMirror') === note.querySelector('.ProseMirror'), 'Remove replaced the open editor');
    await click('Actions for UI store empty');
    await click('Upload to store');
    storeDialog = await until('Upload choices', () => document.querySelector('dialog[aria-label="Upload folder to store"] select'), true);
    storeDialog = storeDialog.closest('dialog');
    await until('missing key error', () => storeDialog.querySelector('[role="alert"]'), true);
    check(button('Upload', storeDialog).disabled, 'Upload allowed without a compatible key');
    const box = bounds(storeDialog);
    check(box.left >= 0 && box.right <= innerWidth + 1, 'Store dialog exceeds window');
    check(box.top >= 0 && box.bottom <= innerHeight + 1, 'Store dialog exceeds height');
    check(!storeDialog.querySelector('h1,h2,h3'), 'Store dialog has a visible panel title');
    await snapshot('store-upload');
    setValue(field('Provider', storeDialog), 'google');
    check(field('API key', storeDialog).value === '', 'Provider retained an incompatible key');
    storeDialog.dispatchEvent(new Event('cancel', { cancelable: true }));
    await until('Upload closed', () => !document.querySelector('dialog'), true);
    check(document.activeElement?.getAttribute('aria-label') === 'UI store empty', 'Upload did not restore row focus');
    completed.push('Folder store root exclusion, incomplete Remove, provider choices, missing keys, sizing and Escape focus');

    step = 'folder store failed deletion and editor recovery';
    const failureRow = Array.from(document.querySelectorAll('.cha-notes-row')).find(row => row.getAttribute('aria-label') === 'UI store failure');
    failureRow.click();
    await click('Actions for UI store note');
    document.querySelector('[role="menu"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    const storeNoteRow = Array.from(document.querySelectorAll('.cha-notes-row')).find(row => row.getAttribute('aria-label') === 'UI store note');
    storeNoteRow.click();
    await until('store note editor', () => document.querySelector('.ProseMirror')?.textContent.includes('Native store editor fixture'));
    const storeEditor = document.querySelector('.ProseMirror').chaEditor;
    await click('Actions for UI store failure');
    await click('Remove from store');
    storeDialog = await until('failed Remove confirmation', () => document.querySelector('dialog[aria-label="Remove folder from store"]'));
    await click('Remove', storeDialog);
    await until('failed Remove', () => button('Close', storeDialog) && !button('Close', storeDialog).disabled, true);
    check(storeDialog.textContent.includes('Could not remove “UI store failure” from OpenAI:'), 'Remove failure omits folder or provider');
    check(!storeDialog.textContent.includes('Use Remove from store'), 'Remove repeats its own action as guidance');
    check(!storeDialog.textContent.includes('vs_native') && !storeDialog.textContent.includes('store_record='), 'Remove exposes resource diagnostics');
    check(storeDialog.scrollWidth <= storeDialog.clientWidth + 1, 'Remove failure scrolls horizontally');
    await snapshot('store-remove-failed');
    button('Close', storeDialog).click();
    await until('failed Remove closed', () => !document.querySelector('dialog'), true);
    const beginDelete = async () => {
      await click('Actions for UI store failure');
      await click('Delete');
      const dialog = await until('store deletion confirmation', () => document.querySelector('dialog[aria-label="Delete “UI store failure”"]'));
      await until('Delete confirmation ready', () => button('Delete', dialog) && !button('Delete', dialog).disabled);
      check(dialog.textContent.includes('1 remote store will also be removed.'), 'Store count missing');
      await click('Delete', dialog);
      await until('failed deletion', () => button('Retry', dialog) && !button('Retry', dialog).disabled, true);
      return dialog;
    };
    storeDialog = await beginDelete();
    check(storeDialog.textContent.includes('Could not remove 1 OpenAI store in “UI store failure”:'), 'Delete failure omits actual store count');
    check(!storeDialog.textContent.includes('vs_native') && !storeDialog.textContent.includes('store_record='), 'Delete exposes resource diagnostics');
    check(storeDialog.scrollWidth <= storeDialog.clientWidth + 1, 'Delete failure scrolls horizontally');
    await snapshot('store-delete-failed');
    check(!storeEditor.isDestroyed && storeEditor.isEditable, 'Failed delete disposed or froze editor');
    check(storeDialog.textContent.includes('Remote data and charges may remain.'), 'Delete anyway warning missing');
    storeDialog.dispatchEvent(new Event('cancel', { cancelable: true }));
    await until('failed reservation released', () => !document.querySelector('dialog'), true);
    storeEditor.commands.insertContent(' still editable');
    storeDialog = await beginDelete();
    // Expected errors stay visible through the explicit local-delete decision.
    button('Delete anyway', storeDialog).click();
    await until('warning after local deletion', () => storeDialog.textContent.includes('Some remote data could not be confirmed deleted.'), true);
    await until('affected editor disposed', () => storeEditor.isDestroyed, true);
    await snapshot('store-delete-warning');
    check(!button('Retry', storeDialog), 'Warning offered a second local deletion');
    button('Close', storeDialog).click();
    await until('warning acknowledged', () => !document.querySelector('dialog'), true);
    check(!document.querySelector('[aria-label="UI store failure"][role="treeitem"]'), 'Deleted folder remains');
    check(document.activeElement?.getAttribute('role') === 'treeitem', 'Deletion did not restore tree focus');
    completed.push('Folder store failed cleanup, editor preservation, Cancel release, Delete anyway, warning and focus');
    // Restore the original editor for the existing toolbar tests.
    Array.from(document.querySelectorAll('.cha-notes-row')).find(row => row.getAttribute('aria-label') === 'UI smoke note').click();
    note = await screen('Note');
    await until('original note reopened', () => note.querySelector('.ProseMirror')?.textContent.includes('Second line.'));

    step = 'Notes toolbar scroll position';
    const editor = note.querySelector('.ProseMirror').chaEditor;
    const original = editor.getJSON();
    editor.commands.setContent(Array.from({ length: 80 }, (_, index) => (
      `<p>Paragraph ${index + 1}: keep this reading position.</p>`
    )).join(''));
    editor.commands.setTextSelection(editor.state.doc.content.size - 1);
    const documentHost = note.querySelector('.cha-notes-document');
    const settle = () => wait(75);
    const topText = () => {
      const box = documentHost.getBoundingClientRect();
      const found = editor.view.posAtCoords({ left: box.left + box.width / 2, top: box.top + 1 });
      return found && editor.state.doc.resolve(found.pos).parent.textContent;
    };
    await settle();
    check(documentHost.scrollHeight > documentHost.clientHeight + 800, 'Test note is not scrollable');
    // macOS mouse clicks keep editor focus; keyboard activation focuses the
    // toolbar control. Cover both with a caret and an offscreen text selection.
    for (const selectText of [false, true]) {
      const end = editor.state.doc.content.size - 1;
      editor.commands.setTextSelection(selectText ? { from: end - 7, to: end } : end);
      for (const focusControl of [false, true]) {
        for (const [name, menu] of [
          ['Increase font size', 'Font size'], ['Decrease font size', 'Font size'],
          ['Bold', 'Edit'], ['Italic', 'Edit'], ['Underline', 'Edit'], ['Highlight', null], ['Code', 'Insert'],
          ['Title', 'Headings'], ['Chapter', 'Headings'], ['Section', 'Headings'],
        ]) {
          editor.view.focus();
          documentHost.scrollTop = 400;
          await settle();
          const top = topText();
          const selection = editor.state.selection.toJSON();
          if (menu) await click(menu, note);
          const control = await until(name, () => button(name, note));
          if (focusControl) control.focus({ preventScroll: true });
          control.click();
          await settle();
          // A font size change moves the pixels; the text at the top stays.
          if (name.endsWith('font size')) {
            check(topText() === top, `${name} moved the note from ${top} to ${topText()}`);
          } else {
            check(Math.abs(documentHost.scrollTop - 400) <= 1,
              `${name} moved the note to ${documentHost.scrollTop}`);
          }
          check(JSON.stringify(editor.state.selection.toJSON()) === JSON.stringify(selection),
            `${name} changed the selection`);
          check(document.activeElement === editor.view.dom, `${name} did not return focus`);
        }
      }
    }
    await click('Jump to heading', note);
    await settle();
    check(Math.abs(documentHost.scrollTop - 400) <= 1, 'Opening headings moved the note');
    note.querySelector('[role="menu"] button').click();
    await settle();
    check(documentHost.scrollTop > 400, 'Heading navigation did not scroll to the heading');
    editor.commands.setContent(original);
    completed.push('Notes toolbar preserves scroll position and selection');

    step = 'settings navigation';
    await click('Settings', document.querySelector('aside'));
    for (const [name, label, ready] of [
      ['Personas', 'Personas navigation', 'New persona'],
      ['Characters', 'Characters navigation', 'New character'],
      ['Forums', 'Forums navigation', 'New forum'],
      ['Providers', 'Providers settings', 'New provider'],
      ['Session settings', 'Session settings', 'Save session naming'],
      ['Voices', 'Voices settings', 'New voice'],
    ]) {
      step = name;
      await click(name, await screen('Settings'));
      const page = await screen(label);
      await until(name + ' loaded', () => button(ready, page));
      await click('Settings', page);
      await screen('Settings');
      completed.push(name);
    }

    step = 'style save and reset';
    await click('Styles', await screen('Settings'));
    await click('New style', await screen('Styles settings'));
    let page = await screen('New style settings');
    setValue(field('Name', page), 'Cancelled style');
    await click('Cancel', page);
    page = await screen('Styles settings');
    await until('Styles loaded', () => button('New style', page));
    check(!button('Cancelled style', page), 'Cancel created a style');
    await click('New style', page);
    page = await screen('New style settings');
    setValue(field('Name', page), 'UI smoke style');
    await click('Create style', page);
    page = await screen('Style settings');
    await until('style fields', () => page.querySelector('select'));
    setValue(field('Font', page), 'mono');
    await click('Save style', page);
    await until('style saved', () => button('Save style', page)?.disabled);
    await click('Styles', page);
    await click('UI smoke style', await screen('Styles settings'));
    page = await screen('Style settings');
    await until('saved style', () => page.querySelector('select'));
    check(field('Font', page).value === 'mono', 'Saved style did not persist');
    setValue(field('Font', page), 'serif');
    await click('Reset', page);
    await until('style reset', () => field('Font', page).value === 'mono');
    check(button('Save style', page).disabled, 'Reset left unsaved style changes');
    completed.push('Style create, cancel, save, reopen and reset');

    step = 'note persistence';
    const row = Array.from(document.querySelectorAll('.cha-notes-row')).find((node) => (
      node.querySelector('.cha-notes-label')?.textContent === 'UI smoke note'
    ));
    check(row, 'Created note is missing from the tree');
    row.click();
    await screen('Note');
    await until('saved note content', () => (
      document.querySelector('.ProseMirror')?.textContent.includes('Saved by the UI smoke test.')
    ));
    composerLayout();
    completed.push('Note persistence after navigation');
    return { ok: true, width: innerWidth, completed };
  } catch (error) {
    return { ok: false, width: innerWidth, step, completed, reason: String(error.message || error) };
  }
}
