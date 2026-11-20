// @vitest-environment jsdom
import { renderHook } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { CallState } from '../softphone/Softphone';
import { startTone, toneFor, useIncomingNotification, useRinging } from './ringing';

/** Enough of Web Audio to see what a tone plays and that it stops. */
function fakeAudio() {
  const made: Array<{ frequencies: number[]; started: number; stopped: number; closed: boolean; levels: Array<[number, number]> }> = [];
  const create = () => {
    const record = { frequencies: [] as number[], started: 0, stopped: 0, closed: false, levels: [] as Array<[number, number]> };
    made.push(record);
    return {
      currentTime: 0,
      destination: {},
      createGain: () => ({ gain: { value: 0, setValueAtTime: (value: number, at: number) => { record.levels.push([value, at]); } }, connect: () => undefined }),
      createOscillator: () => {
        const oscillator = { frequency: { value: 0 }, connect: () => undefined, start: () => { record.started += 1; record.frequencies.push(oscillator.frequency.value); }, stop: () => { record.stopped += 1; } };
        return oscillator;
      },
      resume: async () => undefined,
      close: async () => { record.closed = true; },
    } as unknown as AudioContext;
  };
  return { made, create };
}

afterEach(() => { vi.unstubAllGlobals(); });

describe('toneFor', () => {
  it('rings for a call arriving and rings back for one ringing at the far end, and is otherwise quiet', () => {
    expect(toneFor('incoming')).toBe('ring');
    expect(toneFor('ringing')).toBe('ringback');
    for (const call of ['idle', 'calling', 'connected', 'ended', 'failed'] as CallState[]) expect(toneFor(call)).toBeUndefined();
  });
});

describe('startTone', () => {
  it('plays a pair of frequencies in a double cadence, and stops everything it started', () => {
    const { made, create } = fakeAudio();
    const stop = startTone('ring', create);
    expect(made[0].frequencies).toEqual([400, 450]);
    expect(made[0].levels.slice(0, 4)).toEqual([[0.18, 0], [0, 0.4], [0.18, 0.6], [0, 1]]);
    stop();
    expect(made[0]).toMatchObject({ stopped: 2, closed: true });
  });

  it('is quiet rather than failing where there is no audio to be had', () => {
    expect(() => startTone('ring', () => { throw new Error('no audio'); })()).not.toThrow();
  });
});

describe('useRinging', () => {
  it('rings while a call is incoming and stops when it is answered', () => {
    const { made, create } = fakeAudio();
    const { rerender } = renderHook(({ call }) => useRinging(call, create), { initialProps: { call: 'incoming' as CallState } });
    expect(made).toHaveLength(1);
    rerender({ call: 'connected' });
    expect(made[0].closed).toBe(true);
    expect(made).toHaveLength(1);
  });
});

describe('useIncomingNotification', () => {
  it('notifies of a call only while the tab is hidden and notifications are allowed, and closes it after', () => {
    const shown: Array<{ title: string; body?: string; closed: boolean }> = [];
    class FakeNotification {
      static permission = 'granted';
      record: { title: string; body?: string; closed: boolean };
      onclick: (() => void) | null = null;
      constructor(title: string, options: NotificationOptions) { this.record = { title, body: options.body, closed: false }; shown.push(this.record); }
      close() { this.record.closed = true; }
    }
    vi.stubGlobal('Notification', FakeNotification);
    const hidden = vi.spyOn(document, 'hidden', 'get').mockReturnValue(false);

    const { rerender } = renderHook(({ call }) => useIncomingNotification(call, 'athenaphone'), { initialProps: { call: 'incoming' as CallState } });
    expect(shown).toHaveLength(0);

    hidden.mockReturnValue(true);
    rerender({ call: 'idle' });
    rerender({ call: 'incoming' });
    expect(shown).toEqual([{ title: 'Incoming call', body: 'athenaphone', closed: false }]);
    rerender({ call: 'connected' });
    expect(shown[0].closed).toBe(true);
  });
});
