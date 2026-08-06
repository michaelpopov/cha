import type { MainView } from '../state/view';
import {
  CharacterIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  ForumsIcon,
  MessageIcon,
  PlusIcon,
  SendIcon,
  TargetIcon,
} from './Icons';

interface ScreenProps {
  onNavigate: (view: MainView) => void;
}

export function ChatScreen() {
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
        <input aria-label="Message" disabled placeholder="Message Assistant" />
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
        <span>Entrance</span>
        <span>From: Guest</span>
        <span>To: Assistant</span>
      </div>
    </section>
  );
}

export function PersonasScreen() {
  return (
    <section className="cha-screen cha-navigation" aria-label="Personas navigation">
      <div className="cha-persona-list" role="radiogroup" aria-label="Persona">
        <label className="cha-persona-row">
          <span className="cha-persona-copy">
            <span className="cha-persona-name">Guest</span>
            <span className="cha-persona-description">The built-in visitor persona</span>
          </span>
          <input defaultChecked name="persona" type="radio" value="guest" />
        </label>
      </div>
    </section>
  );
}

export function CharactersScreen({ onNavigate }: ScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Characters navigation">
      <div className="cha-character-list">
        <button
          className="cha-character-row"
          onClick={() => onNavigate('character-detail')}
          type="button"
        >
          <span className="cha-persona-copy">
            <span className="cha-persona-name">Assistant</span>
            <span className="cha-persona-description">CHA application guide</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
      </div>
    </section>
  );
}

export function CharacterDetailScreen({ onNavigate }: ScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Character detail navigation">
      <button className="cha-back-row" onClick={() => onNavigate('characters')} type="button">
        <ChevronLeftIcon />
        <span>Characters</span>
      </button>
      <article className="cha-markdown">
        <h2>About Assistant</h2>
        <p>The complete workspace character definition will be shown here.</p>
      </article>
    </section>
  );
}

export function ForumsScreen({ onNavigate }: ScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forums navigation">
      <div className="cha-list">
        <button className="cha-list-action" onClick={() => onNavigate('sessions')} type="button">
          <span className="cha-list-icon"><ForumsIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">Entrance</span>
            <span className="cha-secondary-line">Assistant</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
      </div>
    </section>
  );
}

export function SessionsScreen({ onNavigate }: ScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forum sessions navigation">
      <button className="cha-back-row" onClick={() => onNavigate('forums')} type="button">
        <ChevronLeftIcon />
        <span>All forums</span>
      </button>
      <div className="cha-list">
        <button className="cha-list-action" onClick={() => onNavigate('new-session')} type="button">
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New session</span>
            <span className="cha-secondary-line">Enter a name to begin</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
        <button className="cha-list-action" onClick={() => onNavigate('chat')} type="button">
          <span className="cha-list-icon"><MessageIcon /></span>
          <span className="cha-list-copy"><span className="cha-primary-line">Welcome</span></span>
          <span className="cha-secondary-line">Now</span>
        </button>
      </div>
    </section>
  );
}

export function NewSessionScreen({ onNavigate }: ScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="New session navigation">
      <button className="cha-back-row" onClick={() => onNavigate('sessions')} type="button">
        <ChevronLeftIcon />
        <span>Sessions</span>
      </button>
      <form className="cha-new-session">
        <label htmlFor="session-name">Session name</label>
        <input className="cha-form-control" disabled id="session-name" placeholder="e.g. Architecture review" />
        <div className="cha-new-session-actions">
          <button className="cha-button cha-button-ghost" onClick={() => onNavigate('sessions')} type="button">
            Cancel
          </button>
          <button className="cha-button cha-button-primary" disabled type="submit">Start session</button>
        </div>
      </form>
    </section>
  );
}
