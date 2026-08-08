import { expect, it } from 'vitest';

import { renderRestrictedMarkdown } from './Markdown';

const attacks: Array<[string, string]> = [
  ['script', '<script>alert(1)</script>'],
  ['img onerror', '<img src=x onerror=alert(1)>'],
  ['js link', '[click](javascript:alert(1))'],
  ['md image', '![alt](http://evil.example/x.png)'],
  ['raw anchor', '<a href="javascript:alert(1)">x</a>'],
  ['div handler', '<div onclick="alert(1)">x</div>'],
  ['autolink', '<https://evil.example/>'],
  ['style', '<style>body{background:url(http://evil.example/x)}</style>'],
  ['nested script', '<sc<script>ript>alert(1)</script>'],
  ['iframe', '<iframe src="http://evil.example/"></iframe>'],
  ['svg onload', '<svg onload=alert(1)></svg>'],
  ['object', '<object data="http://evil.example/"></object>'],
  ['mxss noscript', '<noscript><p title="</noscript><img src=x onerror=alert(1)>">'],
  ['form', '<form action="http://evil.example/"><input name="a"></form>'],
  ['base', '<base href="http://evil.example/">'],
  ['meta refresh', '<meta http-equiv="refresh" content="0;url=http://evil.example/">'],
  ['link stylesheet', '<link rel="stylesheet" href="http://evil.example/x.css">'],
  ['html comment cond', '<!--[if IE]><script>alert(1)</script><![endif]-->'],
  ['data uri image', '![x](data:text/html;base64,PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg==)'],
  ['template', '<template><img src=x onerror=alert(1)></template>'],
];

// The rendered string is inserted as HTML, so what matters is the DOM it
// produces: escaped text such as "&lt;img onerror=...&gt;" is inert.
function parse(html: string): Document {
  return new DOMParser().parseFromString(`<body>${html}</body>`, 'text/html');
}

const allowed = new Set([
  'HTML', 'HEAD', 'BODY',
  'H1', 'H2', 'H3', 'H4', 'H5', 'H6',
  'P', 'STRONG', 'EM', 'UL', 'OL', 'LI', 'CODE', 'PRE', 'BR',
]);

it.each(attacks)('neutralizes %s', (_name, source) => {
  const document_ = parse(renderRestrictedMarkdown(source));

  for (const element of document_.querySelectorAll('*')) {
    expect(allowed).toContain(element.tagName);
    expect(element.attributes.length).toBe(0);
  }
  expect(document_.querySelector('script, img, a, iframe, object, embed, svg, style, link, form'))
    .toBeNull();
});

it('keeps supported markup and escapes HTML inside code blocks', () => {
  const html = renderRestrictedMarkdown(
    '# Title\n\nSome *emphasis* and `code`.\n\n```\n<script>alert(1)</script>\n```\n',
  );
  const document_ = parse(html);
  expect(document_.querySelector('h1')?.textContent).toBe('Title');
  expect(document_.querySelector('em')?.textContent).toBe('emphasis');
  expect(document_.querySelector('pre')?.textContent).toContain('<script>alert(1)</script>');
  expect(document_.querySelector('script')).toBeNull();
});

it('renders unexpanded template directives literally', () => {
  expect(parse(renderRestrictedMarkdown('$$(EPICTETUS.md)')).body.textContent)
    .toContain('$$(EPICTETUS.md)');
});
