import { type Dispatch } from 'react';
import type { NavigationAction } from '../state/view';
import {
  CharacterIcon,
  DatabaseIcon,
  ForumsIcon,
  KeyIcon,
  PersonasIcon,
  SettingsIcon,
  SpeakerIcon,
  TextLinesIcon,
} from './Icons';
import { SettingsRow } from './SettingsShared';

export function SettingsNavigation({ navigate }: { navigate: Dispatch<NavigationAction> }) {
  return (
    <section className="cha-settings-card" aria-label="Configuration">
      <div className="cha-settings-links">
        <SettingsRow
          icon={<PersonasIcon />}
          label="Personas"
          onClick={() => navigate({ type: 'show-personas' })}
        />
        <SettingsRow
          icon={<CharacterIcon />}
          label="Characters"
          onClick={() => navigate({ type: 'show-characters' })}
        />
        <SettingsRow
          icon={<ForumsIcon />}
          label="Forums"
          onClick={() => navigate({ type: 'show-forums' })}
        />
        <SettingsRow
          icon={<DatabaseIcon />}
          label="Vaults"
          onClick={() => navigate({ type: 'show-settings-vaults' })}
        />
        <SettingsRow
          icon={<SettingsIcon />}
          label="Providers"
          onClick={() => navigate({ type: 'show-settings-providers' })}
        />
        <SettingsRow
          icon={<TextLinesIcon />}
          label="Styles"
          onClick={() => navigate({ type: 'show-settings-styles' })}
        />
        <SettingsRow icon={<SettingsIcon />} label="Session settings"
          onClick={() => navigate({ type: 'show-settings-jev' })} />
        <SettingsRow icon={<SettingsIcon />} label="Search API"
          onClick={() => navigate({ type: 'show-settings-web-search' })} />
        <SettingsRow
          icon={<SpeakerIcon />}
          label="Voices"
          onClick={() => navigate({ type: 'show-settings-voices' })}
        />
        <SettingsRow
          icon={<KeyIcon />}
          label="API Keys"
          onClick={() => navigate({ type: 'show-settings-api-keys' })}
        />
      </div>
    </section>
  );
}
