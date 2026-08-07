import DOMPurify from 'dompurify';
import { Marked } from 'marked';
import { useMemo } from 'react';

function escapeHtml(value: string): string {
  return value
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');
}

const markdown = new Marked({
  async: false,
  gfm: false,
  renderer: {
    html({ text }) {
      return escapeHtml(text);
    },
    link({ tokens }) {
      return this.parser.parseInline(tokens);
    },
    image({ text }) {
      return escapeHtml(text);
    },
  },
});

const allowedTags = [
  'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
  'p', 'strong', 'em', 'ul', 'ol', 'li', 'code', 'pre', 'br',
];

export function renderRestrictedMarkdown(source: string): string {
  const rendered = markdown.parse(source);
  if (typeof rendered !== 'string') throw new TypeError('Markdown rendering became asynchronous.');
  return DOMPurify.sanitize(rendered, {
    ALLOWED_TAGS: allowedTags,
    ALLOWED_ATTR: [],
    KEEP_CONTENT: true,
  });
}

export function Markdown({ source }: { source: string }) {
  const html = useMemo(() => renderRestrictedMarkdown(source), [source]);
  return <article className="cha-markdown" dangerouslySetInnerHTML={{ __html: html }} />;
}
