import { describe, expect, it, vi } from 'vitest';

import {
  appHref,
  currentAppRoute,
  parseAppRoute,
  reloadApplication,
  sessionRoute,
  usesHashRoutes,
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

  it('keeps path routing on the HTTP frontend and hash routing on native origins', () => {
    expect(usesHashRoutes({ protocol: 'http:' })).toBe(false);
    expect(usesHashRoutes({ protocol: 'https:' })).toBe(true);
    expect(usesHashRoutes({ protocol: 'cha:' })).toBe(true);
    expect(appHref('/s/lobby/planning/', {
      protocol: 'http:', pathname: '/', search: '',
    })).toBe('/s/lobby/planning/');
    expect(appHref('/s/lobby/planning/', {
      protocol: 'cha:', pathname: '/', search: '',
    })).toBe('/#/s/lobby/planning/');
    expect(currentAppRoute({
      protocol: 'http:', pathname: '/s/lobby/planning/', hash: '',
    })).toEqual({ kind: 'session', forumId: 'lobby', sessionId: 'planning' });
    expect(currentAppRoute({
      protocol: 'cha:', pathname: '/', hash: '#/s/lobby/planning/',
    })).toEqual({ kind: 'session', forumId: 'lobby', sessionId: 'planning' });
    expect(currentAppRoute({
      protocol: 'https:', pathname: '/', hash: '#/',
    })).toEqual({ kind: 'root' });
  });

  it('writes fragment history on native origins and path history on HTTP', () => {
    const http = { protocol: 'http:' as const, pathname: '/', search: '' };
    writeAppRoute('/s/lobby/planning/', 'push', http);
    expect(`${window.location.pathname}${window.location.search}`).toBe('/s/lobby/planning/');

    window.history.replaceState(null, '', '/shell');
    writeAppRoute('/s/lobby/planning/', 'replace', {
      protocol: 'cha:', pathname: '/shell', search: '',
    });
    expect(window.location.pathname).toBe('/shell');
    expect(window.location.hash).toBe('#/s/lobby/planning/');
  });

  it('reloads the document on native origins instead of assigning a fragment', () => {
    const http = {
      protocol: 'http:' as const,
      assign: vi.fn(),
      reload: vi.fn(),
    };
    reloadApplication(http);
    expect(http.assign).toHaveBeenCalledWith('/');
    expect(http.reload).not.toHaveBeenCalled();

    const native = {
      protocol: 'cha:' as const,
      assign: vi.fn(),
      reload: vi.fn(),
    };
    reloadApplication(native);
    expect(native.reload).toHaveBeenCalledOnce();
    expect(native.assign).not.toHaveBeenCalled();
  });
});
