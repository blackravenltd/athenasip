// @vitest-environment jsdom
import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { FakeAdminApi } from '../api/FakeAdminApi';
import { RegistrationsScreen, contactTransport, describeExpiry, describeProbe } from './RegistrationsScreen';

describe('describeExpiry', () => {
  const now = 1_000_000;

  it('reads the server’s Unix seconds, and treats a nearly-expired binding as ordinary', () => {
    expect(describeExpiry(now + 30, now)).toEqual({ label: '30s left', tone: 'warn' });
    expect(describeExpiry(now + 240, now)).toEqual({ label: '4m left', tone: 'ok' });
  });

  it('calls an elapsed binding expired rather than reporting negative time', () => {
    expect(describeExpiry(now - 5, now)).toEqual({ label: 'Expired', tone: 'down' });
  });
});

describe('contactTransport', () => {
  it('reads the transport out of the contact, which is the only place the API says it', () => {
    expect(contactTransport('sip:ada@192.168.1.24:5061;transport=tls')).toBe('TLS');
    expect(contactTransport('sip:ada@192.168.1.24:5060')).toBeUndefined();
  });
});

describe('describeProbe', () => {
  const client = { subscriber: 'sip:a@b', contact: 'sip:a@c', interval: 60, said_media_profile: null };
  const now = Date.parse('2026-10-02T10:00:00Z');

  it('treats one missed probe as a lost packet and several as a client gone', () => {
    expect(describeProbe({ ...client, answered_at: '2026-10-02T09:59:48Z', unanswered: 0 }, now)).toEqual({ label: 'Answered 12s ago', tone: 'ok' });
    expect(describeProbe({ ...client, answered_at: null, unanswered: 1 }, now).tone).toBe('warn');
    expect(describeProbe({ ...client, answered_at: null, unanswered: 3 }, now).tone).toBe('down');
  });
});

describe('RegistrationsScreen', () => {
  it('lists the probed clients, with what each said it takes', async () => {
    render(<RegistrationsScreen api={new FakeAdminApi()} />);
    const ada = await screen.findByRole('group', { name: 'Probe of sip:ada@sip.athenasip.org' });
    expect(ada.parentElement!.textContent).toContain('Says Plain RTP');
    const adaweb = screen.getByRole('group', { name: 'Probe of sip:adaweb@sip.athenasip.org' });
    expect(adaweb.parentElement!.textContent).toContain('3 unanswered');
  });


  it('lists the live bindings with their transport and NAT state', async () => {
    render(<RegistrationsScreen api={new FakeAdminApi()} />);
    expect(await screen.findByRole('group', { name: 'sip:ada@sip.athenasip.org' })).toBeTruthy();
    expect(await screen.findByText('WS')).toBeTruthy();
    expect(await screen.findByText('NAT')).toBeTruthy();
  });
});
