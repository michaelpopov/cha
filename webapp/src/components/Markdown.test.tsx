import { render, screen } from '@testing-library/react';
import { expect, it } from 'vitest';

import { Markdown, renderRestrictedMarkdown } from './Markdown';

it('renders the supported Markdown subset', () => {
  render(<Markdown source={'# Dossier\n\nA **bold** and *careful* note.\n\n- one\n- `two`\n\n```txt\nthree\n```'} />);
  expect(screen.getByRole('heading', { name: 'Dossier' })).toBeInTheDocument();
  expect(screen.getByText('bold').tagName).toBe('STRONG');
  expect(screen.getByText('careful').tagName).toBe('EM');
  expect(screen.getByText('two').tagName).toBe('CODE');
  expect(screen.getByText('three').tagName).toBe('CODE');
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
