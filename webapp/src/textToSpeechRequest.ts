import type { TextToSpeechConfiguration, TextToSpeechVoice } from './textToSpeech';

// Choose UI defaults and route already normalized runtime endpoints.
// URL validation, normalization and errors belong to the save endpoint.
function speechProvider(url: string): 'fish-audio' | 'elevenlabs' | null {
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') return null;
    return parsed.hostname === 'api.fish.audio' ? 'fish-audio' : 'elevenlabs';
  } catch {
    return null;
  }
}

export function isFishAudioUrl(url: string): boolean {
  return speechProvider(url) === 'fish-audio';
}

export function speechModelForEndpoint(previousUrl: string, url: string, model: string): string {
  const previous = speechProvider(previousUrl);
  const next = speechProvider(url);
  if (!previous || !next || previous === next) return model;
  return next === 'fish-audio' ? 's2.1-pro' : 'eleven_multilingual_v2';
}

export interface AudioRequest {
  url: string;
  body: string;
  headers: Record<string, string>;
  cacheKey: string;
  provider: string;
}

export function audioRequest(
  configuration: TextToSpeechConfiguration,
  voice: TextToSpeechVoice | undefined,
  text: string,
): AudioRequest {
  const voiceId = voice?.elevenlabs_voice_id ?? configuration.voiceId;
  if (isFishAudioUrl(configuration.baseUrl)) {
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
      provider: 'FishAudio',
    };
  }

  const url = `${configuration.baseUrl}/${encodeURIComponent(voiceId)}`
    + `?output_format=${encodeURIComponent(configuration.outputFormat)}`;
  const body = JSON.stringify({
    text,
    model_id: configuration.model,
    ...(voice && Object.keys(voice.settings).length > 0 ? { voice_settings: voice.settings } : {}),
  });
  return {
    url,
    body,
    headers: { 'Content-Type': 'application/json', 'xi-api-key': configuration.apiKey },
    cacheKey: `${url}\n${body}`,
    provider: 'ElevenLabs',
  };
}
