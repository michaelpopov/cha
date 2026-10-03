declare module 'codec-parser' {
  export interface CodecFrame {
    data: Uint8Array<ArrayBuffer>;
    duration: number;
    header: { layer: string };
  }
  export default class CodecParser {
    constructor(mimeType: string, options?: { enableFrameCRC32?: boolean });
    parseChunk(bytes: Uint8Array): IterableIterator<CodecFrame>;
    flush(): IterableIterator<CodecFrame>;
  }
}

declare module 'mse-audio-wrapper' {
  export default class MSEAudioWrapper {
    constructor(mimeType: string, options: {
      codec: 'mpeg'; minFramesPerSegment: number; minBytesPerSegment: number;
    });
    iterator(frames: unknown[]): IterableIterator<Uint8Array<ArrayBuffer>>;
  }
}
