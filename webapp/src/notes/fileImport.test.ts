import { describe, expect, it } from 'vitest';

import { noteFileHtml, promptFilePath } from './fileImport';

describe('note file import', () => {
  it.each([
    ['  /tmp/plan with spaces.md\n', '/tmp/plan with spaces.md'],
    ['"/tmp/plan with spaces.md"', '/tmp/plan with spaces.md'],
    ['"C:\\Users\\Me\\plan with spaces.md"', 'C:\\Users\\Me\\plan with spaces.md'],
    ['C:/Users/Me/plan.md', 'C:/Users/Me/plan.md'],
    ['\\\\server\\share\\plan.md', '\\\\server\\share\\plan.md'],
    ['Read /tmp/plan.md', null],
    ['plan.md', null],
    ['/tmp/plan.md\nExplain this', null],
    ['/tmp/plan.md\0', null],
    ['https://example.org/plan.md', null],
    ['/plan.md', null],
    ['/todo', null],
    ['/', null],
  ])('recognizes a standalone full path in %s', (input, expected) => {
    expect(promptFilePath(input)).toBe(expected);
  });

  it('gets the title from a formatted Markdown heading, including Setext headings', () => {
    expect(noteFileHtml({ name: 'plan.MD', text: 'Introduction\n\n# **A plan**\n\nBody' }).title).toBe('A plan');
    expect(noteFileHtml({ name: 'plan.markdown', text: 'A plan\n======\n\nBody' }).title).toBe('A plan');
    expect(noteFileHtml({ name: 'plan.md', text: 'No heading' }).title).toBe('');
  });

  it('sanitizes Markdown and keeps source code and HTML files literal', () => {
    const markdown = noteFileHtml({ name: 'plan.md', text: '# Title\n\n<script>bad()</script>\n\n[bad](javascript:bad())' });
    expect(markdown.html).not.toContain('<script');
    expect(markdown.html).not.toContain('href="javascript:');
    const html = noteFileHtml({ name: 'plan.html', text: '<h1>Title</h1>\n**Text**' });
    expect(html.title).toBe('');
    expect(html.html).toBe('<p>&lt;h1&gt;Title&lt;/h1&gt;<br>**Text**</p>');
  });
});
