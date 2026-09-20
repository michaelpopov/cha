import { describe, expect, it } from 'vitest';

import { isAppendEvent } from './events';

describe('session event payloads', () => {
  it('accepts typed entry and reasoning appends', () => {
    expect(isAppendEvent({
      target: { kind: 'entry', entry_id: 4 }, text: 'hello', seq: 0,
    })).toBe(true);
    expect(isAppendEvent({
      target: { kind: 'reasoning', request_id: 9 }, text: 'thinking', seq: 1,
    })).toBe(true);
  });

  it('rejects malformed payloads and sequence numbers', () => {
    expect(isAppendEvent({ target: { kind: 'entry' }, text: 'missing id', seq: 0 }))
      .toBe(false);
    expect(isAppendEvent({ target: { kind: 'entry', entry_id: 4 }, text: 7, seq: 0 }))
      .toBe(false);
    expect(isAppendEvent({
      target: { kind: 'reasoning', request_id: 9 }, text: 'old', seq: -1,
    })).toBe(false);
  });
});
