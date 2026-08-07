import { fireEvent, render, screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import type { Bootstrap } from '../api/client';
import { bootstrapFixture, fixtureClient } from '../test/fixtures';
import { App } from './App';

function renderAt(width: number) {
  Object.defineProperty(window, 'innerWidth', { configurable: true, value: width });
  return render(<App client={fixtureClient()} />);
}

describe.each([
  ['desktop', 1280],
  ['iPhone', 390],
])('App shell at %s width', (_name, width) => {
  it('lets only the two-line control change sidebar visibility', async () => {
    const { container } = renderAt(width);
    const app = container.querySelector('.cha-app');
    expect(app).toHaveAttribute('data-sidebar', 'open');

    fireEvent.click(await screen.findByRole('button', { name: 'Personas' }));
    expect(screen.getByRole('heading', { name: 'Personas' })).toBeInTheDocument();
    expect(app).toHaveAttribute('data-sidebar', 'open');

    fireEvent.click(screen.getByRole('button', { name: 'Hide sidebar' }));
    expect(app).toHaveAttribute('data-sidebar', 'closed');
    expect(screen.getByRole('heading', { name: 'Personas' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Characters' }));
    expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
    expect(app).toHaveAttribute('data-sidebar', 'closed');
  });
});

it('renders bootstrap discovery data and preserves conversation context while navigating', async () => {
  render(<App client={fixtureClient()} />);

  expect(await screen.findByLabelText('Current chat context')).toHaveTextContent(
    'EntranceFrom: GuestTo: Assistant',
  );
  const recents = screen.getByLabelText('Recent sessions');
  expect(within(recents).getByText('Welcome')).toBeInTheDocument();
  expect(within(recents).getByText('Planning')).toBeInTheDocument();
  expect(within(recents).getByText('The Lobby')).toBeInTheDocument();

  fireEvent.click(screen.getByRole('button', { name: 'Personas' }));
  expect(screen.getByText('Thoughtful, curious, and concise')).toBeInTheDocument();
  fireEvent.click(screen.getByRole('radio', { name: /Reader/ }));
  fireEvent.click(screen.getByRole('button', { name: 'WelcomeEntrance' }));
  expect(screen.getByLabelText('Current chat context')).toHaveTextContent('From: Reader');
});

it('loads character detail and renders the restricted Markdown presentation', async () => {
  render(<App client={fixtureClient()} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Characters' }));
  expect(screen.getByText('A deterministic test character')).toBeInTheDocument();

  fireEvent.click(screen.getByRole('button', { name: /Guide/ }));
  expect(await screen.findByRole('heading', { name: 'Guide dossier' })).toBeInTheDocument();
  expect(screen.getByText('careful').tagName).toBe('STRONG');
  expect(screen.getByRole('heading', { name: 'Guide' })).toBeInTheDocument();

  fireEvent.click(within(screen.getByLabelText('Character detail navigation'))
    .getByRole('button', { name: 'Characters' }));
  expect(screen.getByRole('heading', { name: 'Characters' })).toBeInTheDocument();
});

it('shows real forums and their plain-text character membership', async () => {
  render(<App client={fixtureClient()} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));

  expect(screen.getByRole('button', { name: 'EntranceAssistant' })).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
  expect(screen.getByRole('heading', { name: 'Sessions' })).toBeInTheDocument();
  expect(screen.getByText('The Lobby sessions are not loaded yet.')).toBeInTheDocument();
});

it('shows a clear incompatible-response state instead of a blank screen', async () => {
  const client = fixtureClient({
    getBootstrap: async () => ({ personas: [] } as unknown as Bootstrap),
  });
  render(<App client={client} />);

  expect(await screen.findByRole('heading', { name: 'Incompatible application response' }))
    .toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('initial_persona_id');
});

it('contains the amended chat controls and no Settings entry point', async () => {
  renderAt(1280);
  expect(await screen.findByRole('button', { name: 'Choose target character' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
  expect(screen.queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
  expect(bootstrapFixture.personas).toHaveLength(2);
});
