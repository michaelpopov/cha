import { strToU8, zipSync, type ZipOptions } from 'fflate';

import { ChaError, type NoteContent, type NoteItem, type NoteTextMode } from '../api/client';
import { renderNoteDocument } from './noteDocument';
import { decodeEmbeddedImage } from './pictures';
import { buildNoteIndex } from './tree';

const xhtml = 'http://www.w3.org/1999/xhtml';
const epub = 'http://www.idpf.org/2007/ops';
const opf = 'http://www.idpf.org/2007/opf';
const dc = 'http://purl.org/dc/elements/1.1/';
const xml = 'http://www.w3.org/XML/1998/namespace';
const xmlns = 'http://www.w3.org/2000/xmlns/';

const container = `<?xml version="1.0" encoding="UTF-8"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
  <rootfiles>
    <rootfile full-path="EPUB/book.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>`;
const stylesheet = `mark[data-color] { background: #b3d9ff; }
@media (prefers-color-scheme: dark) {
  mark[data-color] { background: #24496b; }
}
img { max-width: 100%; height: auto; }
pre { white-space: pre-wrap; overflow-wrap: break-word; }
`;
const xhtmlType = 'application/xhtml+xml';
const zip32SizeLimit = 0xffffffff;
const zip32EntryLimit = 0xffff;

export class EpubExportError extends Error {}

function tooLarge(): EpubExportError {
  return new EpubExportError('This folder is too large to export as one EPUB.');
}

function noteError(note: NoteItem): EpubExportError {
  return new EpubExportError(`Could not export the note “${note.name}”.`);
}

function xmlText(text: string): string {
  return text.replace(/[^\u0009\u000A\u000D\u0020-\uD7FF\uE000-\uFFFD\u{10000}-\u{10FFFF}]/gu, '\uFFFD');
}

function append(parent: Element, name: string, text?: string, namespace = parent.namespaceURI): Element {
  const element = parent.ownerDocument.createElementNS(namespace, name);
  if (text !== undefined) element.textContent = xmlText(text);
  parent.appendChild(element);
  return element;
}

function xhtmlPage(title: string) {
  const doc = document.implementation.createDocument(xhtml, 'html');
  const root = doc.documentElement;
  root.setAttribute('lang', 'und');
  root.setAttributeNS(xml, 'xml:lang', 'und');
  const head = append(root, 'head');
  append(head, 'title', title);
  return { doc, head, body: append(root, 'body') };
}

function xmlBytes(doc: XMLDocument) {
  return strToU8('<?xml version="1.0" encoding="UTF-8"?>\n' + new XMLSerializer().serializeToString(doc));
}

function numbered(prefix: string, number: number) {
  return `${prefix}-${String(number).padStart(4, '0')}`;
}

type ReadChapter = (note: NoteItem, mode: NoteTextMode) => Promise<NoteContent['document']>;
type BookFiles = Record<string, Uint8Array | [Uint8Array, ZipOptions]>;

