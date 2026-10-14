import { type Dispatch, type ReactNode } from 'react';
import { type ChaClient } from '../api/client';
import type { AppState, DataAction, NavigationAction } from '../state/view';
import { ChevronLeftIcon, ChevronRightIcon } from './Icons';

export interface SettingsScreenProps {
  client: ChaClient;
  dispatch: Dispatch<DataAction>;
  navigate: Dispatch<NavigationAction>;
  state: AppState;
}

export function BackToSettings({ navigate }: { navigate: Dispatch<NavigationAction> }) {
  return (
    <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings' })} type="button">
      <ChevronLeftIcon />
      <span>Settings</span>
    </button>
  );
}

export function SettingsRow({
  description,
  disabled = false,
  icon,
  label,
  onClick,
  trailingIcon,
}: {
  description?: string;
  disabled?: boolean;
  icon: ReactNode;
  label: string;
  onClick(): void;
  trailingIcon?: ReactNode;
}) {
  return (
    <button className="cha-list-action cha-settings-row" disabled={disabled} onClick={onClick} type="button">
      <span className="cha-list-icon">{icon}</span>
      <span className="cha-list-copy">
        <span className="cha-primary-line">{label}</span>
        {description && <span className="cha-secondary-line">{description}</span>}
      </span>
      {trailingIcon === undefined
        ? <ChevronRightIcon className="cha-chevron" />
        : trailingIcon}
    </button>
  );
}

export function LoadFailure({ message, retry }: { message: string; retry(): void }) {
  return (
    <div className="cha-state-message cha-error-message" role="alert">
      <p>{message}</p>
      <button className="cha-button cha-button-ghost" onClick={retry} type="button">Try again</button>
    </div>
  );
}

export function UsedBy({ empty, items }: { empty: string; items: string[] }) {
  return (
    <section className="cha-settings-usage">
      <h2>Used by</h2>
      {items.length
        ? <ul>{items.map((item) => <li key={item}>{item}</li>)}</ul>
        : <p>{empty}</p>}
    </section>
  );
}
