import { expect, it } from 'vitest';

import { welcomeSnapshot, welcomeSubmission } from './welcomeTurn';

const key = 'entrance/builtin-welcome';

it('ends an answer once when it starts and ends between two snapshots', () => {
  const opened = welcomeSnapshot(null, key, false);
  expect(opened.ended).toBe(false);
  const sent = welcomeSubmission(opened.turn, key);
  const idle = welcomeSnapshot(sent, key, false);
  expect(idle.ended).toBe(true);
  expect(welcomeSnapshot(idle.turn, key, false).ended).toBe(false);
});

it('ends a visible answer when its session becomes idle', () => {
  const active = welcomeSnapshot(welcomeSnapshot(null, key, false).turn, key, true);
  expect(active.ended).toBe(false);
  expect(welcomeSnapshot(active.turn, key, false).ended).toBe(true);
});

it('does not end an answer for a different session key', () => {
  const sent = welcomeSubmission(null, 'other/builtin-welcome');
  expect(welcomeSnapshot(sent, key, false).ended).toBe(false);
});
