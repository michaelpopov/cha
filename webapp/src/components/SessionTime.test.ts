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
  it('formats relative ages, older dates, and future timestamps', () => {
    const cases: Array<[number, string]> = [
      [0, 'Now'], [59, 'Now'], [minute, '1m'], [59 * minute, '59m'],
      [hour, '1h'], [23 * hour, '23h'], [day, '1d'], [6 * day, '6d'], [-minute, 'Now'],
    ];
    for (const [elapsed, expected] of cases) {
      expect(formatSessionTime(secondsAgo(elapsed), now), `elapsed: ${elapsed}`).toBe(expected);
    }
    const thisYear = formatSessionTime(secondsAgo(8 * day), now);
    expect(thisYear).not.toMatch(/^(Now|\d+[mhd])$/);
    expect(thisYear).not.toContain('2026');
    expect(formatSessionTime(secondsAgo(400 * day), now)).toContain('2025');
  });

});
