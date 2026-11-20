import { describe, expect, it } from 'vitest';
import type { CallRecord } from '../api/types';
import { addressOf, historyFor, when } from './PhoneAside';

const record = (caller: string | null, callee: string | null, answered: boolean): CallRecord => ({
  id: `${caller}-${callee}`, caller, callee, created_at: null, answered_at: answered ? '2026-10-03T19:00:05Z' : null,
  ended_at: '2026-10-03T19:03:09Z', duration: answered ? 184 : 0, nodes: [], media_engine: null,
});

describe('addressOf', () => {
  it('reduces two spellings of one address to the same thing', () => {
    expect(addressOf('sip:1001@10.101.1.20')).toBe('1001@10.101.1.20');
    expect(addressOf('SIP:Bob@Example.org:5060;transport=ws')).toBe('bob@example.org');
    expect(addressOf('sip:10.101.1.20')).toBeUndefined();
    expect(addressOf(null)).toBeUndefined();
  });
});

describe('historyFor', () => {
  it('keeps the calls this line made or took, as it saw them', () => {
    const entries = historyFor([
      record('sip:1001@h', 'sip:1002@h', true),
      record('sip:1002@h', 'sip:1001@h;transport=ws', false),
      record('sip:1002@h', 'sip:1003@h', true),
      record(null, null, false),
    ], 'sip:1001@h');
    expect(entries.map(({ direction, other, answered }) => ({ direction, other, answered }))).toEqual([
      { direction: 'outgoing', other: 'sip:1002@h', answered: true },
      { direction: 'incoming', other: 'sip:1002@h', answered: false },
    ]);
  });
});

describe('when', () => {
  const now = Date.parse('2026-10-03T20:00:00Z');
  it('says how long ago, then the date', () => {
    expect(when(now - 20_000, now)).toBe('just now');
    expect(when(now - 15 * 60_000, now)).toBe('15 min ago');
    expect(when(now - 3 * 3_600_000, now)).toBe('3 h ago');
    expect(when(undefined, now)).toBe('');
  });
});
