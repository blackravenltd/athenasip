import { useCallback, useEffect, useState } from 'react';

export interface Devices {
  microphones: MediaDeviceInfo[];
  cameras: MediaDeviceInfo[];
  speakers: MediaDeviceInfo[];
}

const NONE: Devices = { microphones: [], cameras: [], speakers: [] };

/** Whether this browser can send audio to a chosen output (`setSinkId`). */
export function canChooseSpeaker(): boolean {
  return typeof HTMLMediaElement !== 'undefined' && 'setSinkId' in HTMLMediaElement.prototype;
}

/**
 * The browser's inputs and outputs, and a way to ask for the microphone.
 *
 * Device labels are empty until the page is allowed to use a device; `named`
 * says whether they are filled in. The phone asks before registering, because
 * changing a permission in Chrome reloads the page.
 */
export function useDevices(): { devices: Devices; named: boolean; ask: (video: boolean) => Promise<void>; error?: string } {
  const [devices, setDevices] = useState<Devices>(NONE);
  const [error, setError] = useState<string>();
  const media = typeof navigator === 'undefined' ? undefined : navigator.mediaDevices;

  const refresh = useCallback(async () => {
    if (!media?.enumerateDevices) return;
    const all = await media.enumerateDevices();
    setDevices({
      microphones: all.filter((device) => device.kind === 'audioinput'),
      cameras: all.filter((device) => device.kind === 'videoinput'),
      speakers: all.filter((device) => device.kind === 'audiooutput'),
    });
  }, [media]);

  useEffect(() => {
    if (!media) return;
    void refresh().catch(() => undefined);
    media.addEventListener?.('devicechange', refresh);
    return () => media.removeEventListener?.('devicechange', refresh);
  }, [media, refresh]);

  const ask = useCallback(async (video: boolean) => {
    if (!media) return;
    setError(undefined);
    try {
      const stream = await media.getUserMedia({ audio: true, video });
      stream.getTracks().forEach((track) => track.stop());
      await refresh();
    } catch (cause) {
      setError(cause instanceof DOMException && cause.name === 'NotAllowedError'
        ? 'The browser was refused the microphone. Allow it from the address bar, and the page will reload.'
        : `No microphone: ${cause instanceof Error ? cause.message : String(cause)}`);
    }
  }, [media, refresh]);

  const named = [...devices.microphones, ...devices.cameras].some((device) => device.label);
  return { devices, named, ask, error };
}

/**
 * Whether a call is sending silence: the sent audio's accumulated energy has
 * not grown across the last `readings` samples, taken one a second. A real
 * microphone in a quiet room still moves it; a dead or loopback input does not.
 */
export function sendingSilence(energies: readonly (number | undefined)[], readings = 5): boolean {
  if (energies.length < readings) return false;
  const recent = energies.slice(-readings);
  if (recent.some((energy) => energy === undefined)) return false;
  return (recent[recent.length - 1] as number) - (recent[0] as number) < 1e-6;
}
