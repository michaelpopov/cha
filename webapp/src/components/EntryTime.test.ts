import { describe, expect, it } from 'vitest';

import { formatEntryTime, formatTokenUsage } from './ChatScreen';

// Local-time constructor values: an entry from this morning and one from
// last year, both relative to a local noon.
const now = new Date(2026, 7, 6, 12, 0, 0).getTime();
const sameMorning = new Date(2026, 7, 6, 8, 30, 0);
const lastYear = new Date(2025, 1, 1, 9, 0, 0);

function seconds(date: Date): number {
  return Math.floor(date.getTime() / 1000);
}

function timeLabel(date: Date): string {
  return date.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
}

describe('entry time', () => {
  it('formats entry dates and times, including the year only for older years', () => {
    for (const date of [sameMorning, lastYear]) {
      const label = formatEntryTime(seconds(date), now);
      expect(label.endsWith(timeLabel(date)), date.toISOString()).toBe(true);
      expect(label).not.toBe(timeLabel(date));
      expect(label).not.toContain('2026');
      if (date === lastYear) expect(label).toContain('2025');
    }
  });

});

describe('token usage', () => {
  it('shows exact small totals and rounded whole thousands', () => {
    expect(formatTokenUsage(842)).toBe('842');
    expect(formatTokenUsage(1_000)).toBe('1K');
    expect(formatTokenUsage(55_600)).toBe('56K');
  });
});
