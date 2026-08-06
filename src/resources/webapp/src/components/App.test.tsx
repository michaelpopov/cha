import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from './App';

function renderAt(width: number) {
  Object.defineProperty(window, 'innerWidth', { configurable: true, value: width });
  return render(<App />);
}

describe.each([
  ['desktop', 1280],
  ['iPhone', 390],
])('App shell at %s width', (_name, width) => {
  it('lets only the two-line control change sidebar visibility', () => {
    const { container } = renderAt(width);
    const app = container.querySelector('.cha-app');
    expect(app).toHaveAttribute('data-sidebar', 'open');

    fireEvent.click(screen.getByRole('button', { name: 'Personas' }));
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

it('contains the amended chat controls and no Settings entry point', () => {
  renderAt(1280);
  expect(screen.getByRole('button', { name: 'Choose target character' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
  expect(screen.queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
});
