import { getSchema } from '@tiptap/core';
import { DOMParser as NoteParser } from '@tiptap/pm/model';
import DOMPurify from 'dompurify';
import { unzipSync } from 'fflate';

import { ChaError, type NoteBookImport, type NoteItem } from '../api/client';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { canonicalNoteLink, flattenClipboardTable, noteHeadings, notesExtensions } from './editor';
import { notePageName } from './pageImport';
import { availableNoteName } from './tree';

const textFileLimit = 16 * 1024 * 1024;
const requestLimit = textFileLimit - 64 * 1024;
const epubNamespace = 'http://www.idpf.org/2007/ops';
const xhtmlNamespace = 'http://www.w3.org/1999/xhtml';
const omittedTags = [
  'img', 'svg', 'script', 'style', 'iframe', 'object', 'embed', 'applet',
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
  const xml = new DOMParser().parseFromString(text, 'application/xhtml+xml');
  if (elements(xml, 'parsererror').length) invalid(`Could not parse XML in “${path}”.`);
  return xml;
}

function hasToken(value: string | null, token: string): boolean {
  return (value ?? '').split(/\s+/u).includes(token);
}

export function prepareEpubImport(
  bytes: Uint8Array,
  filename: string,
  items: readonly NoteItem[],
): NoteBookImport {
  // fflate's filter runs before decompression. Never inflate unused resources.
  function read(path: string, text = true): string {
    let found = false;
    try {
      const files = unzipSync(bytes, { filter: (file) => {
        if (file.name !== path) return false;
        found = true;
        if (!text) return false;
        if (file.originalSize > textFileLimit) {
          throw new ChaError('body_too_large', `“${path}” exceeds the 16 MiB EPUB file limit.`);
        }
        return true;
      } });
      if (!found) invalid(`The EPUB is missing “${path}”.`);
      if (!text) return '';
      const content = files[path];
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
    } catch (failure) {
      if (failure instanceof ChaError) throw failure;
      invalid(`Could not read “${path}” from the EPUB.`);
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
  const seen = new Set<string>();
  const notes: NoteBookImport['notes'] = [];
  for (const ref of elements(spine, 'itemref')) {
    const entry = entries.find((item) => item.getAttribute('id') === ref.getAttribute('idref'));
    const href = entry?.getAttribute('href');
    const path = href ? resolvePath(href, packagePath) : null;
    if (!entry || !path) invalid(`Invalid spine file in “${packagePath}”.`);
    if (seen.has(path)) continue;
    seen.add(path);
    let doc = schema.topNodeType.createAndFill()!;
    if (entry.getAttribute('media-type') === 'application/xhtml+xml') {
      const xml = parseXml(read(path), path);
      // A body outside the XHTML namespace does not become the inert document's body.
      const body = xml.getElementsByTagNameNS(xhtmlNamespace, 'body')[0];
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
      for (const table of [...inert.body.querySelectorAll('table')].reverse()) {
        flattenClipboardTable(table);
      }
      try {
        doc = parser.parse(inert.body);
      } catch {
        invalid(`Could not convert “${path}” to a note.`);
      }
    } else {
      read(path, false); // Require the spine entry, but omit its media content.
    }
    const content = doc.toJSON() as Record<string, unknown>;
    if (noteDocumentByteLength(content) > noteDocumentByteLimit) {
      throw new ChaError('body_too_large', `The note from “${path}” exceeds 12 MiB.`);
    }
    const basename = path.slice(path.lastIndexOf('/') + 1).replace(/\.[^.]*$/u, '');
    const name = labels.get(path) || notePageName(noteHeadings(doc)[0]?.text ?? '')
      || notePageName(basename) || 'File';
    notes.push({ name, document: content });
  }
  if (!notes.length) invalid('The book has no reading-order files.');
  const digits = Math.max(3, String(notes.length).length);
  notes.forEach((note, index) => { note.name = `${String(index + 1).padStart(digits, '0')}. ${note.name}`; });
  const metadata = elements(pkg, 'metadata')[0];
  const title = metadata ? elements(metadata, 'title').map((title) => notePageName(title.textContent ?? '')).find(Boolean) : '';
  const name = availableNoteName(items, null, title || notePageName(filename.replace(/\.epub$/iu, '')) || 'Book');
  const request = { name, notes };
  if (noteDocumentByteLength(request) > requestLimit) {
    throw new ChaError('body_too_large', 'The book exceeds the EPUB import request limit (16 MiB).');
  }
  return request;
}
