// Native UI coverage; all sources and stores belong to the disposable fixture.
async function nativeParity() {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const check = (value, message) => { if (!value) throw new Error(message); };
  let step = 'startup';
  const until = async (name, condition) => {
    for (const deadline = Date.now() + 5000; Date.now() < deadline;) {
      const value = condition();
      if (value) return value;
      await wait(25);
    }
    throw new Error('Timed out: ' + name);
  };
  const button = (name, root = document) => [...root.querySelectorAll('button, [role="tab"]')]
    .find(node => node.getClientRects().length && (node.getAttribute('aria-label')
      ?? node.querySelector('.cha-primary-line, .cha-roster-name')?.textContent ?? node.textContent).trim() === name);
  const click = async (name, root = document) => {
    const node = await until(name, () => {
      const found = button(name, root);
      return found && !found.disabled && found;
    });
    node.scrollIntoView({ block: 'nearest' });
    node.click();
  };
  const screen = name => until(name, () => document.querySelector(`section[aria-label="${name}"]`));
  const setRef = value => {
    const input = [...document.querySelectorAll('label')].find(label => label.firstChild?.textContent === 'Ref ID')?.control;
    check(input, 'Ref ID field is missing');
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, value);
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const snapshot = async name => {
    await wait(80);
    const page = document.querySelector('main');
    check(page.scrollWidth <= page.clientWidth + 1, 'Reference screen overflows horizontally');
    for (const node of page.querySelectorAll('input, button')) {
      if (!node.getClientRects().length) continue;
      const box = node.getBoundingClientRect();
      check(box.left >= -1 && box.right <= innerWidth + 1, 'Reference control exceeds viewport');
    }
    await window.webkit?.messageHandlers?.chaTestSnapshot?.postMessage(`${name}-${innerWidth}`);
  };
  try {
    await until('Chat', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    let copied;
    Object.defineProperty(navigator, 'clipboard', { configurable: true,
      value: { writeText: async value => { copied = value; } } });
    await click('Notes');
    const refs = {};
    await click('Actions for Reference note');
    await click('Copy Ref ID');
    refs['Reference note'] = await until('copied reference', () => copied);
    // Folders have no Copy Ref ID. The fixture creates both folders right after the note.
    const base62 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
    const noteId = [...refs['Reference note'].slice(2)].reduce((id, digit) => id * 62 + base62.indexOf(digit), 0);
    const ref = id => '##' + [5, 4, 3, 2, 1, 0].map(power => base62[Math.floor(id / 62 ** power) % 62]).join('');
    refs['OpenAI reference folder'] = ref(noteId + 1);
    refs['Gemini reference folder'] = ref(noteId + 2);
    await click('Settings', document.querySelector('aside'));
    await click('Characters');
    await click('Guide');
    await click('Character settings');
    await click('Files');
    await click('New file');
    await click('Folder reference');
    setRef(refs['Reference note']);
    await click('Add file');
    await until('wrong-kind error', () => document.querySelector('[role="alert"]')?.textContent.includes('Select Note reference'));
    await click('Note reference');
    check([...document.querySelectorAll('label')].find(label => label.firstChild?.textContent === 'Ref ID').control.value === refs['Reference note'], 'Mode change lost Ref ID');
    await snapshot('new-note-reference');
    await click('Add file');
    let files = await screen('Character files navigation');
    await click('Reference note (note reference)', files);
    let detail = await screen('Character reference navigation');
    await until('saved note reference detail', () => detail.textContent.includes(refs['Reference note']));
    check(document.querySelector('.cha-topbar-title h1') === null, 'Reference adds a navigation title');
    await snapshot('saved-note-reference');
    await click('Delete', detail);
    await click('Delete', await until('delete dialog', () => document.querySelector('dialog')));
    files = await screen('Character files navigation');
    await until('reference removed', () => !button('Reference note (note reference)', files));
    for (const name of ['OpenAI reference folder', 'Gemini reference folder']) {
      step = name;
      await click('New file', files);
      await click('Folder reference');
      setRef(refs[name]);
      await snapshot('new-folder-reference-' + (name.startsWith('OpenAI') ? 'openai' : 'gemini'));
      await click('Add file');
      files = await screen('Character files navigation');
      await until('saved folder row', () => button(name + ' (folder reference)', files));
    }
    await snapshot('character-reference-list');
    await click('Character settings', files);
    await click('Guide');
    await click('Characters');
    await click('Assistant');
    await click('Character settings');
    await click('Files');
    await click('New file');
    check(!button('Write text'), 'Assistant text files became editable');
    await click('Note reference');
    setRef(refs['Reference note']);
    await click('Add file');
    files = await screen('Character files navigation');
    await until('Assistant reference', () => button('Reference note (note reference)', files));
    await click('Settings', document.querySelector('aside'));
    await click('Forums');
    await click('The Lobby');
    await click('The Lobby', await screen('Forum sessions navigation'));
    const forum = await screen('Forum detail navigation');
    await click('New file', forum);
    await click('Folder reference');
    setRef(refs['Gemini reference folder']);
    await click('Add file');
    const forumFiles = await screen('Forum detail navigation');
    await click('Gemini reference folder (folder reference)', forumFiles);
    detail = await screen('Forum reference navigation');
    await until('saved forum reference detail', () => detail.textContent.includes(refs['Gemini reference folder']));
    await snapshot('saved-forum-reference');
    return { ok: true, width: innerWidth, completed: ['note and both folder services', 'mode retention and errors',
      'saved detail and deletion', 'Assistant permissions', 'Forum reference'] };
  } catch (error) {
    return { ok: false, width: innerWidth, step, reason: String(error.message || error) };
  }
}
