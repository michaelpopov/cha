import { getSchema } from '@tiptap/core';
import { DOMParser as NoteParser } from '@tiptap/pm/model';

import { flattenClipboardTable, notesExtensions } from './editor';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { notePageHtml } from './pageImport';

export const summaryConversionError = 'The generated summary could not be converted to a note. Save summary again.';

export function summaryDocument(markdown: string): Record<string, unknown> {
  if (!markdown.trim() || new TextEncoder().encode(markdown).length > noteDocumentByteLimit)
    throw new Error(summaryConversionError);
  const body = document.createElement('div');
  body.innerHTML = notePageHtml({ url: 'file:///', title: '', markdown }, { summary: true });
  for (const table of [...body.querySelectorAll('table')].reverse()) {
    for (const cell of table.querySelectorAll('th, td')) {
      for (const attribute of ['rowspan', 'colspan']) {
        const span = cell.getAttribute(attribute);
        if (span !== null && span !== '1') throw new Error(summaryConversionError);
      }
    }
    flattenClipboardTable(table);
  }
  const parsed = NoteParser.fromSchema(getSchema(notesExtensions())).parse(body);
  const result = parsed.toJSON() as Record<string, unknown>;
  if (!parsed.textContent.trim() || noteDocumentByteLength(result) > noteDocumentByteLimit)
    throw new Error(summaryConversionError);
  return result;
}
