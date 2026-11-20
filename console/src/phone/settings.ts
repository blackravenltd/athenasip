/**
 * What the phone remembers between visits: the SIP URI, the WebSocket and the
 * devices, none of which is a secret. The password is never stored.
 *
 * Browser storage can be missing, full or refused, so every read and write
 * may fail, and the phone then starts from blank fields.
 */
export interface PhoneSettings {
  socket?: string;
  uri?: string;
  /** `deviceId`s; absent for the browser's default. */
  microphone?: string;
  camera?: string;
  speaker?: string;
}

const KEY = 'athenasip.phone.v1';
const FIELDS: ReadonlyArray<keyof PhoneSettings> = ['socket', 'uri', 'microphone', 'camera', 'speaker'];

export function loadSettings(storage: Pick<Storage, 'getItem'> | undefined = safeStorage()): PhoneSettings {
  try {
    const raw = storage?.getItem(KEY);
    if (!raw) return {};
    const parsed: unknown = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object') return {};
    const settings: PhoneSettings = {};
    for (const field of FIELDS) {
      const value = (parsed as Record<string, unknown>)[field];
      if (typeof value === 'string' && value) settings[field] = value;
    }
    return settings;
  } catch {
    return {};
  }
}

/** Writes only the known fields, so nothing passed in by mistake (a password) can reach storage. */
export function saveSettings(settings: PhoneSettings, storage: Pick<Storage, 'setItem'> | undefined = safeStorage()): void {
  const kept: PhoneSettings = {};
  for (const field of FIELDS) {
    const value = settings[field];
    if (value) kept[field] = value;
  }
  try {
    storage?.setItem(KEY, JSON.stringify(kept));
  } catch {
    // The phone works without stored settings.
  }
}

function safeStorage(): Storage | undefined {
  try {
    return typeof window === 'undefined' ? undefined : window.localStorage;
  } catch {
    return undefined;
  }
}
