import { generateHTML, getSchema, type JSONContent } from '@tiptap/core';
import { Fragment, type Node as PmNode } from '@tiptap/pm/model';
import DOMPurify from 'dompurify';

import type { NoteItem, NoteContent } from '../api/client';
import { isRecord } from '../api/guards';
import { sortAnnotationGroups } from './annotationSource';
import { noteCommentColor, notesExtensions, storedWebDestination } from './editor';
import { embeddedImageError } from './pictures';

const extensions = notesExtensions();
const schema = getSchema(extensions);
const normalized = (text: string) => text.normalize('NFC').replace(/\s+/gu, ' ').trim();

// nodeFromJSON rejects unknown types; check also verifies schema content and marks.
// Check the JSON containers first, since ProseMirror accepts absent/null content.
function validateJSON(value: unknown): asserts value is JSONContent {
  if (!isRecord(value) || typeof value.type !== 'string'
    || (value.content !== undefined && !Array.isArray(value.content))
    || (value.marks !== undefined && !Array.isArray(value.marks))) throw new Error('Invalid node');
  if (value.type === 'text' ? typeof value.text !== 'string' || value.content !== undefined
    : value.text !== undefined) throw new Error('Invalid text');
  if (Array.isArray(value.content)) value.content.forEach(validateJSON);
}

// Reject picture sources before serialization creates any image elements.
function safePictures(node: PmNode): PmNode {
  if (node.type.name === 'image' && embeddedImageError(node.attrs.src) !== null) {
    const text = typeof node.attrs.alt === 'string' && node.attrs.alt.trim()
      ? node.attrs.alt : 'Image unavailable';
    return schema.nodes.paragraph.create(null, schema.text(text));
  }
  const children: PmNode[] = [];
  node.forEach((child) => children.push(safePictures(child)));
  return node.isLeaf ? node : node.copy(Fragment.fromArray(children));
}

export function renderNoteDocument(
  document: NoteContent['document'], item: NoteItem, items: readonly NoteItem[],
): { html: string; empty: boolean } | null {
  try {
    validateJSON(document);
    if (document.type !== 'doc') return null;
    const node = schema.nodeFromJSON(document);
    node.check();
    let display = safePictures(node).toJSON() as JSONContent;
    if (item.kind === 'annotation') display = sortAnnotationGroups(display, items);
    const blocks = [...(display.content ?? [])];
    if (blocks[0]?.type === 'heading') {
      const heading = schema.nodeFromJSON(blocks[0]);
      if (normalized(heading.textContent) === normalized(item.name)) blocks.shift();
    }
    const html = generateHTML({ ...display, content: blocks }, extensions);
    const template = globalThis.document.createElement('template');
    template.innerHTML = html;
    const body = template.content;
    for (const heading of body.querySelectorAll('h1,h2,h3,h4,h5,h6')) {
      const replacement = globalThis.document.createElement(`h${Math.min(6, Number(heading.tagName[1]) + 1)}`);
      replacement.append(...heading.childNodes);
      heading.replaceWith(replacement);
    }
    for (const link of body.querySelectorAll('a')) {
      const destination = storedWebDestination(link.getAttribute('href') ?? '');
      if (destination) link.setAttribute('href', destination);
      else link.replaceWith(...link.childNodes);
    }
    for (const mark of body.querySelectorAll('mark')) {
      if (mark.getAttribute('data-color') !== noteCommentColor) mark.removeAttribute('data-color');
    }
    for (const span of body.querySelectorAll('span:empty')) span.remove();
    const sanitized = DOMPurify.sanitize(template.innerHTML, {
      ALLOWED_TAGS: ['p', 'br', 'h2', 'h3', 'h4', 'h5', 'h6', 'ul', 'ol', 'li',
        'strong', 'em', 'u', 'code', 'pre', 'mark', 'a', 'img', 'span'],
      ALLOWED_ATTR: ['href', 'src', 'alt', 'start', 'data-color'],
      ALLOW_DATA_ATTR: false,
      ALLOW_ARIA_ATTR: false,
    });
    return {
      html: sanitized,
      empty: !body.textContent?.trim() && !body.querySelector('img'),
    };
  } catch {
    return null;
  }
}
