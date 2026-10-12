export const noteDocumentByteLimit = 12_582_912;

// Compact UTF-8 JSON size. A ProseMirror node serializes through its toJSON.
export function noteDocumentByteLength(document: unknown): number {
  return new TextEncoder().encode(JSON.stringify(document)).length;
}
