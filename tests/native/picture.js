// Tiny fixtures verify native decoding, without network access.
const pictureFixtures = [
  {
    "mime": "image/png",
    "base64": "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAMCAIAAADQ/GvKAAAAE0lEQVR4nGP8z4AdMOEQZxi5EgA5IAEXVaZ0AAAAAABJRU5ErkJggg=="
  },
  {
    "mime": "image/jpeg",
    "base64": "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAMAAgDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDi6KKK+ZP3E//Z"
  },
  {
    "mime": "image/gif",
    "base64": "R0lGODlhCAAMAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQADAAAACwAAAAACAAMAAAIEgABCBxIsKDBgwgTKlzIsGHBgAAh+QQBDAABACwAAAAACAAMAIEAAP8AAAAAAAAAAAAIEgABCBxIsKDBgwgTKlzIsGHBgAA7"
  },
  {
    "mime": "image/webp",
    "base64": "UklGRsQAAABXRUJQVlA4WAoAAAACAAAABwAACwAAQU5JTQYAAAAAAAAAAABBTk1GSgAAAAAAAAAAAAcAAAsAAHgAAAJWUDggMgAAADABAJ0BKggADAABQCYloAADcAD+8ut///mwP/bz/wR6Af//0uD//pcH//S4P/SkAAAAQU5NRkYAAAAAAAAAAAAHAAALAAB4AAAAVlA4IC4AAAA0AQCdASoIAAwAAAAmJaAAA3AA/vtV4///S4P/+lwf/9Lg/9Lg//rV5Vesq6AA"
  }
];

