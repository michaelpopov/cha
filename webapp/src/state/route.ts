export type AppRoute =
  | { kind: 'root' }
  | { kind: 'session'; forumId: string; sessionId: string }
  | { kind: 'invalid' };

const identifier = /^[A-Za-z0-9._~-]+$/;

function isIdentifier(value: string): boolean {
  return value !== '.' && value !== '..' && identifier.test(value);
}

export function parseAppRoute(pathname: string): AppRoute {
  if (pathname === '/') return { kind: 'root' };

  const match = /^\/s\/([^/]+)\/([^/]+)\/$/.exec(pathname);
  if (!match || !isIdentifier(match[1]) || !isIdentifier(match[2])) {
    return { kind: 'invalid' };
  }

  return { kind: 'session', forumId: match[1], sessionId: match[2] };
}

export function sessionRoute(forumId: string, sessionId: string): string {
  if (!isIdentifier(forumId) || !isIdentifier(sessionId)) {
    throw new TypeError('Session routes require URL-safe identifiers.');
  }
  return `/s/${forumId}/${sessionId}/`;
}
