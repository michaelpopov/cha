import type { TextToSpeechConfiguration, TextToSpeechVoice } from './textToSpeech';

export interface AudioRequest {
  url: string;
  body: string;
  headers: Record<string, string>;
  cacheKey: string;
}

export function audioRequest(
  configuration: TextToSpeechConfiguration,
  voice: TextToSpeechVoice | undefined,
  text: string,
): AudioRequest {
  const voiceId = voice?.elevenlabs_voice_id ?? configuration.voiceId;
  // CHA forwards FishAudio requests to avoid browser CORS restrictions, using
  // the configured endpoint and key; neither is supplied by the caller.
  const speed = voice?.settings.speed;
  const body = JSON.stringify({
    text, reference_id: voiceId,
    settings: speed === undefined ? {} : { speed },
  });
  return {
    url: '/api/v1/voice-output/audio',
    body,
    headers: { 'Content-Type': 'application/json' },
    cacheKey: `${configuration.baseUrl}\n${configuration.model}\n${configuration.outputFormat}\n${body}`,
  };
}
