import { OpenAiVoiceInputSession } from './openAiVoiceInput';

export const xaiVoiceInputUnavailable = 'xAI voice input transport is not implemented';

export interface VoiceInputConfiguration {
  provider: 'openai' | 'xai';
  model: string;
  delay: 'low' | 'medium' | 'high' | 'xhigh';
  prompt: string;
  languages?: string[];
}

export interface VoiceInputTransport {
  stop(): Promise<void>;
  cancel(): void;
}

export interface VoiceInputXaiStartResult {
  session_id: string;
  stop_budget_ms: number;
}

export interface VoiceInputXaiPieces {
  session_id: string;
  pieces: string[];
}

// Session 4 calls these methods. This session rejects xAI before using them.
export interface VoiceInputXaiBridge {
  start(
    sessionId: string,
    languages: string[],
    signal: AbortSignal,
  ): Promise<VoiceInputXaiStartResult>;
  audio(
    sessionId: string,
    pcmBase64: string,
    signal: AbortSignal,
  ): Promise<VoiceInputXaiPieces>;
  stop(
    sessionId: string,
    remainingMs: number,
    signal: AbortSignal,
  ): Promise<VoiceInputXaiPieces>;
  cancel(sessionId: string): Promise<void>;
}

function unavailableXaiCall(): Promise<never> {
  return Promise.reject(new Error(xaiVoiceInputUnavailable));
}

export const unimplementedXaiBridge: VoiceInputXaiBridge = {
  start: unavailableXaiCall,
  audio: unavailableXaiCall,
  stop: unavailableXaiCall,
  cancel: unavailableXaiCall,
};

export function appendTranscription(current: string, transcription: string): string {
  const addition = normalizeDictationCommands(transcription).trim();
  if (!addition) return current;
  if (!current || /\s$/.test(current) || /^[,.;:!?)}\]]/.test(addition)) {
    return current + addition;
  }
  return `${current} ${addition}`;
}

const wordEdge = String.raw`[\p{L}\p{N}_]`;
const punctuation = String.raw`[,.!?;:]`;

const dictationCommands = [
  {
    phrase: 'exclamation (?:mark|point|sign)|восклицательный знак|знак восклицания',
    replacement: '!',
  },
  {
    phrase: 'question mark|вопросительный знак|знак вопроса',
    replacement: '?',
  },
  { phrase: 'comma|запятая', replacement: ',' },
  { phrase: 'period|full stop|точка', replacement: '.' },
  { phrase: 'new line|новая строка|с новой строки', replacement: '\n' },
].map(({ phrase, replacement }) => ({
  pattern: new RegExp(
    `(?:${punctuation}[ \\t]*)?[ \\t]*(?<!${wordEdge})(?:${phrase})`
      + `(?!${wordEdge})(?:[ \\t]*${punctuation})?`,
    'giu',
  ),
  replacement,
}));

function normalizeDictationCommands(transcription: string): string {
  let result = transcription;
  for (const command of dictationCommands) {
    result = result.replace(command.pattern, (match, offset: number, source: string) => {
      if (command.replacement === '\n') return '\n';
      const next = source[offset + match.length];
      return next && !/\s/.test(next) ? `${command.replacement} ` : command.replacement;
    });
  }
  return result.replace(/[ \t]*\n[ \t]*/g, '\n');
}

export type VoiceInputConnect = (
  sdp: string,
  languages: string[],
  signal: AbortSignal,
) => Promise<string>;

function hasMediaInput(): boolean {
  return typeof navigator.mediaDevices?.getUserMedia === 'function';
}

// Session 4 replaces this stub and calls xaiBridge. Do not route xAI through OpenAI.
function startXaiVoiceInput(xaiBridge: VoiceInputXaiBridge): Promise<VoiceInputTransport> {
  void xaiBridge;
  return Promise.reject(new Error(xaiVoiceInputUnavailable));
}

export class VoiceInputSession {
  static supported(provider: VoiceInputConfiguration['provider']): boolean {
    if (!hasMediaInput()) return false;
    if (provider === 'openai') return typeof RTCPeerConnection !== 'undefined';
    return typeof AudioContext !== 'undefined'
      && typeof AudioWorkletNode !== 'undefined';
  }

  static async start(
    configuration: VoiceInputConfiguration,
    onTranscription: (text: string) => void,
    onFailure: (failure: unknown) => void,
    nativeConnect: VoiceInputConnect,
    xaiBridge: VoiceInputXaiBridge,
    signal?: AbortSignal,
  ): Promise<VoiceInputTransport> {
    if (signal?.aborted) {
      throw new DOMException('The operation was aborted.', 'AbortError');
    }
    if (configuration.provider === 'xai') return startXaiVoiceInput(xaiBridge);
    if (configuration.provider !== 'openai' || !VoiceInputSession.supported('openai')) {
      throw new Error('Voice input is unavailable.');
    }
    return OpenAiVoiceInputSession.start(
      configuration, onTranscription, onFailure, nativeConnect, signal,
    );
  }
}
