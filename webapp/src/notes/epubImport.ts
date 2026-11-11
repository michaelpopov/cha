import { getSchema, isNodeEmpty } from '@tiptap/core';
import { DOMParser as NoteParser, type Node as NoteNode } from '@tiptap/pm/model';
import DOMPurify from 'dompurify';
import { unzipSync } from 'fflate';

import { ChaError, type NoteBookImport, type NoteItem } from '../api/client';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { canonicalNoteLink, flattenClipboardTable, noteHeadings, notesExtensions } from './editor';
import { notePageName } from './pageImport';
import { embeddedImageError, pictureByteLimit, pictureFromBytes } from './pictures';
import { availableNoteName } from './tree';

const textFileLimit = 16 * 1024 * 1024;
const requestLimit = textFileLimit - 64 * 1024;
const epubNamespace = 'http://www.idpf.org/2007/ops';
const xhtmlNamespace = 'http://www.w3.org/1999/xhtml';
const omittedTags = [
  'svg', 'script', 'style', 'iframe', 'object', 'embed', 'applet',
  'video', 'audio', 'canvas', 'form', 'input', 'button', 'select', 'textarea',
];
// Keep the EPUB link policy separate from the sanitizer used by page import.
const epubPurifier = DOMPurify();
// Only explicit web links remain. ALLOWED_URI_REGEXP would also remove values such as <ol start="4">.
epubPurifier.addHook('afterSanitizeAttributes', (element) => {
  const href = element.getAttribute('href');
  if (href) element.setAttribute('href', /^https?:/iu.test(href) ? canonicalNoteLink(href) ?? '' : '');
});

function invalid(message: string): never {
  throw new ChaError('invalid_argument', message);
}

// containingPath is a literal archive path; href is already a URL reference.
function resolvePath(href: string, containingPath = ''): string | null {
  try {
    const base = 'epub:/' + containingPath.split('/').map(encodeURIComponent).join('/');
    const url = new URL(href, base);
    if (url.protocol !== 'epub:' || url.host) return null;
    return decodeURIComponent(url.pathname.slice(1));
  } catch {
    return null;
  }
}

function elements(node: Document | Element, name: string): Element[] {
  return [...node.getElementsByTagNameNS('*', name)];
}

