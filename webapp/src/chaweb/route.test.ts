import { expect, it } from 'vitest';

import { bootstrapFixture } from '../test/fixtures';
import {
  defaultForumId,
  parseChawebHash,
  sameChawebPlace,
  sessionHash,
  sessionUnavailable,
  visibleForums,
} from './route';

it('parses stored-session hashes and ignores other fragments', () => {
  expect(parseChawebHash('')).toEqual({ kind: 'list' });
  expect(parseChawebHash('#/')).toEqual({ kind: 'list' });
  expect(parseChawebHash('#/forums/lobby/sessions/planning')).toEqual({
    kind: 'session', forumId: 'lobby', sessionId: 'planning',
  });
  expect(parseChawebHash(sessionHash('a b', 'c/d'))).toEqual({ kind: 'unknown' });
  expect(parseChawebHash('#/forums/lobby')).toEqual({ kind: 'unknown' });
  expect(parseChawebHash('#/new')).toEqual({ kind: 'unknown' });
});

it('treats the same stored session as one place', () => {
  expect(sameChawebPlace('', '#/')).toBe(true);
  expect(sameChawebPlace(
    '#/forums/lobby/sessions/planning',
    sessionHash('lobby', 'planning'),
  )).toBe(true);
  expect(sameChawebPlace(
    '#/forums/lobby/sessions/planning',
    '#/forums/lobby/sessions/other',
  )).toBe(false);
  expect(sameChawebPlace('#/nope', '#/other')).toBe(false);
});

it('hides Entrance and Welcome and ignores the initial session for the default forum', () => {
  expect(visibleForums(bootstrapFixture).map((forum) => forum.id)).toEqual(['lobby']);
  expect(defaultForumId(bootstrapFixture)).toBe('lobby');
  expect(sessionUnavailable(bootstrapFixture, 'entrance', 'welcome')).toBe(true);
  expect(sessionUnavailable(bootstrapFixture, 'lobby', 'builtin-welcome')).toBe(true);
  expect(sessionUnavailable(bootstrapFixture, 'missing', 'planning')).toBe(true);
  expect(sessionUnavailable(bootstrapFixture, 'lobby', 'planning')).toBe(false);
});
