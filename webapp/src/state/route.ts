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

export function currentAppRoute(
  location: Pick<Location, 'hash'> = window.location,
): AppRoute {
  const raw = location.hash.startsWith('#') ? location.hash.slice(1) : '';
  return parseAppRoute(raw === '' ? '/' : raw);
}

export function appHref(
  path: string,
  location: Pick<Location, 'pathname' | 'search'> = window.location,
): string {
  return `${location.pathname}${location.search}#${path}`;
}

export function writeAppRoute(
  path: string,
  mode: 'push' | 'replace' = 'push',
  location: Pick<Location, 'pathname' | 'search'> = window.location,
): void {
  const href = appHref(path, location);
  if (mode === 'replace') window.history.replaceState(null, '', href);
  else window.history.pushState(null, '', href);
}

// Hash hrefs are same-document. Native post-maintenance reload must replace
// the document, not only the fragment.
export function reloadApplication(
  location: Pick<Location, 'reload'> = window.location,
): void {
  location.reload();
}