function parseXml(text: string, path: string): Document {
  const parser = new DOMParser();
  const xml = parser.parseFromString(text, 'application/xhtml+xml');
  if (!elements(xml, 'parsererror').length) return xml;

  // Keep XML structure and literal text intact while recovering named HTML entities.
  const template = document.createElement('template');
  const replacements = new Map<string, string>();
  const tokens = /<!--[\s\S]*?-->|<!\[CDATA\[[\s\S]*?\]\]>|<\?[\s\S]*?\?>|<!DOCTYPE(?:[^"'[>]|"[^"]*"|'[^']*')*(\[|>)|(&[A-Za-z][A-Za-z0-9]*;)/gu;
  const repaired = text.replace(tokens, (token, doctypeEnd, entity) => {
    // A DTD with an internal subset can define its own meanings for these names.
    if (doctypeEnd === '[') invalid(`Could not parse XML in “${path}”.`);
    if (!entity) return token;
    const replacement = replacements.get(entity);
    if (replacement !== undefined) return replacement;
    // Attribute parsing leaves unknown names intact, even with a known prefix.
    template.innerHTML = `<i title="${entity}"></i>`;
    const decoded = template.content.firstElementChild!.getAttribute('title')!;
    if (decoded === entity) return token;
    const references = Array.from(decoded, (character) => `&#${character.codePointAt(0)};`).join('');
    replacements.set(entity, references);
    return references;
  });
  if (repaired !== text) {
    const recovered = parser.parseFromString(repaired, 'application/xhtml+xml');
    if (!elements(recovered, 'parsererror').length) return recovered;
  }
  invalid(`Could not parse XML in “${path}”.`);
}

function hasToken(value: string | null, token: string): boolean {
  return (value ?? '').split(/\s+/u).includes(token);
}

function openingChapterTitle(doc: NoteNode): string {
  let opening: NoteNode | undefined;
  let following = '';
  for (let index = 0; index < doc.childCount; index++) {
    const block = doc.child(index);
    if (!block.textContent.trim()) continue;
    if (block.type.name !== 'paragraph') break;
    if (opening) {
      following = block.textContent.trim();
      break;
    }
    opening = block;
  }
  if (!opening) return '';

  const [firstLine, ...otherLines] = opening.textBetween(0, opening.content.size, '', '\n').trim().split('\n');
  const title = notePageName(firstLine);
  if (title.length > 80 || !/\p{L}/u.test(title) || /[.!?,;…。！？]$/u.test(title)
    || canonicalNoteLink(title)) return '';

  // A short opening line above much longer text can be a title without formatting.
  const prose = otherLines.join(' ').trim() || following;
  if (prose.length >= Math.max(40, 2 * title.length)) return title;
  if (otherLines.length) return '';
  if (/\p{Lu}/u.test(title) && title === title.toUpperCase()) return title;

  let bold = true;
  opening.forEach((inline) => {
    if (inline.text?.trim() && !inline.marks.some((mark) => mark.type.name === 'bold')) bold = false;
  });
  return bold ? title : '';
}

export function prepareEpubImport(
  bytes: Uint8Array,
  filename: string,
  items: readonly NoteItem[],
  parentId: number | null = null,
): NoteBookImport {
  // fflate's filter runs before decompression. Never inflate unused resources.
  function readBytes(path: string, limit: number | null = textFileLimit): Uint8Array {
    let found = false;
    try {
      const files = unzipSync(bytes, { filter: (file) => {
        if (file.name !== path) return false;
        found = true;
        if (limit === null) return false;
        if (file.originalSize > limit) {
          throw new ChaError('body_too_large', `“${path}” exceeds the ${limit / (1024 * 1024)} MiB EPUB file limit.`);
        }
        return true;
      } });
      if (!found) invalid(`The EPUB is missing “${path}”.`);
      return files[path] ?? new Uint8Array();
    } catch (failure) {
      if (failure instanceof ChaError) throw failure;
      invalid(`Could not read “${path}” from the EPUB.`);
    }
  }

  function read(path: string): string {
    const content = readBytes(path);
    try {
      // UTF-16 XML identifies its byte order with a BOM or the opening "<?".
      let encoding = 'utf-8';
      if ((content[0] === 0xff && content[1] === 0xfe)
        || (content[0] === 0x3c && content[1] === 0 && content[2] === 0x3f && content[3] === 0)) {
        encoding = 'utf-16le';
      } else if ((content[0] === 0xfe && content[1] === 0xff)
        || (content[0] === 0 && content[1] === 0x3c && content[2] === 0 && content[3] === 0x3f)) {
        encoding = 'utf-16be';
      }
      return new TextDecoder(encoding, { fatal: true }).decode(content);
    } catch {
      invalid(`Could not read “${path}” from the EPUB.`);
    }
  }

  function readPicture(source: string, chapterPath: string): string | null {
    if (source.startsWith('data:')) return embeddedImageError(source) ? null : source;
    const path = source ? resolvePath(source, chapterPath) : null;
    if (!path) return null;
    try {
      const picture = pictureFromBytes(readBytes(path, pictureByteLimit));
      return 'src' in picture ? picture.src : null;
    } catch {
      // Missing, oversized or unreadable pictures must not discard the chapter.
      return null;
    }
  }

  const container = parseXml(read('META-INF/container.xml'), 'META-INF/container.xml');
  const rootfile = elements(container, 'rootfile').find((element) => (
    element.getAttribute('media-type') === 'application/oebps-package+xml'
  ));
  const packagePath = resolvePath(rootfile?.getAttribute('full-path') ?? '');
  if (!packagePath) invalid('The EPUB container has no package path.');
  const pkg = parseXml(read(packagePath), packagePath);
  const manifest = elements(pkg, 'manifest')[0];
  const spine = elements(pkg, 'spine')[0];
  if (pkg.documentElement.localName !== 'package' || !manifest || !spine) {
    invalid(`Invalid EPUB package in “${packagePath}”.`);
  }
  const entries = elements(manifest, 'item');
  const labels = new Map<string, string>();
  const nav = entries.find((entry) => hasToken(entry.getAttribute('properties'), 'nav'));
  const ncx = nav ? undefined : entries.find((entry) => entry.getAttribute('id') === spine.getAttribute('toc'));
  const navigation = nav ?? ncx;
  const navigationPath = navigation ? resolvePath(navigation.getAttribute('href') ?? '', packagePath) : null;
  if (navigationPath) {
    try {
      const xml = parseXml(read(navigationPath), navigationPath);
      const toc = nav
        ? elements(xml, 'nav').find((element) => hasToken(element.getAttributeNS(epubNamespace, 'type'), 'toc'))
        : elements(xml, 'navMap')[0];
      if (toc) {
        const links = elements(toc, nav ? 'a' : 'navPoint');
        for (const link of links) {
          const children = [...link.children];
          const href = nav ? link.getAttribute('href') : children.find((child) => child.localName === 'content')?.getAttribute('src');
          const label = notePageName((nav ? link : children.find((child) => child.localName === 'navLabel'))?.textContent ?? '');
          const target = href ? resolvePath(href, navigationPath) : null;
          if (target && label && !labels.has(target)) labels.set(target, label);
        }
      }
    } catch (failure) {
      // Labels are optional. A missing or unusable navigation file leaves the title fallbacks.
      if (!(failure instanceof ChaError)) throw failure;
    }
  }

  const schema = getSchema(notesExtensions());
  const parser = NoteParser.fromSchema(schema);
  function parseChapter(body: HTMLElement, path: string): NoteNode {
    try {
      return parser.parse(body);
    } catch {
      invalid(`Could not convert “${path}” to a note.`);
    }
  }
  const seen = new Set<string>();
  const notes: NoteBookImport['notes'] = [];
  const emptyNotes = new Set<NoteBookImport['notes'][number]>();
  const illustrated: Array<{
    path: string;
    body: HTMLElement;
    pictures: Array<{ image: HTMLImageElement; source: string }>;
    note: NoteBookImport['notes'][number];
    size: number;
  }> = [];
  for (const ref of elements(spine, 'itemref')) {
    const entry = entries.find((item) => item.getAttribute('id') === ref.getAttribute('idref'));
    const href = entry?.getAttribute('href');
    const path = href ? resolvePath(href, packagePath) : null;
    if (!entry || !path) invalid(`Invalid spine file in “${packagePath}”.`);
    if (seen.has(path)) continue;
    seen.add(path);
    if (entry.getAttribute('media-type') !== 'application/xhtml+xml') {
      readBytes(path, null); // Require the spine entry, but omit its media content.
      continue;
    }
    const pictures: Array<{ image: HTMLImageElement; source: string }> = [];
    const xml = parseXml(read(path), path);
    let body = xml.getElementsByTagNameNS(xhtmlNamespace, 'body')[0];
    const root = xml.documentElement;
    if (!body && root.namespaceURI === null && root.localName === 'html') {
      const unqualifiedBody = [...root.children].find((child) => (
        child.namespaceURI === null && child.localName === 'body'
      ));
      if (unqualifiedBody) {
        // Some XHTML spine files omit the namespace. Supply it before importing into HTML.
        const markup = new XMLSerializer().serializeToString(unqualifiedBody);
        const qualified = parseXml(`<html xmlns="${xhtmlNamespace}">${markup}</html>`, path);
        body = qualified.getElementsByTagNameNS(xhtmlNamespace, 'body')[0];
      }
    }
    if (!body) invalid(`No XHTML body in “${path}”.`);
    const inert = document.implementation.createHTMLDocument();
    inert.body.replaceWith(inert.importNode(body, true));
    // DOMPurify and ProseMirror recognize tag names without namespace prefixes.
    for (const element of elements(inert, '*')) {
      if (!element.prefix) continue;
      const unprefixed = inert.createElementNS(element.namespaceURI, element.localName);
      for (const attribute of element.attributes) {
        unprefixed.setAttributeNS(attribute.namespaceURI, attribute.name, attribute.value);
      }
      unprefixed.append(...element.childNodes);
      element.replaceWith(unprefixed);
    }
    epubPurifier.sanitize(inert.body, {
      IN_PLACE: true,
      // Without the MathML profile, DOMPurify also removes the text of equations.
      USE_PROFILES: { html: true, mathMl: true },
      FORBID_TAGS: omittedTags,
      ADD_FORBID_CONTENTS: omittedTags,
      FORBID_ATTR: ['target', 'rel'],
    });
    // Pictures are read after all text is known. The editor skips images without a source.
    for (const image of inert.body.querySelectorAll('img')) {
      const placeholder = inert.createElement('img');
      for (const attribute of ['alt', 'title']) {
        if (image.hasAttribute(attribute)) placeholder.setAttribute(attribute, image.getAttribute(attribute)!);
      }
      image.replaceWith(placeholder);
      pictures.push({ image: placeholder, source: image.getAttribute('src') ?? '' });
    }
    // Pictures use the editor's existing block nodes, without empty wrapper paragraphs.
    for (const paragraph of inert.body.querySelectorAll('p')) {
      if (paragraph.querySelector('img') && !paragraph.textContent?.trim()) {
        paragraph.replaceWith(...paragraph.childNodes);
      }
    }
    for (const table of [...inert.body.querySelectorAll('table')].reverse()) {
      flattenClipboardTable(table);
    }
    const doc = parseChapter(inert.body, path);
    const empty = isNodeEmpty(doc, { ignoreWhitespace: true });
    if (empty && !pictures.length) continue;
    const content = doc.toJSON() as Record<string, unknown>;
    const size = empty ? 0 : noteDocumentByteLength(content);
    if (size > noteDocumentByteLimit) {
      throw new ChaError('body_too_large', `The note from “${path}” exceeds 12 MiB.`);
    }
    const basename = path.slice(path.lastIndexOf('/') + 1).replace(/\.[^.]*$/u, '');
    const name = labels.get(path) || notePageName(noteHeadings(doc)[0]?.text ?? '')
      || openingChapterTitle(doc) || notePageName(basename) || 'File';
    const note = { name, document: content };
    notes.push(note);
    if (empty) emptyNotes.add(note);
    if (pictures.length) illustrated.push({ path, body: inert.body, pictures, note, size });
  }
  if (!notes.length) invalid('The book has no reading-order files.');
  const digits = Math.max(3, String(notes.length).length);
  const metadata = elements(pkg, 'metadata')[0];
  const title = metadata ? elements(metadata, 'title').map((title) => notePageName(title.textContent ?? '')).find(Boolean) : '';
  const name = availableNoteName(items, parentId, title || notePageName(filename.replace(/\.epub$/iu, '')) || 'Book');
  const request = { parent_id: parentId, name, notes: notes.filter((note) => !emptyNotes.has(note)) };
  // Reserve the text and its numbering before spending any space on pictures.
  let free = requestLimit - noteDocumentByteLength(request) - request.notes.length * (digits + 2);
  if (free < 0) {
    throw new ChaError('body_too_large', 'The book exceeds the EPUB import request limit (16 MiB).');
  }
  // Pictures use the space that the text leaves, in reading order. A picture that does not fit is skipped.
  for (const { path, body, pictures, note, size } of illustrated) {
    // An image-only chapter adds a note object as well as a document.
    const entryOverhead = emptyNotes.has(note)
      ? noteDocumentByteLength({ name: note.name, document: null }) + digits + 3
      : 0;
    const limit = Math.min(noteDocumentByteLimit - size, free - entryOverhead);
    let pictureSpace = limit;
    if (pictureSpace <= 0) continue;
    const selected: Array<{ image: HTMLImageElement; bytes: number }> = [];
    for (const { image, source } of pictures) {
      const src = readPicture(source, path);
      if (src === null) continue;
      const picture = schema.nodes.image.create({
        src, alt: image.getAttribute('alt'), title: image.getAttribute('title'),
      });
      const added = noteDocumentByteLength(picture) + 1;
      if (added > pictureSpace) continue;
      image.setAttribute('src', src);
      pictureSpace -= added;
      selected.push({ image, bytes: added });
    }
    if (!selected.length) continue;
    let doc = parseChapter(body, path);
    let added = noteDocumentByteLength(doc) - size;
    // An image can split a paragraph. If the extra markup exceeds the budget,
    // drop pictures from the end and measure again.
    while (added > limit && selected.length) {
      let excess = added - limit;
      while (excess > 0 && selected.length) {
        const last = selected.pop()!;
        last.image.removeAttribute('src');
        excess -= last.bytes;
      }
      doc = parseChapter(body, path);
      added = noteDocumentByteLength(doc) - size;
    }
    if (!selected.length || isNodeEmpty(doc, { ignoreWhitespace: true })) continue;
    free -= added + entryOverhead;
    note.document = doc.toJSON() as Record<string, unknown>;
    emptyNotes.delete(note);
  }
  request.notes = notes.filter((note) => !emptyNotes.has(note));
  if (!request.notes.length) invalid('The book has no reading-order files.');
  const finalDigits = Math.max(3, String(request.notes.length).length);
  request.notes.forEach((note, index) => {
    note.name = `${String(index + 1).padStart(finalDigits, '0')}. ${note.name}`;
  });
  return request;
}
