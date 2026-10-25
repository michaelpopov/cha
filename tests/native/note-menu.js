// Run with CHA_NATIVE_PARITY_SCRIPT against the native host's disposable vault.
async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const check = (value, message) => { if (!value) throw new Error(message); };
  let step = 'startup';
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
  const button = (name, root = document) => Array.from(root.querySelectorAll('button, [role="tab"]'))
    .find((node) => node.getClientRects().length && (
      node.getAttribute('aria-label') ?? node.querySelector('.cha-primary-line')?.textContent ?? node.textContent
    ).trim() === name);
  const click = async (name, root = document) => (await until(name, () => {
    const found = button(name, root);
    return found && !found.disabled && found;
  })).click();
  const editor = () => document.querySelector('.ProseMirror')?.chaEditor;
  const setPrompt = (text) => {
    const prompt = document.querySelector('textarea[aria-label="Prompt"]');
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set.call(prompt, text);
    prompt.dispatchEvent(new Event('input', { bubbles: true }));
    return prompt;
  };
  const openMenu = async (readOnly, target = editor().view.dom.querySelector('a'), commentAction = 'Add comment') => {
    const event = new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
      clientX: innerWidth - 5, clientY: innerHeight - 5 });
    target.dispatchEvent(event);
    const menu = await until('context menu', () => document.querySelector('[role="menu"]'));
    check(event.defaultPrevented, 'default menu was not suppressed');
    const items = Array.from(menu.querySelectorAll('[role="menuitem"]'));
    check(items.map((item) => item.textContent).join(',') === 'Cut,Copy,Copy JSON,Paste,Bold,Italic,Highlight,' + commentAction,
      'unexpected context menu items');
    for (const item of items) {
      check(item.disabled === (readOnly && !['Copy', 'Copy JSON', 'Highlight', commentAction].includes(item.textContent)),
        'wrong disabled state for ' + item.textContent);
    }
    const box = menu.getBoundingClientRect();
    check(box.right <= innerWidth + 1 && box.bottom <= innerHeight + 1, 'menu extends beyond the window');
    return menu;
  };
  const openNote = async () => {
    const previous = editor();
    const row = await until('menu note', () => Array.from(document.querySelectorAll('[role="treeitem"]'))
      .find((node) => node.querySelector('.cha-notes-label')?.textContent === 'Menu note'));
    row.click();
    await until('reopened note', () => editor() && editor() !== previous);
  };
  try {
    await until('Chat ready', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    await click('Actions for Test');
    await click('New note');
    const input = await until('name', () => document.querySelector('dialog input'));
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, 'Menu note');
    input.dispatchEvent(new Event('input', { bubbles: true }));
    await click('Create');
    await until('new editor', () => editor()?.isEditable);
    editor().commands.setContent('<p>Alpha <a href="https://example.com">beta</a> gamma</p>');
    const destination = editor().view.dom.querySelector('a').getAttribute('href');
    editor().commands.setTextSelection({ from: 7, to: 11 });
    step = 'writable formatting';
    for (const command of ['Bold', 'Italic', 'Highlight']) {
      await click(command, await openMenu(false));
    }
    check(editor().view.dom.querySelector('mark')?.textContent === 'beta', 'highlight missed the selection');
    check(editor().view.dom.querySelector('strong')?.textContent === 'beta', 'bold missed the selection');
    check(editor().view.dom.querySelector('em')?.textContent === 'beta', 'italic missed the selection');
    await click('Actions for Menu note');
    await click('Make read-only');
    await until('read-only', () => editor() && !editor().isEditable
      && button('Actions for Menu note') && !button('Actions for Menu note').disabled);
    step = 'read-only highlight removal and save';
    editor().commands.setTextSelection({ from: 7, to: 11 });
    await click('Highlight', await openMenu(true));
    check(!editor().view.dom.querySelector('mark'), 'read-only highlight removal failed');
    await click('Settings');
    await until('Settings', () => document.querySelector('section[aria-label="Settings"]'));
    await openNote();
    check(!editor().isEditable, 'read-only state was lost');
    check(!editor().view.dom.querySelector('mark'), 'highlight removal was not saved');
    step = 'read-only highlight addition and save';
    editor().commands.setTextSelection({ from: 2, to: 5 });
    await click('Highlight', await openMenu(true));
    check(editor().view.dom.querySelector('mark')?.textContent === 'lph', 'read-only highlight addition failed');
    await click('Settings');
    await until('Settings', () => document.querySelector('section[aria-label="Settings"]'));
    await openNote();
    check(editor().view.dom.querySelector('mark')?.textContent === 'lph', 'read-only highlight was not saved');
    check(editor().getText() === 'Alpha beta gamma', 'note text changed');
    check(editor().view.dom.querySelector('a')?.getAttribute('href') === destination, 'link changed');
    for (const readOnly of [true, false]) {
      if (!readOnly) {
        await click('Actions for Menu note');
        await click('Make editable');
        await until('editable', () => editor()?.isEditable);
      }
      step = `${readOnly ? 'read-only' : 'writable'} comment insertion and save`;
      const prompt = setPrompt('');
      check(!prompt.disabled, 'prompt is disabled');
      const before = editor().getText();
      await click('Add comment', await openMenu(readOnly));
      check(editor().getText() === before, 'empty prompt added a comment');
      const text = readOnly ? 'Read-only comment' : 'Writable comment';
      setPrompt(text);
      await click('Add comment', await openMenu(readOnly));
      check(prompt.value === '', 'comment did not clear the prompt');
      const paragraphs = Array.from(editor().view.dom.querySelectorAll('p'));
      check(paragraphs.at(-2)?.textContent === text, 'comment was not before the clicked paragraph');
      const comment = paragraphs.at(-2)?.querySelector('mark');
      const dark = matchMedia('(prefers-color-scheme: dark)').matches;
      check(comment && getComputedStyle(comment).backgroundColor === (dark ? 'rgb(59, 83, 110)' : 'rgb(211, 227, 245)'),
        'comment highlight does not match the OS appearance');
      check(editor().isEditable === !readOnly, 'comment changed read-only state');
      await click('Settings');
      await until('Settings', () => document.querySelector('section[aria-label="Settings"]'));
      await openNote();
      check(Array.from(editor().view.dom.querySelectorAll('p')).at(-2)?.textContent === text,
        'comment was not saved');
      check(editor().isEditable === !readOnly, 'saved comment changed read-only state');

      step = `${readOnly ? 'read-only' : 'writable'} comment deletion and save`;
      const retainedPrompt = setPrompt('Keep this draft');
      const savedComment = editor().view.dom.querySelector('mark[data-color="#b3d9ff"]');
      await click('Delete comment', await openMenu(readOnly, savedComment, 'Delete comment'));
      check(editor().getText() === before, 'deletion changed another paragraph');
      check(retainedPrompt.value === 'Keep this draft', 'deletion changed the prompt');
      check(editor().isEditable === !readOnly, 'deletion changed read-only state');
      await click('Settings');
      await until('Settings', () => document.querySelector('section[aria-label="Settings"]'));
      await openNote();
      check(editor().getText() === before, 'comment deletion was not saved');
      check(!editor().view.dom.querySelector('mark[data-color="#b3d9ff"]'), 'deleted comment reappeared');
    }
    return { ok: true, completed: ['exact menu in both states', 'menu stays within window',
      'formatting preserves selection', 'read-only highlight addition/removal survives reopen',
      'blue comments before clicked paragraphs survive reopen in both states', 'empty prompt does nothing',
      'comment deletion survives reopen in both states and keeps the prompt'] };
  } catch (error) {
    return { ok: false, step, error: String(error) };
  }
}
