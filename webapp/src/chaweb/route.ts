import type { Bootstrap } from '../api/client';
import { welcomeSessionId } from '../state/route';

const identifier = /^[A-Za-z0-9._~-]+$/;

export type ChawebHash =
  | { kind: 'list' }
  | { kind: 'session'; forumId: string; sessionId: string }
  | { kind: 'unknown' };

export function parseChawebHash(hash: string): ChawebHash {
  const raw = hash.startsWith('#') ? hash.slice(1) : hash;
  if (raw === '' || raw === '/') return { kind: 'list' };
  const match = /^\/forums\/([^/]+)\/sessions\/([^/]+)$/.exec(raw);
  if (!match) return { kind: 'unknown' };
  try {
    const forumId = decodeURIComponent(match[1]);
    const sessionId = decodeURIComponent(match[2]);
    if (!identifier.test(forumId) || !identifier.test(sessionId)) return { kind: 'unknown' };
    return { kind: 'session', forumId, sessionId };
  } catch {
    return { kind: 'unknown' };
  }
}

export function sessionHash(forumId: string, sessionId: string): string {
  return `#/forums/${encodeURIComponent(forumId)}/sessions/${encodeURIComponent(sessionId)}`;
}

export function sameChawebPlace(left: string, right: string): boolean {
  const a = parseChawebHash(left);
  const b = parseChawebHash(right);
  if (a.kind !== b.kind) return false;
  if (a.kind === 'session' && b.kind === 'session') {
    return a.forumId === b.forumId && a.sessionId === b.sessionId;
  }
  if (a.kind === 'list' && b.kind === 'list') return true;
  return left === right;
}

export function visibleForums(bootstrap: Bootstrap) {
  return bootstrap.forums.filter((forum) => forum.id !== bootstrap.entrance_forum_id);
}

export function defaultForumId(bootstrap: Bootstrap): string {
  const forums = visibleForums(bootstrap);
  return forums.find((forum) => forum.id === bootstrap.initial_forum_id)?.id ?? forums[0]?.id ?? '';
}

export function sessionUnavailable(
  bootstrap: Bootstrap,
  forumId: string,
  sessionId: string,
): boolean {
  if (sessionId === welcomeSessionId) return true;
  if (forumId === bootstrap.entrance_forum_id) return true;
  return !bootstrap.forums.some((forum) => forum.id === forumId);
}
