import { describe, expect, it } from 'vitest';
import { callTimer, dialTarget, userOf } from './dial';

describe('dialTarget', () => {
  const own = 'sip:1001@10.101.1.20';
  it('dials an extension in this phone realm, and an address as given', () => {
    expect(dialTarget('1002', own)).toBe('sip:1002@10.101.1.20');
    expect(dialTarget(' athenaphone ', own)).toBe('sip:athenaphone@10.101.1.20');
    expect(dialTarget('bob@example.org', own)).toBe('sip:bob@example.org');
    expect(dialTarget('sips:bob@example.org', own)).toBe('sips:bob@example.org');
  });

  it('has nothing to dial from nothing, or from a bare name with no realm to put it in', () => {
    expect(dialTarget('   ', own)).toBeUndefined();
    expect(dialTarget('1002', '')).toBeUndefined();
  });
});

describe('userOf', () => {
  it('names the user part', () => {
    expect(userOf('sip:athenaphone@10.101.1.20')).toBe('athenaphone');
    expect(userOf('sip:10.101.1.20')).toBe('10.101.1.20');
    expect(userOf(null)).toBe('unknown');
  });
});

describe('callTimer', () => {
  it('counts minutes and seconds, and hours past the hour', () => {
    expect(callTimer(0)).toBe('0:00');
    expect(callTimer(65_400)).toBe('1:05');
    expect(callTimer(3_725_000)).toBe('1:02:05');
  });
});
