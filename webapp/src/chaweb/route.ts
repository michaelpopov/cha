import type { Bootstrap } from '../api/client';
import { welcomeSessionId } from '../state/route';

const identifier = /^[A-Za-z0-9._~-]+$/;
export type NotesFolder = number | 'root';
export type ChawebNotesRoute =
  | { kind: 'notes'; folder?: NotesFolder }
  | { kind: 'note'; id: number };

export type ChawebHash =
  | ChawebNotesRoute
  | { kind: 'list' }
  | { kind: 'session'; forumId: string; sessionId: string }
  | { kind: 'unknown' };

export function parseChawebHash(hash: string): ChawebHash {
  const raw = hash.startsWith('#') ? hash.slice(1) : hash;
  if (raw === '' || raw === '/') return { kind: 'list' };
  if (raw === '/notes') return { kind: 'notes' };
  const notes = /^\/notes\/(folders|items)\/([0-9]+|root)$/.exec(raw);
  if (notes) {
    if (notes[1] === 'folders' && notes[2] === 'root') return { kind: 'notes', folder: 'root' };
    if (!/^[0-9]+$/.test(notes[2])) return { kind: 'unknown' };
    const id = Number(notes[2]);
    if (!Number.isSafeInteger(id) || id < 1) return { kind: 'unknown' };
    return notes[1] === 'folders' ? { kind: 'notes', folder: id } : { kind: 'note', id };
  }
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

export function notesHash(folder?: NotesFolder): string {
  return folder === undefined ? '#/notes' : `#/notes/folders/${folder}`;
}

export function noteHash(id: number): string {
  return `#/notes/items/${id}`;
}

export function sameChawebPlace(left: string, right: string): boolean {
  const a = parseChawebHash(left);
  const b = parseChawebHash(right);
  if (a.kind !== b.kind) return false;
  if (a.kind === 'session' && b.kind === 'session') {
    return a.forumId === b.forumId && a.sessionId === b.sessionId;
  }
  if (a.kind === 'list' && b.kind === 'list') return true;
  if (a.kind === 'notes' && b.kind === 'notes') return a.folder === b.folder;
  if (a.kind === 'note' && b.kind === 'note') return a.id === b.id;
  return left === right;
}

export function visibleForums(bootstrap: Bootstrap) {
  return bootstrap.forums.map((forum) => forum.id === bootstrap.entrance_forum_id
    ? { ...forum, display_name: 'Welcome' } : forum);
}

export function defaultForumId(bootstrap: Bootstrap): string {
  const forums = bootstrap.forums.filter((forum) => forum.id !== bootstrap.entrance_forum_id);
  return forums.find((forum) => forum.id === bootstrap.initial_forum_id)?.id
    ?? forums[0]?.id ?? bootstrap.forums[0]?.id ?? '';
}

export function isWelcomeSession(
  bootstrap: Bootstrap,
  forumId: string,
  sessionId: string,
): boolean {
  return forumId === bootstrap.entrance_forum_id
    && sessionId === welcomeSessionId
    && bootstrap.forums.some((forum) => forum.id === forumId);
}

export function sessionUnavailable(
  bootstrap: Bootstrap,
  forumId: string,
  sessionId: string,
): boolean {
  if (isWelcomeSession(bootstrap, forumId, sessionId)) return false;
  if (sessionId === welcomeSessionId) return true;
  if (forumId === bootstrap.entrance_forum_id) return true;
  return !bootstrap.forums.some((forum) => forum.id === forumId);
}
