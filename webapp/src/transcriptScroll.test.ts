import { expect, it } from 'vitest';
import { scrollTranscript, type PromptFocus, type TranscriptScroll } from './transcriptScroll';
import type { SessionSnapshot } from './api/client';

type Entry = SessionSnapshot['transcript'][number];
const entry = (id: number, kind: Entry['kind'] = 'character', text = 'Text'): Entry => ({
  id, kind, text, participant_id: 'guest', display_name: '', addressed_to: '', addressed_to_name: '',
  status: 'streaming', created_at: null,
});

function viewport() {
  const node = document.createElement('div');
  let top = 0;
  let writes = 0;
  Object.defineProperties(node, {
    scrollHeight: { value: 2000 }, clientHeight: { value: 200 },
    scrollTop: { get: () => top, set: (value: number) => { top = Math.max(0, Math.min(value, 1800)); writes++; } },
  });
  node.getBoundingClientRect = () => ({ top: 10 } as DOMRect);
  let state: TranscriptScroll | null = null;
  return {
    node,
    get writes() { return writes; },
    readAt(offset: number) { top = offset; if (state) state.following = false; },
    update(entries: Entry[], automatic = true, prompt: PromptFocus | null = null, key = 'vault/session') {
      node.replaceChildren();
      let lastHuman: Entry | null = null;
      for (const item of entries) {
        if (item.kind === 'human') {
          if (lastHuman?.text === item.text) continue;
          lastHuman = item;
        }
        const article = document.createElement('article');
        article.dataset.entryId = String(item.id);
        article.getBoundingClientRect = () => ({ top: 10 + item.id * 100 - top } as DOMRect);
        const body = document.createElement('div');
        body.dataset.messageBody = '';
        body.textContent = item.text;
        article.append(body);
        node.append(article);
      }
      state = scrollTranscript(node, state, key, entries, '', automatic, prompt);
      return state;
    },
  };
}

it('reveals first readable text once, preserves manual reading, and ignores completion', () => {
  const view = viewport();
  view.update([entry(1, 'character', '')]);
  view.readAt(0);
  view.update([entry(1, 'character', ' ')]);
  expect(view.node.scrollTop).toBe(0);
  view.update([entry(1)]);
  expect(view.node.scrollTop).toBe(100);
  view.readAt(20);
  view.update([entry(1, 'character', '')]);
  view.update([{ ...entry(1), status: 'complete' }]);
  expect(view.node.scrollTop).toBe(20);
});

it('does not catch up when automatic audio is enabled after readable text', () => {
  const view = viewport();
  view.update([]);
  view.readAt(0);
  const entries = [entry(1)];
  view.update(entries, false);
  const writes = view.writes;
  view.update(entries, true);
  expect(view.writes).toBe(writes);
  expect(view.node.scrollTop).toBe(0);
});

it('targets identical local prompts once, without following hidden multicast copies', () => {
  const view = viewport();
  const original = entry(1, 'human', 'Again');
  view.update([original, entry(2)]);
  view.readAt(0);
  const marker = { key: 'vault/session', afterId: 2 };
  const submitted = [...[original, entry(2)], entry(3, 'human', 'Again'), entry(4)];
  expect(view.update(submitted, false, marker).consumed).toBe(marker);
  expect(view.node.scrollTop).toBe(100);
  view.readAt(30);
  view.update(submitted, false, marker); // Strict Mode/late parent consumption.
  view.update([...submitted, entry(5, 'human', 'Again')], false);
  expect(view.node.scrollTop).toBe(30);
  view.update([...submitted, entry(5, 'human', 'Again')], false, { key: 'vault/session', afterId: 4 });
  expect(view.node.scrollTop).toBe(100);
});

it('prioritizes the creation prompt over a response in the first snapshot', () => {
  const view = viewport();
  const marker = { key: 'vault/session', afterId: 0 };
  view.update([entry(1, 'human'), entry(2)], true, marker);
  expect(view.node.scrollTop).toBe(100);
});

it('does not move on a marker-only render or replay and resets for another vault', () => {
  const view = viewport();
  const entries = [entry(1)];
  view.update(entries);
  const writes = view.writes;
  view.update(entries, true, { key: 'vault/session', afterId: 1 });
  view.update(entries);
  expect(view.writes).toBe(writes);
  view.readAt(0);
  view.update(entries, true, null, 'other-vault/session');
  expect(view.node.scrollTop).toBe(1800);
});

it('ignores failed/cancelled text and clamps placement to the transcript range', () => {
  const view = viewport();
  view.update([]);
  view.readAt(0);
  view.update([{ ...entry(1), status: 'failed' }, { ...entry(2), status: 'cancelled' }]);
  expect(view.node.scrollTop).toBe(0);
  view.update([entry(30)]);
  expect(view.node.scrollTop).toBe(1800);
});
