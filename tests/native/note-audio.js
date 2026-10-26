// Run with CHA_NATIVE_PARITY_SCRIPT against a disposable vault containing the
// read-only "Audio check" note and a configured local fake audio service.
async function nativeParity({ offline = false } = {}) {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const until = async (label, condition) => {
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
      const result = condition();
      if (result) return result;
      await wait(25);
    }
    throw new Error('Timed out waiting for ' + label + ': ' + document.body.innerText.slice(-1200));
  };
  const button = (name, root = document) => [...root.querySelectorAll('button,[role="tab"]')]
    .find(node => node.getClientRects().length && (node.getAttribute('aria-label') ?? node.textContent).trim() === name);
  const click = async (name, root = document) => (await until(name, () => {
    const node = button(name, root); return node && !node.disabled && node;
  })).click();
  const openMenu = async target => {
    const bounds = target.getBoundingClientRect();
    target.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: bounds.left + 8, clientY: bounds.top + 8 }));
    return until('context menu', () => document.querySelector('[role="menu"]'));
  };
  const noteRow = () => [...document.querySelectorAll('[role="treeitem"]')].find(node => node.textContent.includes('Audio check'));
  const editor = () => document.querySelector('.ProseMirror')?.chaEditor;
  const completed = [];
  try {
    await until('startup', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    (await until('audio note', noteRow)).click();
    await until('read-only note', () => editor() && !editor().isEditable && editor().getText().includes('Second paragraph'));
    if (!offline) {
      await click('Actions for Audio check');
      const menu = await until('note actions', () => document.querySelector('[role="menu"]'));
      await click('Generate audio', menu);
      const dialog = await until('audio dialog', () => document.querySelector('dialog[aria-label="Generate note audio"]'));
      const voice = dialog.querySelector('select');
      await until('loaded voice selector', () => !voice.disabled);
      check(voice.value === '', 'voice was preselected');
      check([...voice.options].slice(1).map(option => option.text).join(',') === 'boroda,grant,dzig,gerdt,Kore', 'unexpected voice labels');
      check(!dialog.querySelector('h1,h2,h3'), 'redundant dialog heading');
      Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(voice, 'Kore'); voice.dispatchEvent(new Event('change', { bubbles: true }));
      await click('Generate', dialog);
      await until('accepted setup closed', () => !dialog.isConnected);
      check(!document.querySelector('dialog[aria-label="Generate note audio"]'), 'setup remained open');
      await click('Chat');
      await click('Settings', document.querySelector('aside'));
      await until('Settings during generation', () => document.querySelector('section[aria-label="Settings"]'));
      await click('Notes');
      (await until('audio note', noteRow)).click();
      await until('saved note reopened', () => editor() && !editor().isEditable);
      // Opening the menu is a visible availability probe; wait for the completion
      // notification to make Play available without opening another modal.
      await until('audio playback available', () => {
        const block = editor().view.dom.querySelector('p');
        if (!document.querySelector('[role="menu"]')) {
          const bounds = block.getBoundingClientRect();
          block.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: bounds.left + 8, clientY: bounds.top + 8 }));
        }
        const menu = document.querySelector('[role="menu"]');
        if (button('Play from here', menu ?? document)) return true;
        menu?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
        return false;
      });
      document.querySelector('[role="menu"]')?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
      await until('menu closed', () => !document.querySelector('[role="menu"]'));
      check(!document.querySelector('.cha-note-audio-current'), 'generation autoplayed');
      completed.push('background acceptance, completion notice, and native commit');
  
    } else completed.push('saved audio available with service offline');

    const blocks = editor().view.dom.querySelectorAll('h1,p');
    const reading = editor().view.dom;
    let playbackMenu = await openMenu(blocks[1]);
    await click('Play from here', playbackMenu);
    await until('middle clip playback', () => blocks[1].classList.contains('cha-note-audio-current'));
    check(reading.contains(document.activeElement), 'reading area did not regain focus');
    const key = repeat => new KeyboardEvent('keydown', { key: ' ', code: 'Space', repeat, bubbles: true, cancelable: true });
    const pause = key(false); reading.dispatchEvent(pause);
    check(pause.defaultPrevented, 'Space scrolled instead of pausing');
    reading.dispatchEvent(key(true));
    await wait(300);
    check(blocks[1].classList.contains('cha-note-audio-current'), 'pause removed current highlight');
    playbackMenu = await openMenu(blocks[0]);
    check(button('Stop playing', playbackMenu), 'paused playlist lost Stop action');
    check(!button('Play from here', playbackMenu), 'paused playlist offered a new start');
    playbackMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    await until('menu closed', () => !document.querySelector('[role="menu"]'));
    reading.dispatchEvent(key(false));
    await until('next clip playback', () => blocks[2].classList.contains('cha-note-audio-current'));
    await until('end of note', () => !document.querySelector('.cha-note-audio-current'));
    completed.push('M4A playback, Space pause/resume, and ordered completion');

    playbackMenu = await openMenu(blocks[0]); await click('Play from here', playbackMenu);
    await until('heading playback', () => blocks[0].classList.contains('cha-note-audio-current'));
    playbackMenu = await openMenu(blocks[2]); await click('Stop playing', playbackMenu);
    check(!document.querySelector('.cha-note-audio-current'), 'Stop retained highlight');
    reading.dispatchEvent(key(false)); await wait(150);
    check(!document.querySelector('.cha-note-audio-current'), 'Space resumed a stopped playlist');
    await click('Actions for Audio check');
    const reopen = await until('note actions', () => document.querySelector('[role="menu"]'));
    await click('Generate audio', reopen);
    const again = await until('reopened dialog', () => document.querySelector('dialog[aria-label="Generate note audio"]'));
    await until('fresh voice', () => !again.querySelector('select').disabled);
    check(again.querySelector('select').value === '', 'previous voice was recalled');
    check(again.textContent.includes('Existing audio will be replaced after successful generation.'), 'replacement consequence missing');
    await click('Close', again);
    completed.push('explicit Stop and fresh voice on reopen');
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error), alerts: [...document.querySelectorAll('[role="alert"]')].map(node => node.textContent) }; }
}

