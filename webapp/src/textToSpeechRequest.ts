import type { TextToSpeechConfiguration, TextToSpeechVoice } from './textToSpeech';

export interface AudioEntry {
  forum_id: string;
  session_id: string;
  entry_id: number;
}

export interface AudioRequest {
  url: string;
  body: string;
  headers: Record<string, string>;
}

export function audioRequest(
  configuration: TextToSpeechConfiguration,
  voice: TextToSpeechVoice | undefined,
  text: string,
  entry?: AudioEntry,
): AudioRequest {
  const voiceId = voice?.elevenlabs_voice_id ?? configuration.voiceId;
  // CHA forwards FishAudio requests to avoid browser CORS restrictions, using
  // the configured endpoint and key; neither is supplied by the caller.
  const speed = voice?.settings.speed;
  const body = JSON.stringify({
    text, reference_id: voiceId,
    settings: speed === undefined ? {} : { speed },
    ...(entry ? { entry } : {}),
  });
  return {
    url: '/api/v1/voice-output/audio',
    body,
    headers: { 'Content-Type': 'application/json' },
  };
}
