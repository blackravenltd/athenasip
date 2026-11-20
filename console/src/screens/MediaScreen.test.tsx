// @vitest-environment jsdom
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { FakeAdminApi } from '../api/FakeAdminApi';
import { MediaScreen, subscriberOf, describeReoffer, engineState } from './MediaScreen';

describe('engineState', () => {
  it('calls no engine a choice and a lost engine a fault', () => {
    expect(engineState({ engine: null, connected: false, capabilities: [] }).tone).toBe('warn');
    expect(engineState({ engine: 'rtpengine', connected: false, capabilities: [] }).tone).toBe('down');
    expect(engineState({ engine: 'builtin', connected: true, capabilities: ['bridge'] })).toEqual({ label: 'builtin, connected', tone: 'ok' });
  });
});

describe('subscriberOf', () => {
  it('reads the realm and user out of an address of record', () => {
    expect(subscriberOf('sip:reception@example.org')).toEqual({ user: 'reception', realm: 'example.org' });
    expect(subscriberOf('sip:example.org')).toBeUndefined();
  });
});

describe('describeReoffer', () => {
  it('says what was refused and what was taken, or that both were', () => {
    const base = { subscriber: 'sip:a@b', count: 2, last_at: '2026-10-02T00:00:00Z' };
    expect(describeReoffer({ ...base, rejected: 'webrtc', took: 'rtp', suggested_media_profile: 'rtp' })).toBe('Refused WebRTC and took Plain RTP, 2 times.');
    expect(describeReoffer({ ...base, count: 1, rejected: 'rtp', took: null, suggested_media_profile: null })).toBe('Refused Plain RTP and then the other as well, once.');
  });
});

describe('MediaScreen', () => {
  it('sets a re-offered subscriber’s profile to what its phone took, when asked', async () => {
    const api = new FakeAdminApi();
    render(<MediaScreen api={api} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Set sip:reception@example.org to Plain RTP' }));
    expect(await screen.findByText('Set to Plain RTP')).toBeTruthy();
    expect((await api.listSubscribers('example.org'))[0].behaviour.media_profile).toBe('rtp');
  });


  it('shows the media engine and what it can do', async () => {
    render(<MediaScreen api={new FakeAdminApi()} />);
    expect(await screen.findByText('builtin, connected')).toBeTruthy();
    expect(screen.getByText('bridge')).toBeTruthy();
  });

  it('shows each realm’s media policy in words, marking what is the server’s', async () => {
    render(<MediaScreen api={new FakeAdminApi()} />);
    const row = await screen.findByRole('group', { name: 'sip.athenasip.org' });
    expect(row.textContent).toContain('Every leg is WebRTC');
    const tags = [...row.parentElement!.querySelectorAll('.record-tag')].map((tag) => tag.textContent);
    expect(tags).toEqual(['Relayed (server default)', 'WebRTC']);
  });

  it('changes only the media policy of a realm', async () => {
    const api = new FakeAdminApi();
    const before = (await api.listRealms()).find((realm) => realm.name === 'example.org')!;
    render(<MediaScreen api={api} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Edit media for example.org' }));
    const profile = within(screen.getByRole('dialog')).getByLabelText('Media profile');
    // blackraven inherits its profile, so the server's is shown.
    expect(within(profile).getByRole('option', { name: 'Server default (mirror the caller)' })).toBeTruthy();
    fireEvent.change(profile, { target: { value: 'rtp' } });
    fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Save media policy' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());

    const after = (await api.listRealms()).find((realm) => realm.name === 'example.org');
    expect(after).toEqual({
      ...before,
      behaviour: { ...before.behaviour, media_profile: 'rtp' },
      behaviour_effective: { ...before.behaviour_effective, media_profile: 'rtp' },
    });
  });

  it('puts a setting back to the server’s default', async () => {
    const api = new FakeAdminApi();
    render(<MediaScreen api={api} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Edit media for sip.athenasip.org' }));
    fireEvent.change(within(screen.getByRole('dialog')).getByLabelText('Media profile'), { target: { value: '' } });
    fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Save media policy' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect((await api.listRealms()).find((realm) => realm.name === 'sip.athenasip.org')?.behaviour.media_profile).toBeNull();
  });
});
