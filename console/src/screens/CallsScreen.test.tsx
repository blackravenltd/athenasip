// @vitest-environment jsdom
import { act, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '../api/errors';
import { FakeAdminApi } from '../api/FakeAdminApi';
import type { Call } from '../api/types';
import { CallsScreen, POLL_MS, callDuration, callParties, flow, oneWay } from './CallsScreen';

function call(overrides: Partial<Call> = {}): Call {
  return {
    id: 'c1@host',
    state: 'Connected',
    created_at: '2026-10-01T10:00:00.000Z',
    answered_at: '2026-10-01T10:00:05.000Z',
    participants: [
      { identity: 'sip:a@x', originator: true, profile: 'plain-rtp' },
      { identity: 'sip:b@x', originator: false, profile: 'webrtc' },
    ],
    media: { engine: 'builtin', idle_seconds: 0, legs: [{ participant: null, packets_in: 100, packets_out: 100 }] },
    ...overrides,
  };
}

function legs(...given: Array<{ packets_in?: number; packets_out?: number }>): Call {
  return call({ media: { engine: 'builtin', idle_seconds: 0, legs: given.map((leg) => ({ participant: null, ...leg })) } });
}

describe('callParties', () => {
  it('puts the originator first, whatever order the node lists them in', () => {
    const reversed = call({ participants: [...call().participants].reverse() });
    expect(callParties(reversed)).toEqual({ from: 'sip:a@x', to: 'sip:b@x' });
  });
});

describe('callDuration', () => {
  it('counts from the answer, and from the start for a call still ringing', () => {
    const now = Date.parse('2026-10-01T10:01:10.000Z');
    expect(callDuration(call(), now)).toBe('1:05');
    expect(callDuration(call({ answered_at: null, state: 'Ringing' }), now)).toBe('1:10 unanswered');
  });
});

describe('flow', () => {
  it('tells a direction the engine does not count from one standing still', () => {
    expect(flow(10, undefined, true)).toBe('unreported');
    expect(flow(10, 10, true)).toBe('still');
    expect(flow(10, 20, true)).toBe('moving');
    expect(flow(undefined, 20, false)).toBe('unknown');
  });
});

describe('oneWay', () => {
  it('flags one direction still while another moves', () => {
    expect(oneWay(legs({ packets_in: 200, packets_out: 100 }), legs({ packets_in: 100, packets_out: 100 }))).toBe(true);
  });

  it('does not flag a quiet call, a first reading, or a direction the engine does not count', () => {
    expect(oneWay(legs({ packets_in: 100, packets_out: 100 }), legs({ packets_in: 100, packets_out: 100 }))).toBe(false);
    expect(oneWay(legs({ packets_in: 200, packets_out: 100 }), undefined)).toBe(false);
    // rtpengine: only what each end sent.
    expect(oneWay(legs({ packets_in: 200 }, { packets_in: 200 }), legs({ packets_in: 100 }, { packets_in: 100 }))).toBe(false);
  });

  it('flags one end that stopped sending when the engine counts only what each end sent', () => {
    // rtpengine: the only comparison there is, one end's in against another's.
    expect(oneWay(legs({ packets_in: 200 }, { packets_in: 100 }), legs({ packets_in: 100 }, { packets_in: 100 }))).toBe(true);
  });

  it('does not flag a call that is not connected', () => {
    const ringing = (c: Call) => ({ ...c, state: 'Ringing' as const });
    expect(oneWay(ringing(legs({ packets_in: 200, packets_out: 100 })), ringing(legs({ packets_in: 100, packets_out: 100 })))).toBe(false);
  });
});

describe('CallsScreen', () => {
  afterEach(() => { vi.useRealTimers(); });

  it('lists the live calls, relayed or not', async () => {
    // Only the poll is held, so a slow first render cannot take a second reading.
    vi.useFakeTimers({ toFake: ['setInterval'] });
    render(<CallsScreen api={new FakeAdminApi()} />);
    const relayed = await screen.findByRole('group', { name: 'a84b4c76e66710@192.168.1.24' });
    expect(relayed.textContent).toContain('sip:ada@sip.athenasip.org to sip:adaweb@sip.athenasip.org');
    expect(screen.getByText('Relayed: builtin')).toBeTruthy();
    expect(screen.getByText('Not relayed')).toBeTruthy();
    expect(screen.getAllByText('first reading').length).toBeGreaterThan(0);
  });

  it('reads again on its own, and calls out the leg that hears nothing', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    render(<CallsScreen api={new FakeAdminApi()} />);
    await screen.findByText('Relayed: builtin');
    await act(async () => { await vi.advanceTimersByTimeAsync(POLL_MS); });
    expect(await screen.findByText('One-way audio')).toBeTruthy();
    const leg = screen.getByRole('group', { name: 'Leg 2' });
    expect(within(leg).getByText('nothing new')).toBeTruthy();
  });

  it('drops a call that has ended at the next reading', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    const api = new FakeAdminApi();
    render(<CallsScreen api={api} />);
    await screen.findByText('Not relayed');
    api.endCallElsewhere('3848276298220188511@203.0.113.40');
    await act(async () => { await vi.advanceTimersByTimeAsync(POLL_MS); });
    expect(screen.queryByText('Not relayed')).toBeNull();
  });

  it('stops polling for as long as a 429 asks, then reads again', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    const api = new FakeAdminApi();
    const listCalls = vi.spyOn(api, 'listCalls').mockRejectedValueOnce(new ApiError('too many requests', 429, undefined, 10));
    render(<CallsScreen api={api} />);
    expect(await screen.findByText('The node is limiting requests. Try again in 10 seconds.')).toBeTruthy();
    await act(async () => { await vi.advanceTimersByTimeAsync(POLL_MS * 4); });
    expect(listCalls).toHaveBeenCalledTimes(1);
    await act(async () => { await vi.advanceTimersByTimeAsync(10_000 - POLL_MS * 4); });
    expect(listCalls).toHaveBeenCalledTimes(2);
    expect(await screen.findByText('Not relayed')).toBeTruthy();
  });
});
