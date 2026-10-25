// Run through the native host's custom parity-script hook. All changes belong
// to the runner's disposable vault; no external providers or keys are needed.
async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const check = (value, message) => { if (!value) throw new Error(message); };
  let step = 'startup';
  const completed = [];
  const until = async (label, condition) => {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const error = document.querySelector('[role="alert"]');
      check(!error, error?.textContent);
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
    const note = await screen('Note');
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
        for (const name of [
          'Increase editor font size', 'Decrease editor font size',
          'Bold', 'Italic', 'Underline', 'Highlight', 'Code',
          'Heading 1', 'Heading 2', 'Heading 3',
        ]) {
          editor.view.focus();
          documentHost.scrollTop = 400;
          await settle();
          const top = topText();
          const selection = editor.state.selection.toJSON();
          const control = button(name, note);
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
