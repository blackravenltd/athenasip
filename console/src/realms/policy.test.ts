import { describe, expect, it } from 'vitest';
import type { Realm } from '../api/types';
import { DEFAULT_POLICY, describeBehaviour, describeSeconds, policySettings, serverDefaults } from './policy';

function realm(behaviour: Realm['behaviour'], effective: Realm['behaviour_effective']): Realm {
  return {
    name: 'r', id: 1, nonce_expiry: 3600, registration_timeout: 5000, registration_minimum: 0,
    behaviour, behaviour_effective: effective, behaviour_default: { media_anchor: true, media_profile: 'mirror', qualify_interval: 0, rewrite_contact: false },
  };
}

describe('policySettings', () => {
  it('turns the form into the numbers the API wants', () => {
    expect(policySettings({ ...DEFAULT_POLICY, registration_minimum: ' 60 ' })).toEqual({
      settings: {
        registration_timeout: 5000, registration_minimum: 60, nonce_expiry: 3600,
        behaviour: { media_anchor: null, media_profile: null, qualify_interval: null, rewrite_contact: null },
      },
    });
  });

  it('sends a setting the realm inherits as null, which is how it goes back to the server’s', () => {
    const checked = policySettings({ ...DEFAULT_POLICY, media_anchor: false });
    expect(checked).toMatchObject({ settings: { behaviour: { media_anchor: false, media_profile: null } } });
  });

  it('takes probing as empty for the server’s, 0 for never, or within the node’s bounds', () => {
    expect(policySettings({ ...DEFAULT_POLICY, qualify_interval: '0' })).toMatchObject({ settings: { behaviour: { qualify_interval: 0 } } });
    expect(policySettings({ ...DEFAULT_POLICY, qualify_interval: ' 30 ' })).toMatchObject({ settings: { behaviour: { qualify_interval: 30 } } });
    expect(policySettings({ ...DEFAULT_POLICY, qualify_interval: '3' })).toHaveProperty('problem');
    expect(policySettings({ ...DEFAULT_POLICY, qualify_interval: '90000' })).toHaveProperty('problem');
    expect(policySettings({ ...DEFAULT_POLICY, qualify_interval: '1.5' })).toHaveProperty('problem');
  });

  it('refuses what is not a whole number of seconds', () => {
    expect(policySettings({ ...DEFAULT_POLICY, nonce_expiry: '1.5' })).toEqual({ problem: 'The nonce lifetime must be a whole number of seconds.' });
    expect(policySettings({ ...DEFAULT_POLICY, registration_timeout: '-1' })).toHaveProperty('problem');
  });

  it('refuses a realm nothing could register in', () => {
    expect(policySettings({ ...DEFAULT_POLICY, registration_timeout: '0' })).toHaveProperty('problem');
    expect(policySettings({ ...DEFAULT_POLICY, registration_minimum: '6000' })).toHaveProperty('problem');
    expect(policySettings({ ...DEFAULT_POLICY, nonce_expiry: '0' })).toHaveProperty('problem');
  });
});

describe('serverDefaults', () => {
  it('reads the server’s values from a realm, whatever the realm sets itself', () => {
    expect(serverDefaults(realm({ media_anchor: false, media_profile: 'webrtc', qualify_interval: 30, rewrite_contact: true }, { media_anchor: false, media_profile: 'webrtc', qualify_interval: 30, rewrite_contact: true })))
      .toEqual({ media_anchor: true, media_profile: 'mirror', qualify_interval: 0, rewrite_contact: false });
    expect(serverDefaults(undefined)).toEqual({});
  });
});

describe('describeBehaviour', () => {
  it('says what the settings come to, and which are the server’s', () => {
    const described = describeBehaviour(realm({ media_anchor: false, media_profile: null, qualify_interval: null, rewrite_contact: null }, { media_anchor: false, media_profile: 'mirror', qualify_interval: 0, rewrite_contact: false }));
    expect(described).toMatchObject({ relayed: false, anchor: { label: 'Direct', inherited: false }, profile: { label: 'Mirror the caller', inherited: true } });
  });
});

describe('describeSeconds', () => {
  it('says hours and minutes when they are whole', () => {
    expect(describeSeconds(3600)).toBe('1h');
    expect(describeSeconds(120)).toBe('2m');
    expect(describeSeconds(5000)).toBe('5000s');
  });
});
