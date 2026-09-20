import type { CharacterDetail, ForumDetail, PersonaDetail } from '../api/client';
import type { AppState } from './view';

export function rosterSummary(detail: CharacterDetail | PersonaDetail) {
  return {
    id: detail.id,
    display_name: detail.display_name,
    appearance: detail.appearance,
    ...(detail.voice === undefined ? {} : { voice: detail.voice }),
    ...(detail.description === undefined ? {} : { description: detail.description }),
  };
}

export function forumSummary(detail: ForumDetail) {
  const { forum_markdown: _markdown, markdown_files: _files, writable: _writable, ...summary } = detail;
  return summary;
}

// Keep discovery and live presentation in sync after an edit. Transcript
// entries retain the names and appearance recorded with the conversation.
export function applyPersonaUpdate(state: AppState, persona: PersonaDetail): AppState {
  if (!state.bootstrap) return state;
  const bootstrap = {
    ...state.bootstrap,
    personas: state.bootstrap.personas.map((current) => (
      current.id === persona.id
        ? rosterSummary(persona)
        : current
    )),
    forums: state.bootstrap.forums.map((forum) => (
      forum.default_persona_id === persona.id
        ? { ...forum, default_persona_display_name: persona.display_name }
        : forum
    )),
  };
  const sessionSnapshot = state.sessionSnapshot
    && state.sessionSnapshot.forum.default_persona_id === persona.id
    ? {
      ...state.sessionSnapshot,
      forum: {
        ...state.sessionSnapshot.forum,
        default_persona_display_name: persona.display_name,
      },
    }
    : state.sessionSnapshot;
  return { ...state, bootstrap, sessionSnapshot };
}

export function applyCharacterUpdate(state: AppState, character: CharacterDetail): AppState {
  if (!state.bootstrap) return state;
  const summary = rosterSummary(character);
  const updateMembers = <T extends { id: string }>(members: T[]) => (
    members.map((member) => member.id === character.id
      ? { ...member, ...summary }
      : member)
  );
  const bootstrap = {
    ...state.bootstrap,
    characters: updateMembers(state.bootstrap.characters),
    forums: state.bootstrap.forums.map((forum) => ({
      ...forum,
      members: updateMembers(forum.members),
    })),
  };
  const sessionSnapshot = state.sessionSnapshot ? {
    ...state.sessionSnapshot,
    characters: updateMembers(state.sessionSnapshot.characters),
    forum: {
      ...state.sessionSnapshot.forum,
      members: updateMembers(state.sessionSnapshot.forum.members),
    },
    generation: state.sessionSnapshot.generation.character_id === character.id
      ? {
        ...state.sessionSnapshot.generation,
        character_display_name: character.display_name,
      }
      : state.sessionSnapshot.generation,
  } : null;
  return { ...state, bootstrap, sessionSnapshot };
}

export function applyForumUpdate(state: AppState, forum: ForumDetail): AppState {
  if (!state.bootstrap) return state;
  const summary = forumSummary(forum);
  const bootstrap = {
    ...state.bootstrap,
    forums: state.bootstrap.forums.map((forum) => (
      forum.id === summary.id ? summary : forum
    )),
  };
  const sessionSnapshot = state.sessionSnapshot?.forum.id === summary.id
    ? {
      ...state.sessionSnapshot,
      forum: { ...state.sessionSnapshot.forum, ...summary },
    }
    : state.sessionSnapshot;
  return {
    ...state,
    bootstrap,
    sessionSnapshot,
    inspectedForum: {
      ...state.inspectedForum,
      writable: forum.writable,
    },
  };
}
