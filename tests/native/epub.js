// Shared WKWebView/WebView2 probe. Select with CHA_NATIVE_PARITY_SCRIPT.
// Tiny EPUB 2/3 fixtures cover XHTML entities and self-closing tags, encoded
// paths, a nested navigation file, spine order, repeated files and omitted media.
const epubFixtures = [
  {
    "name": "Native EPUB 2",
    "base64": "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"
  },
  {
    "name": "Native EPUB 3",
    "base64": "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"
  }
];

async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const check = (condition, message) => { if (!condition) throw new Error(message); };
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
  const button = (name) => [...document.querySelectorAll('button')].find((node) => (
    (node.getAttribute('aria-label') ?? node.textContent).trim() === name && !node.disabled
  ));
  const row = (name, parent = document) => [...parent.querySelectorAll('[role="treeitem"]')].find((node) => (
    node.querySelector('.cha-notes-label')?.textContent === name
  ));
  const select = (input, fixture) => {
    const transfer = new DataTransfer();
    const bytes = Uint8Array.from(atob(fixture.base64), (char) => char.charCodeAt(0));
    transfer.items.add(new File([bytes], 'book.epub', { type: 'application/epub+zip' }));
    input.files = transfer.files;
    input.dispatchEvent(new Event('change', { bubbles: true }));
  };
  let step = 'startup';
  const completed = [];
  const violations = [];
  const record = (event) => { if (event.blockedURI?.includes('epub-import-test.invalid')) violations.push(event.blockedURI); };
  document.addEventListener('securitypolicyviolation', record);
  try {
    await until('application', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    (await until('Notes tab', () => button('Notes'))).click();
    const vaultButton = await until('vault actions', () => button('Actions for Test'));
    vaultButton.click();
    await until('import action', () => button('Import EPUB…'));
    document.querySelector('[role="menu"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    const input = document.querySelector('input[accept=".epub"]');
    check(input, 'EPUB file picker missing');
    for (const fixture of epubFixtures) {
      step = fixture.name;
      const previousEditor = document.querySelector('.ProseMirror');
      const previousText = previousEditor?.textContent;
      select(input, fixture);
      const folder = await until('imported folder', () => row(fixture.name));
      check(folder.getAttribute('aria-expanded') === 'true', 'imported folder is collapsed');
      const first = await until('first file', () => row('001. Introduction'));
      check(row('002. Second'), 'second spine file missing');
      check(!row('003. Introduction'), 'repeated spine file imported twice');
      check(document.querySelector('.ProseMirror') === previousEditor
        && previousEditor?.textContent === previousText, 'import changed the open note');
      first.click();
      const editor = await until('read-only imported note', () => {
        const node = document.querySelector('.ProseMirror[contenteditable="false"]');
        return node !== previousEditor && node?.textContent.includes('Readable') && node;
      });
      check(document.querySelector('textarea[aria-label="Prompt"]').disabled, 'read-only prompt enabled');
      editor.focus();
      check(document.activeElement === editor, 'read-only note cannot receive keyboard focus');
      check(editor.querySelector('h1')?.textContent === 'Original heading', 'original heading changed');
      check(editor.querySelector('strong')?.textContent === 'bold', 'bold formatting lost');
      check(editor.querySelector('pre')?.textContent === '  code\nline', 'code block changed');
      check(editor.textContent.includes('Left\tRight'), 'table text lost');
      check(editor.querySelectorAll('a').length === 1
        && editor.querySelector('a').getAttribute('href') === 'https://example.com/source', 'link policy changed');
      check(!editor.querySelector('img:not(.ProseMirror-separator), svg')
        && !editor.textContent.includes('Omitted SVG'), 'media stored in note');
      check(!window.__epubScriptRan, 'book script executed');
      button('Actions for 001. Introduction').click();
      (await until('unlock action', () => button('Make editable'))).click();
      await until('editable note', () => editor.contentEditable === 'true');
      editor.focus();
      const selection = window.getSelection();
      const range = document.createRange();
      range.selectNodeContents(editor);
      range.collapse(false);
      selection.removeAllRanges();
      selection.addRange(range);
      check(document.execCommand('insertText', false, ' Edited EPUB note'), 'note edit failed');
      // Let the editor's DOM observer process the native edit before navigation.
      await wait(25);
      row('002. Second').click();
      await until('second note', () => document.querySelector('.ProseMirror')?.textContent === 'Second');
      await wait(25);
      row('001. Introduction').click();
      await until('saved imported note', () => document.querySelector('.ProseMirror')?.textContent.includes('Edited EPUB note'));
      button('Actions for 001. Introduction').click();
      (await until('lock action', () => button('Make read-only'))).click();
      await until('locked note', () => document.querySelector('.ProseMirror[contenteditable="false"]')
        && document.querySelector('textarea[aria-label="Prompt"]').disabled
        && button('Actions for 001. Introduction'));
      row('002. Second').click();
      await until('second note after locking', () => document.querySelector('.ProseMirror')?.textContent === 'Second');
      row('001. Introduction').click();
      await until('reopened locked note', () => {
        const node = document.querySelector('.ProseMirror[contenteditable="false"]');
        return node?.textContent.includes('Edited EPUB note');
      });
      // Collapse this book so the next fixture's duplicate note names are unambiguous.
      row(fixture.name).click();
      await until('folder collapsed', () => row(fixture.name)?.getAttribute('aria-expanded') === 'false');
      completed.push(fixture.name);
    }
    await wait(100);
    check(violations.length === 0, 'book image triggered a resource request');
    return { ok: true, completed, checks: 'TOC names, spine order, XHTML, formatting, media removal, unlock/edit/save/lock/reopen' };
  } catch (error) {
    return { ok: false, step, completed, reason: String(error.message || error) };
  } finally {
    document.removeEventListener('securitypolicyviolation', record);
  }
}
