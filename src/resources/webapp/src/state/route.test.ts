import { describe, expect, it } from 'vitest';

import { parseAppRoute, sessionRoute } from './route';

describe('application routes', () => {
  it('recognizes the root and a stored-session route', () => {
    expect(parseAppRoute('/')).toEqual({ kind: 'root' });
    expect(parseAppRoute('/s/the-lobby/2026-08-06_session.1/')).toEqual({
      kind: 'session',
      forumId: 'the-lobby',
      sessionId: '2026-08-06_session.1',
    });
  });

  it.each([
    '/s/forum/session',
    '/s/forum/session/extra/',
    '/s/forum//',
    '/s/../session/',
    '/s/forum/a%2Fb/',
    '/other',
  ])('rejects the invalid path %s', (pathname) => {
    expect(parseAppRoute(pathname)).toEqual({ kind: 'invalid' });
  });

  it('formats the canonical route with its required trailing slash', () => {
    expect(sessionRoute('lobby', 'planning')).toBe('/s/lobby/planning/');
    expect(() => sessionRoute('not/safe', 'planning')).toThrow(TypeError);
  });
});
