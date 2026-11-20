import { describe, expect, it } from 'vitest';
import { fixtureFromEnvironment, softphoneUrl } from '../../e2e/fixture';

/** How the end-to-end spec reads `generated/fixture.env`: which phase it is in, and what the page is told. */
describe('fixtureFromEnvironment', () => {
  it('is the direct phase when the engine advertises the node’s own address, or nothing is said', () => {
    expect(fixtureFromEnvironment({}).relay).toBeUndefined();
    const fixture = fixtureFromEnvironment({ ATHENA_INTEROP_PUBLIC_ADDRESS: '127.0.0.1', ATHENA_INTEROP_RTPENGINE_ADVERTISE: '127.0.0.1' });
    expect(fixture.relay).toBeUndefined();
    expect(new URL(softphoneUrl(fixture, '1001', {})).searchParams.has('relay')).toBe(false);
  });

  it('is the relay phase when the engine advertises an address only TURN can reach', () => {
    const fixture = fixtureFromEnvironment({
      ATHENA_INTEROP_PUBLIC_ADDRESS: '127.0.0.1',
      ATHENA_INTEROP_RTPENGINE_ADVERTISE: '172.32.0.30',
      ATHENA_INTEROP_TURN_MIN: '22200',
      ATHENA_INTEROP_TURN_MAX: '22250',
    });
    expect(fixture).toMatchObject({ advertise: '172.32.0.30', relay: { min: 22200, max: 22250 } });
    const url = new URL(softphoneUrl(fixture, '1001', { ice: [{ urls: 'turn:127.0.0.1:3478' }] }));
    expect(url.searchParams.get('relay')).toBe('1');
    expect(JSON.parse(url.searchParams.get('ice')!)).toEqual([{ urls: 'turn:127.0.0.1:3478' }]);
  });

  it('refuses a relay phase with no relay range, rather than asserting against nothing', () => {
    expect(() => fixtureFromEnvironment({ ATHENA_INTEROP_RTPENGINE_ADVERTISE: '172.32.0.30' })).toThrow(/relay range/);
  });
});
