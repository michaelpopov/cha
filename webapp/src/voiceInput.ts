import type { XaiVoicePieces, XaiVoiceStartResult } from './api/client';
import { OpenAiVoiceInputSession } from './openAiVoiceInput';
import { startXaiVoiceInput } from './xaiVoiceInput';

export {
  appendPreparedTranscription,
  appendTranscription,
} from './dictationText';
export { unsupportedAudioFormat } from './xaiVoiceInput';

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

export type VoiceInputXaiStartResult = XaiVoiceStartResult;
export type VoiceInputXaiPieces = XaiVoicePieces;

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

export type VoiceInputConnect = (
  sdp: string,
  languages: string[],
  signal: AbortSignal,
) => Promise<string>;

function hasMediaInput(): boolean {
  return typeof navigator.mediaDevices?.getUserMedia === 'function';
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
    onTranscription: (text: string, provisional?: boolean) => void,
    onFailure: (failure: unknown) => void,
    nativeConnect: VoiceInputConnect,
    xaiBridge: VoiceInputXaiBridge,
    signal?: AbortSignal,
  ): Promise<VoiceInputTransport> {
    if (signal?.aborted) {
      throw new DOMException('The operation was aborted.', 'AbortError');
    }
    if (configuration.provider === 'xai') {
      if (!VoiceInputSession.supported('xai')) throw new Error('Voice input is unavailable.');
      return startXaiVoiceInput(
        configuration, onTranscription, onFailure, xaiBridge, signal,
      );
    }
    if (configuration.provider !== 'openai' || !VoiceInputSession.supported('openai')) {
      throw new Error('Voice input is unavailable.');
    }
    return OpenAiVoiceInputSession.start(
      configuration, onTranscription, onFailure, nativeConnect, signal,
    );
  }
}
