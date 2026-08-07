import { useEffect, useState, type Dispatch } from 'react';

import type { ChaClient, CharacterDetail } from '../api/client';
import type { AppAction, AppState } from '../state/view';
import { Markdown } from './Markdown';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  ForumsIcon,
  SendIcon,
  TargetIcon,
} from './Icons';

interface DiscoveryScreenProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

export function ChatScreen({ state }: { state: AppState }) {
  const persona = state.bootstrap?.personas.find(({ id }) => id === state.currentPersonaId);
  const forum = state.bootstrap?.forums.find(
    ({ id }) => id === state.activeConversation?.forumId,
  );
  const character = state.bootstrap?.characters.find(
    ({ id }) => id === state.currentDefaultCharacterId,
  );

  return (
    <section className="cha-screen cha-chat" aria-label="Chat area">
      <div className="cha-transcript">
        <div className="cha-chat-welcome">
          <span className="cha-chat-kicker">Welcome</span>
          <p>Your conversation will appear here.</p>
        </div>
      </div>
      <form className="cha-composer" onSubmit={(event) => event.preventDefault()}>
        <button
          aria-label="Choose target character"
          className="cha-composer-action"
          disabled
          type="button"
        >
          <TargetIcon />
        </button>
        <input
          aria-label="Message"
          disabled
          placeholder={`Message ${character?.display_name ?? 'character'}`}
        />
        <button
          aria-label="Send message"
          className="cha-composer-action cha-send"
          disabled
          type="submit"
        >
          <SendIcon />
        </button>
      </form>
      <div className="cha-chat-status" aria-label="Current chat context">
        <span>{forum?.display_name ?? 'Unknown forum'}</span>
        <span>From: {persona?.display_name ?? 'Unknown persona'}</span>
        <span>To: {character?.display_name ?? 'Unknown character'}</span>
      </div>
    </section>
  );
}

export function PersonasScreen({ state, dispatch }: DiscoveryScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Personas navigation">
      <p className="cha-screen-instruction">Select a persona</p>
      <div className="cha-persona-list" role="radiogroup" aria-label="Persona">
        {state.bootstrap?.personas.map((persona) => (
          <label className="cha-persona-row" key={persona.id}>
            <span className="cha-persona-copy">
              <span className="cha-persona-name">{persona.display_name}</span>
              {persona.description && (
                <span className="cha-persona-description">{persona.description}</span>
              )}
            </span>
            <input
              checked={state.currentPersonaId === persona.id}
              name="persona"
              onChange={() => dispatch({ type: 'select-persona', personaId: persona.id })}
              type="radio"
              value={persona.id}
            />
          </label>
        ))}
      </div>
    </section>
  );
}

export function CharactersScreen({ state, dispatch }: DiscoveryScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Characters navigation">
      <div className="cha-character-list">
        {state.bootstrap?.characters.map((character) => (
          <button
            className="cha-character-row"
            key={character.id}
            onClick={() => dispatch({ type: 'inspect-character', characterId: character.id })}
            type="button"
          >
            <span className="cha-persona-copy">
              <span className="cha-persona-name">{character.display_name}</span>
              {character.description && (
                <span className="cha-persona-description">{character.description}</span>
              )}
            </span>
            <ChevronRightIcon className="cha-chevron" />
          </button>
        ))}
      </div>
    </section>
  );
}

interface CharacterDetailScreenProps extends DiscoveryScreenProps {
  client: ChaClient;
}

export function CharacterDetailScreen({
  state,
  dispatch,
  client,
}: CharacterDetailScreenProps) {
  const [detail, setDetail] = useState<CharacterDetail | null>(null);
  const [error, setError] = useState<string | null>(null);
  const characterId = state.inspectedCharacterId;

  useEffect(() => {
    if (!characterId) return;
    let current = true;
    setDetail(null);
    setError(null);
    void client.getCharacter(characterId).then(
      (loaded) => {
        if (current) setDetail(loaded);
      },
      (failure: unknown) => {
        if (current) {
          setError(failure instanceof Error ? failure.message : 'Character detail could not be loaded.');
        }
      },
    );
    return () => {
      current = false;
    };
  }, [characterId, client]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Character detail navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-characters' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Characters</span>
      </button>
      {!characterId && <p className="cha-state-message">No character is selected.</p>}
      {characterId && !detail && !error && (
        <p className="cha-state-message" role="status">Loading character…</p>
      )}
      {error && <p className="cha-state-message cha-error-message" role="alert">{error}</p>}
      {detail && <Markdown source={detail.character_markdown} />}
    </section>
  );
}

export function ForumsScreen({ state, dispatch }: DiscoveryScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forums navigation">
      <p className="cha-screen-instruction">Choose a forum</p>
      <div className="cha-list">
        {state.bootstrap?.forums.map((forum) => (
          <button
            className="cha-list-action"
            key={forum.id}
            onClick={() => dispatch({ type: 'select-forum', forumId: forum.id })}
            type="button"
          >
            <span className="cha-list-icon"><ForumsIcon /></span>
            <span className="cha-list-copy">
              <span className="cha-primary-line">{forum.display_name}</span>
              <span className="cha-secondary-line">
                {forum.members.map(({ display_name }) => display_name).join(', ') || 'No characters'}
              </span>
            </span>
            <ChevronRightIcon className="cha-chevron" />
          </button>
        ))}
      </div>
    </section>
  );
}

export function SessionsScreen({ state, dispatch }: DiscoveryScreenProps) {
  const forum = state.bootstrap?.forums.find(({ id }) => id === state.currentForumId);
  return (
    <section className="cha-screen cha-navigation" aria-label="Forum sessions navigation">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-forums' })} type="button">
        <ChevronLeftIcon />
        <span>All forums</span>
      </button>
      <p className="cha-state-message">
        {forum ? `${forum.display_name} sessions are not loaded yet.` : 'No forum is selected.'}
      </p>
    </section>
  );
}

export function NewSessionScreen({ dispatch }: Pick<DiscoveryScreenProps, 'dispatch'>) {
  return (
    <section className="cha-screen cha-navigation" aria-label="New session navigation">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-forums' })} type="button">
        <ChevronLeftIcon />
        <span>Sessions</span>
      </button>
    </section>
  );
}
