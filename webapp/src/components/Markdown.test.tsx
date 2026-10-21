import { render, screen } from '@testing-library/react';
import { expect, it } from 'vitest';

import { Markdown, renderRestrictedMarkdown } from './Markdown';

it('preserves rendered nodes when the source has not changed', () => {
  const { rerender } = render(<Markdown source="A **stable** response" />);
  const emphasis = screen.getByText('stable');
  rerender(<Markdown source="A **stable** response" />);
  expect(screen.getByText('stable')).toBe(emphasis);
});

it('renders the supported Markdown subset', () => {
  const { container } = render(<Markdown source={'# Dossier\n\nA **bold** and *careful* note.\n\n- one\n- `two`\n\n```txt\nthree\n```\n\n```\n<script>alert(1)</script>\n```\n\n---\n\n> Quoted line\n> Next line'} />);
  expect(screen.getByRole('heading', { name: 'Dossier' })).toBeInTheDocument();
  expect(screen.getByText('bold').tagName).toBe('STRONG');
  expect(screen.getByText('careful').tagName).toBe('EM');
  expect(screen.getByText('two').tagName).toBe('CODE');
  expect(screen.getByText('three').tagName).toBe('CODE');
  expect(screen.getByText('<script>alert(1)</script>').tagName).toBe('CODE');
  expect(container.querySelector('script')).not.toBeInTheDocument();
  expect(screen.getByRole('separator')).toBeInTheDocument();
  expect(container.querySelector('blockquote p')?.textContent).toBe('Quoted line\nNext line');
  expect(screen.queryByRole('article')).not.toBeInTheDocument();
});

it('strips scripts, image fetches, raw HTML, and link interactivity', () => {
  const html = renderRestrictedMarkdown(
    '<script>window.bad = true</script>\n\n<img src="https://example.test/tracker.png">\n\n'
      + '[Read me](https://example.test) ![portrait](https://example.test/image.png)\n\n'
      + '<button onclick="window.bad = true">raw control</button>',
  );
  const { container } = render(<div dangerouslySetInnerHTML={{ __html: html }} />);

  expect(container.querySelector('script')).not.toBeInTheDocument();
  expect(container.querySelector('img')).not.toBeInTheDocument();
  expect(container.querySelector('a')).not.toBeInTheDocument();
  expect(container.querySelector('button')).not.toBeInTheDocument();
  expect(screen.getByText(/Read me portrait/)).toBeInTheDocument();
  expect(container).toHaveTextContent('<script>window.bad = true</script>');
  expect(container).toHaveTextContent('<button onclick="window.bad = true">raw control</button>');
});

it('renders table headers, rows, alignment, and inline Markdown', () => {
  render(<Markdown source={
    '| Machine | Memory | Status |\n'
      + '| :--- | ---: | :---: |\n'
      + '| **Framework Desktop** | 64 GB | `ready` |\n'
      + '| GMKtec EVO-X2 | 128 GB | Available |'
  } />);

  expect(screen.getByRole('table')).toBeInTheDocument();
  expect(screen.getAllByRole('row')).toHaveLength(3);
  expect(screen.getAllByRole('columnheader').map((cell) => cell.textContent))
    .toEqual(['Machine', 'Memory', 'Status']);
  expect(screen.getByRole('columnheader', { name: 'Memory' })).toHaveAttribute('align', 'right');
  expect(screen.getByRole('cell', { name: '64 GB' })).toHaveAttribute('align', 'right');
  expect(screen.getByRole('cell', { name: 'ready' })).toHaveAttribute('align', 'center');
  expect(screen.getByText('Framework Desktop').tagName).toBe('STRONG');
  expect(screen.getByText('ready').tagName).toBe('CODE');
});

it('keeps table cell HTML inert and fenced tables literal', () => {
  const { container } = render(<Markdown source={
    '| Content |\n| --- |\n'
      + '| <img src="https://example.test/tracker.png" onerror="alert(1)"> |\n'
      + '| [Unsafe](javascript:alert(1)) |\n'
      + '| ![portrait](https://example.test/image.png) |\n\n'
      + '```md\n| Literal |\n| --- |\n| Cell |\n```'
  } />);

  expect(screen.getAllByRole('table')).toHaveLength(1);
  expect(screen.getByRole('cell', { name: /<img src=/ })).toBeInTheDocument();
  expect(screen.getByRole('cell', { name: 'Unsafe' })).toBeInTheDocument();
  expect(screen.getByRole('cell', { name: 'portrait' })).toBeInTheDocument();
  expect(container.querySelector('pre code')).toHaveTextContent('| Literal | | --- | | Cell |');
  expect(container.querySelector('img, a, script, [onerror]')).toBeNull();
});

it('opens only the fixed OpenAI device verification link', () => {
  const { container } = render(<Markdown source={
    '[Open ChatGPT](https://auth.openai.com/codex/device)\n\n'
      + '[Other path](https://auth.openai.com/other) '
      + '[Query](https://auth.openai.com/codex/device?next=evil) '
      + '[Other host](https://auth.openai.com.evil.test/codex/device)'
  } />);
  const link = screen.getByRole('link', { name: 'Open ChatGPT' });
  expect(link).toHaveAttribute('href', 'https://auth.openai.com/codex/device');
  expect(link).toHaveAttribute('target', '_blank');
  expect(link).toHaveAttribute('rel', 'noopener noreferrer');
  expect(container.querySelectorAll('a')).toHaveLength(1);
});