export async function buildFolderEpub(
  folderId: number, items: readonly NoteItem[], mode: NoteTextMode, readChapter: ReadChapter,
): Promise<{ title: string; bytes: Uint8Array<ArrayBuffer> }> {
  const folder = items.find(item => item.id === folderId && item.kind === 'folder');
  if (!folder) throw new EpubExportError('This folder is no longer available.');

  const level = mode === 'l1' || mode === 'l2' ? mode : null;
  const title = level ? `${folder.name} (${level.toUpperCase()})` : folder.name;
  const chapters: { note: NoteItem; id: string; path: string }[] = [];
  const navigation = xhtmlPage(title);
  // Without a declared prefix, XMLSerializer can write ns1:type instead of epub:type.
  navigation.doc.documentElement.setAttributeNS(xmlns, 'xmlns:epub', epub);
  const nav = append(navigation.body, 'nav');
  nav.setAttributeNS(epub, 'epub:type', 'toc');
  const { children } = buildNoteIndex(items);
  const byId = new Map(items.map(item => [String(item.id), item]));

  function appendContents(folderKey: string, list: Element) {
    const siblings = (children[folderKey] ?? []).map(key => byId.get(key)!);
    siblings.sort((a, b) => Number(!!b.folder_summary) - Number(!!a.folder_summary));
    for (const item of siblings) {
      if (item.kind === 'folder') {
        const group = append(list, 'li');
        append(group, 'span', item.name);
        const nested = append(group, 'ol');
        appendContents(String(item.id), nested);
        if (!nested.firstChild) group.remove();
      } else if (item.kind === 'note') {
        const id = numbered('chapter', chapters.length + 1);
        const path = `${id}.xhtml`;
        chapters.push({ note: item, id, path });
        append(append(list, 'li'), 'a', item.name).setAttribute('href', path);
      }
    }
  }

  appendContents(String(folderId), append(nav, 'ol'));
  if (!chapters.length) throw new EpubExportError('This folder has no notes to export.');
  if (level && !chapters.some(({ note }) => level === 'l1' ? note.has_l1 : note.has_l2)) {
    throw new EpubExportError(`This folder has no ${level.toUpperCase()} text to export.`);
  }

  const packageDoc = document.implementation.createDocument(opf, 'package');
  const root = packageDoc.documentElement;
  root.setAttribute('version', '3.0');
  root.setAttribute('unique-identifier', 'book-id');
  const metadata = append(root, 'metadata');
  append(metadata, 'dc:identifier', `urn:uuid:${crypto.randomUUID()}`, dc).setAttribute('id', 'book-id');
  append(metadata, 'dc:title', title, dc);
  append(metadata, 'dc:language', 'und', dc);
  append(metadata, 'meta', new Date().toISOString().replace(/\.\d{3}Z$/, 'Z'))
    .setAttribute('property', 'dcterms:modified');
  const manifest = append(root, 'manifest');
  const spine = append(root, 'spine');

  const files: BookFiles = {
    mimetype: [strToU8('application/epub+zip'), { level: 0 }],
    'META-INF/container.xml': strToU8(container),
    'EPUB/book.opf': new Uint8Array(), // Fill after collecting all manifest items.
  };
  function addResource(id: string, path: string, type: string, bytes: Uint8Array): Element {
    files[`EPUB/${path}`] = bytes;
    const item = append(manifest, 'item');
    item.setAttribute('id', id);
    item.setAttribute('href', path);
    item.setAttribute('media-type', type);
    return item;
  }
  addResource('nav', 'nav.xhtml', xhtmlType, xmlBytes(navigation.doc)).setAttribute('properties', 'nav');
  addResource('css', 'book.css', 'text/css', strToU8(stylesheet));

  const fixedEntries = Object.keys(files).length;
  let imageCount = 0;
  if (fixedEntries + chapters.length >= zip32EntryLimit) throw tooLarge();
  for (const { note, id, path } of chapters) {
    const chapter = await renderChapter(note, level ?? 'full_text', readChapter);
    const page = xhtmlPage(note.name);
    const link = append(page.head, 'link');
    link.setAttribute('rel', 'stylesheet');
    link.setAttribute('href', 'book.css');
    append(page.body, 'h1', note.name);

    for (const image of chapter.querySelectorAll('img')) {
      const picture = decodeEmbeddedImage(image.getAttribute('src'));
      if ('error' in picture) {
        const alt = image.getAttribute('alt');
        image.replaceWith(alt?.trim() ? alt : 'Image unavailable');
        continue;
      }
      if (fixedEntries + chapters.length + ++imageCount >= zip32EntryLimit) throw tooLarge();
      const imageId = numbered('image', imageCount);
      const imagePath = `images/${imageId}.${picture.mime.slice('image/'.length)}`;
      addResource(imageId, imagePath, picture.mime, picture.bytes);
      image.setAttribute('src', imagePath);
      if (!image.hasAttribute('alt')) image.setAttribute('alt', '');
    }

    const nodes = document.createTreeWalker(chapter, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
    for (let node = nodes.nextNode(); node; node = nodes.nextNode()) {
      if (node instanceof Element) {
        for (const attribute of node.attributes) attribute.value = xmlText(attribute.value);
      } else if (node.nodeValue !== null) {
        node.nodeValue = xmlText(node.nodeValue);
      }
    }
    page.body.appendChild(page.doc.importNode(chapter, true));
    addResource(id, path, xhtmlType, xmlBytes(page.doc));
    append(spine, 'itemref').setAttribute('idref', id);
  }

  files['EPUB/book.opf'] = xmlBytes(packageDoc);
  const bytes = zipBook(files);
  return { title, bytes };
}

async function renderChapter(note: NoteItem, mode: NoteTextMode, readChapter: ReadChapter): Promise<DocumentFragment> {
  let doc: NoteContent['document'];
  try {
    doc = await readChapter(note, mode);
  } catch (failure) {
    if (failure instanceof ChaError && ['vault_changed', 'application_unavailable'].includes(failure.code)) throw failure;
    throw noteError(note);
  }
  const rendered = renderNoteDocument(doc, note, []);
  if (!rendered) throw noteError(note);
  const template = document.createElement('template');
  template.innerHTML = rendered.html;
  return template.content;
}

function zipBook(files: BookFiles): Uint8Array<ArrayBuffer> {
  // The all-ones size and count fields are reserved for ZIP64.
  let total = 22; // End of central directory.
  for (const [path, entry] of Object.entries(files)) {
    const length = (Array.isArray(entry) ? entry[0] : entry).byteLength;
    if (length >= zip32SizeLimit) throw tooLarge();
    total += 76 + 2 * path.length + length; // Local and central headers plus uncompressed bytes.
  }
  let archive: Uint8Array<ArrayBuffer>;
  try {
    archive = zipSync(files);
  } catch (failure) {
    if (failure instanceof RangeError && total >= zip32SizeLimit) throw tooLarge();
    throw failure;
  }
  if (archive.byteLength >= zip32SizeLimit) throw tooLarge();
  return archive;
}