// Run with the parity harness against a disposable vault. On macOS select this
// script via CHA_NATIVE_PARITY_SCRIPT and repeat with CHA_NATIVE_TEST_WIDTH set
// to 1200, 512, 430, 400 and 320. No provider or shared vault is used.
async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const until = async (condition) => {
    const deadline = Date.now() + 5000;
    while (!condition()) {
      if (Date.now() > deadline) throw new Error('picture UI did not become ready');
      await wait(25);
    }
  };
  const toggle = (name) => document.querySelector(`button[aria-label="${name}"]`);
  const bounds = (element) => element.getBoundingClientRect();
  try {
    await until(() => toggle('Hide picture') && document.querySelector('textarea:not(:disabled)'));
    await wait(300);
    for (const fixture of pictureFixtures) {
      const image = new Image();
      image.src = `data:${fixture.mime};base64,${fixture.base64}`;
      document.body.append(image);
      try {
        await image.decode();
        check(image.naturalWidth === 8 && image.naturalHeight === 12, `${fixture.mime} decode failed`);
      } finally { image.remove(); }
    }
    const assertLayout = async (sidebarOpen, pictureOpen) => {
      await wait(300);
      const chat = document.querySelector('[aria-label="Chat area"]');
      const panel = document.querySelector('.cha-picture');
      const area = chat.parentElement;
      const style = getComputedStyle(area);
      const available = area.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
      const displayed = pictureOpen ? Math.min(280, Math.max(0, available - 320)) : 0;
      check(Math.abs((panel ? bounds(panel).width : 0) - displayed) < 1, 'picture width formula failed');
      const content = document.querySelector('.cha-picture-content');
      check(Math.abs((content ? bounds(content).width : 0) - displayed) < 1, 'picture content exceeds panel width');
      if (sidebarOpen && innerWidth < 512) {
        check(Math.abs(bounds(document.querySelector('[aria-label="Sidebar"]')).width - 192) < 1, 'narrow sidebar width failed');
        check(displayed === 0, 'narrow picture did not reach zero');
      }
      for (const name of [sidebarOpen ? 'Hide sidebar' : 'Show sidebar', pictureOpen ? 'Hide picture' : 'Show picture']) {
        const button = toggle(name);
        const rect = bounds(button);
        check(rect.width >= 20 && rect.left >= bounds(chat).left && rect.right <= bounds(chat).right + 1,
          `${name} clipped or shrunk at ${innerWidth}`);
        check(button.getAttribute('aria-expanded') === String(name.startsWith('Hide')), `${name} state failed`);
      }
      check(bounds(chat).right <= innerWidth + 1, 'chat overflows viewport');
      check(Boolean(document.querySelector('[aria-label="Resize picture"]')) === (pictureOpen && displayed > 0), 'divider visibility failed');
    };
    await assertLayout(true, true);
    toggle('Hide picture').click();
    await assertLayout(true, false);
    toggle('Hide sidebar').click();
    await assertLayout(false, false);
    toggle('Show picture').click();
    await assertLayout(false, true);
    toggle('Show sidebar').click();
    await assertLayout(true, true);
    if (innerWidth >= 1000) {
      const input = document.querySelector('textarea[aria-label="Message"]');
      const transcript = document.querySelector('[aria-label="Conversation transcript"]');
      const setValue = (element, value) => {
        const textarea = element.tagName === 'TEXTAREA';
        const prototype = textarea ? HTMLTextAreaElement.prototype : HTMLSelectElement.prototype;
        Object.getOwnPropertyDescriptor(prototype, 'value').set.call(element, value);
        element.dispatchEvent(new Event(textarea ? 'input' : 'change', {bubbles: true}));
      };
      const resizePicture = async (key, count = 1) => {
        for (let i = 0; i < count; i++) {
          document.querySelector('[aria-label="Resize picture"]').dispatchEvent(
            new KeyboardEvent('keydown', {key, bubbles: true}));
          await wait(25);
        }
      };
      const gap = () => transcript.scrollHeight - transcript.clientHeight - transcript.scrollTop;
      const latestVisible = () => bounds(transcript.lastElementChild).bottom <= bounds(transcript).bottom + 1;
      toggle('Hide picture').click();
      await wait(300);
      setValue(input, 'A moderately long draft should expand automatically when its available width changes. '.repeat(8));
      await wait(100);
      const wideHeight = input.clientHeight;
      toggle('Show picture').click();
      await wait(100);
      check(input.clientHeight > wideHeight && input.scrollHeight <= input.clientHeight + 1,
        'draft did not reflow after opening the picture');
      await resizePicture('ArrowLeft', 8);
      check(input.scrollHeight <= input.clientHeight + 1, 'draft did not reflow after resizing the picture');
      await resizePicture('ArrowRight', 8);
      toggle('Hide picture').click();
      await wait(100);
      check(Math.abs(input.clientHeight - wideHeight) <= 1, 'draft did not shrink after closing the picture');

      setValue(input, '');
      setValue(document.querySelector('select[aria-label="Choose message target"]'), '-');
      await until(() => input.placeholder.startsWith('Self-notes'));
      const note = 'Long transcript line that needs to wrap when the picture panel opens. '.repeat(90);
      setValue(input, note);
      await wait(100);
      toggle('Send message').click();
      await until(() => input.value === '' && transcript.textContent.includes(note.trim()));
      transcript.scrollTop = transcript.scrollHeight;
      await wait(100);
      check(transcript.scrollHeight > transcript.clientHeight, 'transcript fixture does not scroll');
      toggle('Show picture').click();
      await wait(100);
      check(latestVisible(), 'opening the picture lost the latest transcript text');
      await resizePicture('ArrowLeft', 8);
      check(latestVisible(), 'resizing the picture lost the latest transcript text');
      transcript.scrollTop = 0;
      await wait(100);
      await resizePicture('ArrowRight', 8);
      check(gap() > 100, 'resizing the picture pulled a reader away from older text');
    }
    return {ok: true, width: innerWidth, formats: 'PNG JPEG GIF WebP', layout: 'both open, each closed, restored'};
  } catch (error) { return {ok: false, reason: String(error.message || error)}; }
}
