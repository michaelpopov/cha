// Run with the native parity hook in a disposable vault. No provider is used.
async function nativeParity() {
  const check = (ok, message) => { if (!ok) throw new Error(message); };
  const until = async (predicate) => {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      if (predicate()) return;
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    throw new Error('Timed out waiting for transcript update');
  };
  const setValue = (node, value) => {
    const prototype = node.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLTextAreaElement.prototype;
    Object.getOwnPropertyDescriptor(prototype, 'value').set.call(node, value);
    node.dispatchEvent(new Event(node.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }));
  };
  try {
    await until(() => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    const container = document.querySelector('.cha-transcript');
    const input = document.querySelector('textarea[aria-label="Message"]');
    const target = document.querySelector('select[aria-label="Choose message target"]');
    setValue(target, '-');
    await until(() => target.value === '-');
    const text = Array.from({ length: 150 }, (_, i) => `Scroll test line ${i}`).join('\n');
    const beforeWindow = window.scrollY;
    const send = async () => {
      setValue(input, text);
      await until(() => !document.querySelector('button[aria-label="Send message"]').disabled);
      document.querySelector('button[aria-label="Send message"]').click();
      await until(() => input.value === '');
      await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    };
    await send();
    const article = Array.from(container.querySelectorAll('article')).find((node) => node.textContent.includes('Scroll test line 149'));
    check(article, 'Long prompt is absent');
    const aligned = () => Math.abs(article.getBoundingClientRect().top - container.getBoundingClientRect().top - container.clientTop) <= 2;
    check(aligned(), 'First long prompt is not aligned to the transcript top');
    container.scrollTop = container.scrollHeight;
    container.dispatchEvent(new Event('scroll'));
    await send();
    check(aligned(), 'Repeated prompt did not return to its visible representative');
    check(window.scrollY === beforeWindow, 'Ancestor window moved');
    check(document.querySelector('textarea[aria-label="Message"]') === input, 'Composer was remounted');
    return { ok: true, checks: 'long prompt, identical prompt, transcript-only placement, composer identity' };
  } catch (error) {
    return { ok: false, reason: String(error.message || error) };
  }
}
