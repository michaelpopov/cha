import { describe, expect, it } from 'vitest';

import { formatSessionTime } from './Screens';

const now = Date.UTC(2026, 7, 6, 12, 0, 0);
const minute = 60;
const hour = 60 * minute;
const day = 24 * hour;

function secondsAgo(elapsed: number): number {
  return Math.floor(now / 1000) - elapsed;
}

describe('compact session time', () => {
  it('counts minutes, hours, and days up to a week', () => {
    expect(formatSessionTime(secondsAgo(0), now)).toBe('Now');
    expect(formatSessionTime(secondsAgo(59), now)).toBe('Now');
    expect(formatSessionTime(secondsAgo(minute), now)).toBe('1m');
    expect(formatSessionTime(secondsAgo(59 * minute), now)).toBe('59m');
    expect(formatSessionTime(secondsAgo(hour), now)).toBe('1h');
    expect(formatSessionTime(secondsAgo(23 * hour), now)).toBe('23h');
    expect(formatSessionTime(secondsAgo(day), now)).toBe('1d');
    expect(formatSessionTime(secondsAgo(6 * day), now)).toBe('6d');
  });

  it('switches to a date beyond a week, carrying the year only outside this one', () => {
    const thisYear = formatSessionTime(secondsAgo(8 * day), now);
    expect(thisYear).not.toMatch(/^(Now|\d+[mhd])$/);
    expect(thisYear).not.toContain('2026');
    expect(formatSessionTime(secondsAgo(400 * day), now)).toContain('2025');
  });

  it('reports a timestamp from the future as Now rather than a negative age', () => {
    expect(formatSessionTime(secondsAgo(-minute), now)).toBe('Now');
  });
});