// Uses the same disposable native host and mock service as the single-note test.
async function nativeFolderAudio() {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const until = async (label, condition) => {
    const deadline = Date.now() + 12000;
    while (Date.now() < deadline) {
      const result = condition();
      if (result) return result;
      await wait(25);
    }
    throw new Error('Timed out waiting for ' + label + ': ' + document.body.innerText.slice(-1600));
  };
  const button = (name, root = document) => [...root.querySelectorAll('button,[role="tab"]')]
    .find(node => node.getClientRects().length && (node.getAttribute('aria-label') ?? node.textContent).trim() === name);
  const click = async (name, root = document) => (await until(name, () => {
    const node = button(name, root); return node && !node.disabled && node;
  })).click();
  const openFolder = async (name, count) => {
    await click('Actions for ' + name);
    const menu = await until('folder menu', () => document.querySelector('[role="menu"]'));
    const labels = [...menu.querySelectorAll('button')].map(node => node.textContent);
    check(labels.indexOf('Generate audio') + 1 === labels.indexOf('Compress'), 'folder action order');
    await click('Generate audio', menu);
    const dialog = await until('folder dialog', () => document.querySelector('dialog[aria-label="Generate note audio"]'));
    await until('eligibility', () => dialog.textContent.includes(count + ' notes to generate'));
    const voice = dialog.querySelector('select');
    check(voice.value === '', 'folder voice was recalled');
    check(document.activeElement === voice, 'voice did not get focus');
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(voice, 'Kore');
    voice.dispatchEvent(new Event('change', { bubbles: true }));
    await click('Generate', dialog);
    return dialog;
  };
  const closeFolder = async (dialog, name) => {
    await click('Close', dialog);
    await until('dialog closed', () => !dialog.isConnected);
    check(document.activeElement?.getAttribute('role') === 'treeitem'
      && document.activeElement.textContent.includes(name), 'folder focus was not restored');
  };
  const completed = [];
  try {
    await until('startup', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    const success = await openFolder('Folder success', 4);
    check(button('Close', success).disabled && success.querySelector('select').disabled, 'running controls not locked');
    const escape = new Event('cancel', { cancelable: true }); success.dispatchEvent(escape);
    check(escape.defaultPrevented && success.open, 'Escape closed a folder run');
    await until('folder completion', () => success.textContent.includes('2 of 4 notes complete · 2 skipped') && !button('Close', success).disabled);
    check(success.querySelector('progress').value === 4, 'folder progress is not complete');
    check(success.textContent.includes('B empty') && success.textContent.includes('No text to narrate'), 'empty note skip missing');
    check(success.textContent.includes('C oversized') && success.textContent.includes('1–5,000 Unicode code points'), 'size skip missing');
    check(success.textContent.includes('Audio ready'), 'saved notes not ready');
    const elapsed = success.querySelector('[role="timer"]').textContent;
    await wait(1100);
    check(success.querySelector('[role="timer"]').textContent === elapsed, 'folder terminal time kept running');
    await closeFolder(success, 'Folder success');
    completed.push('sequential folder saves, fixed eligibility, skips, Escape, and focus');

    const stopped = await openFolder('Folder stop', 2);
    await until('download active', () => stopped.textContent.includes('Downloading'));
    await click('Stop', stopped);
    await until('stopping', () => stopped.textContent.includes('Stopping…'));
    check(button('Close', stopped).disabled && button('Stop', stopped).disabled, 'stopping controls enabled');
    await until('stopped', () => stopped.textContent.includes('Folder run stopped') && !button('Close', stopped).disabled);
    check(stopped.querySelector('progress').value === 0, 'cancelled note was counted');
    await closeFolder(stopped, 'Folder stop');
    completed.push('native Stop waits for terminal status and prevents later notes');

    const failed = await openFolder('Folder error', 3);
    await until('first error', () => failed.textContent.includes('Folder run stopped') && !button('Close', failed).disabled);
    check(failed.querySelector('progress').value === 1, 'saved note was not retained after error');
    check(failed.textContent.includes('B failing') && failed.textContent.includes('The audio batch failed'), 'folder error missing');
    await closeFolder(failed, 'Folder error');
    completed.push('first error stops remaining notes and preserves earlier saves');
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error) }; }
}
