import { describe, expect, it, vi } from 'vitest';

import {
  appHref,
  currentAppRoute,
  parseAppRoute,
  reloadApplication,
  rememberAppUrl,
  rememberedAppUrl,
  restoreAppUrl,
  sessionRoute,
  writeAppRoute,
} from './route';

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

  it('reads hash routes and keeps the shell path and query when building a link', () => {
    expect(appHref('/s/lobby/planning/', {
      pathname: '/shell', search: '?theme=dark',
    })).toBe('/shell?theme=dark#/s/lobby/planning/');
    expect(currentAppRoute({ hash: '#/s/lobby/planning/' })).toEqual({
      kind: 'session', forumId: 'lobby', sessionId: 'planning',
    });
    expect(currentAppRoute({ hash: '#/' })).toEqual({ kind: 'root' });
    expect(currentAppRoute({ hash: '' })).toEqual({ kind: 'root' });
  });

  it('writes fragment history without changing the document path', () => {
    window.history.replaceState(null, '', '/shell');
    writeAppRoute('/s/lobby/planning/', 'replace', {
      pathname: '/shell', search: '',
    });
    expect(window.location.pathname).toBe('/shell');
    expect(window.location.hash).toBe('#/s/lobby/planning/');
    expect(rememberedAppUrl()).toBe(window.location.href);
  });

  it('restores a captured URL without adding a history entry', () => {
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    rememberAppUrl();
    const previous = rememberedAppUrl();
    const length = window.history.length;
    window.history.replaceState(null, '', '/#/');
    rememberAppUrl();
    restoreAppUrl(previous);
    expect(window.location.hash).toBe('#/s/lobby/planning/');
    expect(rememberedAppUrl()).toBe(previous);
    expect(window.history.length).toBe(length);
  });

  it('does not treat a notes path as a route', () => {
    expect(parseAppRoute('/notes')).toEqual({ kind: 'invalid' });
    expect(parseAppRoute('/s/lobby/planning/notes/')).toEqual({ kind: 'invalid' });
  });

  it('reloads the document', () => {
    const location = { reload: vi.fn() };
    reloadApplication(location);
    expect(location.reload).toHaveBeenCalledOnce();
  });
});
