import { expect, it } from 'vitest';

import { bootstrapFixture } from '../test/fixtures';
import {
  defaultForumId,
  parseChawebHash,
  sameChawebPlace,
  sessionHash,
  sessionUnavailable,
  visibleForums,
  noteHash,
  notesHash,
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

it('parses note and folder identities and generates canonical decimal links', () => {
  expect(parseChawebHash('#/notes')).toEqual({ kind: 'notes' });
  expect(parseChawebHash(notesHash('root'))).toEqual({ kind: 'notes', folder: 'root' });
  expect(parseChawebHash(notesHash(42))).toEqual({ kind: 'notes', folder: 42 });
  expect(parseChawebHash(noteHash(9007199254740991))).toEqual({ kind: 'note', id: 9007199254740991 });
  expect(parseChawebHash('#/notes/items/00042')).toEqual({ kind: 'note', id: 42 });
  expect(sameChawebPlace('#/notes/items/00042', noteHash(42))).toBe(true);
  expect(sameChawebPlace(notesHash(42), notesHash(43))).toBe(false);
  expect(sameChawebPlace(noteHash(42), notesHash(42))).toBe(false);
  for (const id of ['0', '-1', '+1', '1.2', '1e2', '9007199254740992', '%31', '%', '1/extra', '1/']) {
    expect(parseChawebHash(`#/notes/items/${id}`)).toEqual({ kind: 'unknown' });
    expect(parseChawebHash(`#/notes/folders/${id}`)).toEqual({ kind: 'unknown' });
  }
  expect(parseChawebHash('#/notes/items/root')).toEqual({ kind: 'unknown' });
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

it('includes Welcome and prefers an ordinary default forum when one is available', () => {
  expect(visibleForums(bootstrapFixture).map((forum) => [forum.id, forum.display_name]))
    .toEqual([['entrance', 'Welcome'], ['lobby', 'The Lobby']]);
  expect(defaultForumId(bootstrapFixture)).toBe('lobby');
  expect(defaultForumId({ ...bootstrapFixture, forums: [bootstrapFixture.forums[0]] })).toBe('entrance');
  expect(sessionUnavailable(bootstrapFixture, 'entrance', 'welcome')).toBe(true);
  expect(sessionUnavailable(bootstrapFixture, 'entrance', 'builtin-welcome')).toBe(false);
  expect(sessionUnavailable(bootstrapFixture, 'lobby', 'builtin-welcome')).toBe(true);
  expect(sessionUnavailable(bootstrapFixture, 'missing', 'planning')).toBe(true);
  expect(sessionUnavailable(bootstrapFixture, 'lobby', 'planning')).toBe(false);
});
