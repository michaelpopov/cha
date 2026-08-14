import { describe, expect, it } from 'vitest';

import { formatEntryTime } from './Screens';

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
  it('shows the date and the time, omitting the year within the current one', () => {
    const label = formatEntryTime(seconds(sameMorning), now);
    expect(label.endsWith(timeLabel(sameMorning))).toBe(true);
    expect(label).not.toBe(timeLabel(sameMorning));
    expect(label).not.toContain('2026');
  });

  it('carries the year for entries from another year', () => {
    const label = formatEntryTime(seconds(lastYear), now);
    expect(label.endsWith(timeLabel(lastYear))).toBe(true);
    expect(label).toContain('2025');
  });
});
