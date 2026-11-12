// Run with CHA_NATIVE_PARITY_SCRIPT against a disposable vault containing the
// read-only "Audio check" note and a configured local fake audio service.
async function nativeParity({ offline = false, catalogOffline = false, summary } = {}) {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const until = async (label, condition) => {
    const deadline = Date.now() + (summary ? 12000 : 30000);
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
      check([...voice.options].slice(1).map(option => option.text).join(',')
        === 'Achernar · Female · en-US · General American,Kore · en-US', 'unexpected voice labels');
      check(!dialog.querySelector('h1,h2,h3'), 'redundant dialog heading');
      Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(voice, 'kore'); voice.dispatchEvent(new Event('change', { bubbles: true }));
      await click('Generate', dialog);
      await until('accepted setup closed', () => !dialog.isConnected);
      check(!document.querySelector('dialog[aria-label="Generate note audio"]'), 'setup remained open');
      await click('Chat');
      await until('Chat after saving note position', () => document.querySelector('main[data-view="chat"]'));
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
    if (catalogOffline) {
      await until('offline catalog error', () => again.querySelector('[role="alert"]')?.textContent.includes('Could not load voices'));
      check(again.querySelector('select').disabled && button('Generate', again).disabled, 'offline catalog allowed generation');
    } else await until('fresh voice', () => !again.querySelector('select').disabled);
    check(again.querySelector('select').value === '', 'previous voice was recalled');
    check(again.textContent.includes('Existing audio will be replaced after successful generation.'), 'replacement consequence missing');
    await click('Close', again);
    completed.push('explicit Stop and fresh voice on reopen');

    if (summary) {
      const chooseMode = async label => {
        await click('Note text mode');
        await click(label, await until('mode menu', () => document.querySelector('[role="menu"][aria-label="Note text mode"]')));
      };
      await chooseMode('L1');
      await until('L1 document', () => editor()?.getText() === 'Summary narration.');
      if (summary === 'generate') {
        check(!noteRow().classList.contains('has-audio'), 'L1 mode tinted full-text audio');
        await click('Actions for Audio check');
        await click('Generate audio', await until('L1 actions', () => document.querySelector('[role="menu"]')));
        const setup = await until('L1 audio dialog', () => document.querySelector('dialog[aria-label="Generate note audio"]'));
        const voice = setup.querySelector('select');
        await until('L1 voice selector', () => !voice.disabled);
        check(!setup.textContent.includes('Existing audio'), 'full-text clips counted as L1 clips');
        Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(voice, 'kore');
        voice.dispatchEvent(new Event('change', { bubbles: true }));
        await click('Generate', setup);
        await until('L1 setup closed', () => !setup.isConnected);
      }
      await until('L1 tint', () => noteRow().classList.contains('has-audio'));
      const paragraph = editor().view.dom.querySelector('p');
      playbackMenu = await openMenu(paragraph);
      // Pause as soon as playback starts so a short clip cannot finish between polls.
      const pauseOnPlayback = new MutationObserver(() => {
        if (!paragraph.classList.contains('cha-note-audio-current')) return;
        pauseOnPlayback.disconnect();
        editor().view.dom.dispatchEvent(key(false));
      });
      pauseOnPlayback.observe(paragraph, { attributeFilter: ['class'] });
      try {
        await click('Play from here', playbackMenu);
        await until('L1 playback', () => paragraph.classList.contains('cha-note-audio-current'));
      } finally {
        pauseOnPlayback.disconnect();
      }
      await chooseMode('Full text');
      await until('full text restored', () => editor()?.getText().includes('Second paragraph'));
      check(!paragraph.classList.contains('cha-note-audio-current'), 'mode switch kept L1 playback active');
      await chooseMode('L1');
      await until('L1 restored', () => editor()?.getText() === 'Summary narration.');
      (await until('note with full-text audio only', () => [...document.querySelectorAll('[role="treeitem"]')]
        .find(node => node.textContent.includes('Recovery check')))).click();
      await until('full-text fallback', () => editor()?.getText().includes('Second paragraph'));
      const fallback = editor().view.dom.querySelector('p');
      await click('Play from here', await openMenu(fallback));
      await until('full-text fallback playback', () => fallback.classList.contains('cha-note-audio-current'));
      completed.push('L1 tint, L1 playback, mode switch, and full-text fallback playback');
    }
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error), alerts: [...document.querySelectorAll('[role="alert"]')].map(node => node.textContent) }; }
}

// Uses the same disposable native host and concurrent mock service.
async function nativeFolderAudio({ recover = false } = {}) {
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
  const generate = async name => {
    await click('Actions for ' + name);
    await click('Generate audio', await until('folder menu', () => document.querySelector('[role="menu"]')));
    const dialog = await until('folder dialog', () => document.querySelector('dialog[aria-label="Generate note audio"]'));
    const voice = dialog.querySelector('select');
    await until('eligibility', () => !voice.disabled);
    check(voice.value === '', 'folder voice was recalled');
    check(!dialog.querySelector('progress,[role="timer"]') && !button('Stop', dialog), 'obsolete modal controls');
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(voice, 'kore');
    voice.dispatchEvent(new Event('change', { bubbles: true }));
    await click('Generate', dialog);
    await until('local acceptance closes setup', () => !dialog.isConnected);
    check(document.activeElement?.getAttribute('role') === 'treeitem'
      && document.activeElement.textContent.includes(name), 'folder focus was not restored');
  };
  const completed = [];
  try {
    await until('startup', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    if (!recover) {
      await click('Notes');
      await generate('Folder success');
      // A's download remains gated until this host exits. B must finish first.
      await generate('Folder error');
      completed.push('independent folder jobs accepted and setup closed');
    }
    await click('Settings', document.querySelector('aside'));
    await click('Events');
    let saved = false;
    let eventText = '';
    for (let i = 0; i < 100; ++i) {
      await until('event snapshot', () => document.querySelector('.cha-event-list'));
      const text = document.querySelector('section[aria-label="Events"]').textContent;
      eventText = text;
      const expected = recover ? ['A first', 'D last', 'A saved', 'C remaining'] : ['D last', 'A saved', 'C remaining'];
      saved = expected.every(name => text.includes('Audio ready for “' + name + '”'))
        && text.includes('Audio skipped for “B empty”') && text.includes('Audio skipped for “C oversized”')
        && text.includes('Audio batch read failed for “B failing”');
      if (saved) break;
      await wait(100);
      await click('Settings', document.querySelector('aside'));
      await click('Events');
    }
    check(saved, 'independent completions, skips, or error missing from Events: '
      + eventText);
    if (!recover) check(!eventText.includes('Audio ready for “A first”'), 'A completed before its gate was released');
    check(!document.querySelector('.cha-note-audio-current'), 'completion autoplayed');
    completed.push('Events show independent success, skips, and isolated failure');
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error) }; }
}
