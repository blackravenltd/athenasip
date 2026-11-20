import { describe, expect, it } from 'vitest';
import { loadSettings, saveSettings } from './settings';

function memory() {
  const items = new Map<string, string>();
  return { getItem: (key: string) => items.get(key) ?? null, setItem: (key: string, value: string) => { items.set(key, value); }, items };
}

describe('phone settings', () => {
  it('remembers the URI, the socket and the devices, and never a password', () => {
    const storage = memory();
    saveSettings({ uri: 'sip:1001@10.101.1.20', socket: 'wss://10.101.1.20:8089', microphone: 'mic-2', password: 'secret' } as never, storage);
    expect([...storage.items.values()].join()).not.toContain('secret');
    expect(loadSettings(storage)).toEqual({ uri: 'sip:1001@10.101.1.20', socket: 'wss://10.101.1.20:8089', microphone: 'mic-2' });
  });

  it('works from nothing when storage is missing, refuses, or holds rubbish', () => {
    expect(loadSettings(undefined)).toEqual({});
    expect(loadSettings({ getItem: () => { throw new Error('denied'); } })).toEqual({});
    expect(loadSettings({ getItem: () => '{not json' })).toEqual({});
    expect(loadSettings({ getItem: () => JSON.stringify({ uri: 7, socket: 'ws://x' }) })).toEqual({ socket: 'ws://x' });
    expect(() => saveSettings({ uri: 'sip:a@b' }, { setItem: () => { throw new Error('full'); } })).not.toThrow();
  });
});
