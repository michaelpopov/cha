import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { snapshotFixture } from '../test/fixtures';
import { GenerationProgress } from './GenerationProgress';

describe('GenerationProgress', () => {
  it('shows parallel work and fallback, then removes all progress when the response completes', () => {
    const generation = {
      ...snapshotFixture.generation, active: true, request_id: 7, character_display_name: 'Dijkstra',
      progress: { searches_completed: 1, pages_read: 3, activities: [
        { id: 2, tool: 'web_read' as const, argument: 'https://example.org/article', retrying: false },
        { id: 3, tool: 'web_read' as const, argument: 'https://postgresql.us/slides/204/', retrying: true },
      ] },
    };
    const { rerender, container } = render(<GenerationProgress generation={generation} />);
    expect(screen.getByText('Dijkstra is reading 2 pages…')).toBeInTheDocument();
    fireEvent.click(screen.getByText('1 search completed · 3 pages read'));
    expect(screen.getByText('Reading example.org')).toBeVisible();
    expect(screen.getByText('/article')).toBeVisible();
    expect(screen.getByText('Trying another reader…')).toBeVisible();

    const writing = { ...generation, phase: 'answering' as const,
      progress: { searches_completed: 1, pages_read: 5, activities: [] } };
    rerender(<GenerationProgress generation={writing} />);
    expect(screen.getByText('Dijkstra is writing the response…')).toBeInTheDocument();
    expect(screen.getByText('1 search completed · 5 pages read')).toBeInTheDocument();
    expect(screen.queryByText('Reading example.org')).not.toBeInTheDocument();
    rerender(<GenerationProgress generation={{ ...writing, active: false }} />);
    expect(container).toBeEmptyDOMElement();
  });

  it('shows the actual search query and hides tool details while stopping', () => {
    const generation = { ...snapshotFixture.generation, active: true, character_display_name: 'Dijkstra',
      progress: { searches_completed: 0, pages_read: 0, activities: [
        { id: 1, tool: 'web_search' as const, argument: 'Postgres storage latency', retrying: false },
      ] } };
    const { rerender } = render(<GenerationProgress generation={generation} />);
    expect(screen.getByText('Dijkstra is searching the web…')).toBeInTheDocument();
    fireEvent.click(screen.getByText('Searching'));
    expect(screen.getByText('Postgres storage latency')).toBeVisible();
    rerender(<GenerationProgress generation={{ ...generation, phase: 'stopping' }} />);
    expect(screen.getByText('Stopping Dijkstra…')).toBeInTheDocument();
    expect(screen.queryByText('Postgres storage latency')).not.toBeInTheDocument();
  });
});
