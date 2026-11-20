// @vitest-environment jsdom
import { EventEmitter } from 'node:events';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { UA, UAConfiguration } from 'jssip/lib/UA';
import type { AdminApi } from '../api/AdminApi';
import { ApiError } from '../api/errors';
import type { CallRecord, ClientConfig, Registration, Role, Subscriber, SubscriberLine } from '../api/types';
import type { SipStack } from '../softphone/Softphone';
import PhoneHost from './PhoneHost';
import { loadSettings, saveSettings } from './settings';

class FakeAgent extends EventEmitter {
  sessions: FakeSession[] = [];
  calls: Array<{ target: string; options: { pcConfig?: RTCConfiguration; mediaConstraints?: MediaStreamConstraints } }> = [];
  constructor(readonly configuration: UAConfiguration) { super(); }
  start() {}
  stop() {}
  call(target: string, options: { pcConfig?: RTCConfiguration; mediaConstraints?: MediaStreamConstraints }) {
    this.calls.push({ target, options });
    const session = new FakeSession();
    this.sessions.push(session);
    this.emit('newRTCSession', { originator: 'local', session, request: {} });
  }
}

class FakeSession extends EventEmitter {
  log: string[] = [];
  remote_identity = { uri: { toString: () => 'sip:athenaphone@10.101.1.20' } };
  answer(options: { mediaConstraints?: MediaStreamConstraints }) { this.log.push(`answer ${JSON.stringify(options.mediaConstraints)}`); }
  terminate(options?: { status_code?: number }) { this.log.push(`terminate ${options?.status_code ?? ''}`.trim()); }
  mute() { this.log.push('mute'); }
  unmute() { this.log.push('unmute'); }
  hold() { this.log.push('hold'); return true; }
  unhold() { this.log.push('unhold'); return true; }
  sendDTMF(tone: string) { this.log.push(`dtmf ${tone}`); }
}

function stack(): SipStack & { agents: FakeAgent[] } {
  const agents: FakeAgent[] = [];
  return {
    agents,
    createUserAgent: (configuration) => {
      const agent = new FakeAgent(configuration);
      agents.push(agent);
      return agent as unknown as UA;
    },
    createSocket: (url) => ({ url } as unknown as UAConfiguration['sockets']),
  };
}

const CONFIG: ClientConfig = {
  websocket_uri: 'wss://10.101.1.20:8089',
  transports: [{ transport: 'ws', address: '10.101.1.20', port: 8088, uri: 'sip:10.101.1.20:8088;transport=ws' }],
  ice_servers: [{ urls: 'stun:10.101.1.20:3478' }],
};

/** A node with no call history and nobody registered, whose client configuration any line's credentials open. */
function node(): AdminApi & { asked: string[] } {
  const asked: string[] = [];
  return {
    asked,
    subscriberConfig: async (line: SubscriberLine) => { asked.push(`${line.user}@${line.realm} ${line.password}`); return CONFIG; },
    listCallRecords: async () => [],
    listRegistrations: async () => [],
  } as unknown as AdminApi & { asked: string[] };
}

const STATUS: readonly Role[] = ['view-cluster-status'];

function show(sip = stack(), visible = true, api: AdminApi = node(), roles: readonly Role[] = STATUS) {
  const slot = document.createElement('div');
  document.body.appendChild(slot);
  const view = render(<MemoryRouter><PhoneHost api={api} roles={roles} visible={visible} indicator={slot} stack={sip} /></MemoryRouter>);
  return { sip, slot, view };
}

async function registered(sip = stack(), api: AdminApi = node(), roles: readonly Role[] = STATUS) {
  saveSettings({ uri: 'sip:1001@10.101.1.20', socket: 'ws://10.101.1.20:8088' });
  const shown = show(sip, true, api, roles);
  fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'pw' } });
  fireEvent.click(screen.getByRole('button', { name: 'Register' }));
  act(() => { sip.agents[0].emit('registered', {}); });
  await screen.findByText('Registered as 1001');
  return shown;
}

beforeEach(() => { window.localStorage.clear(); });
afterEach(() => { document.body.innerHTML = ''; vi.unstubAllGlobals(); });

/** A browser with two microphones and a camera, labelled only once the microphone is allowed. */
function withDevices() {
  let allowed = false;
  const devices = [
    { kind: 'audioinput', deviceId: 'default', label: 'Default' },
    { kind: 'audioinput', deviceId: 'mic-builtin', label: 'MacBook Pro Microphone' },
    { kind: 'audioinput', deviceId: 'mic-blackhole', label: 'BlackHole 2ch' },
    { kind: 'videoinput', deviceId: 'cam-1', label: 'FaceTime HD Camera' },
  ];
  const asked: MediaStreamConstraints[] = [];
  vi.stubGlobal('navigator', {
    ...navigator,
    mediaDevices: {
      enumerateDevices: async () => devices.map((device) => ({ ...device, label: allowed ? device.label : '' })),
      getUserMedia: async (constraints: MediaStreamConstraints) => { asked.push(constraints); allowed = true; return { getTracks: () => [] }; },
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
    },
  });
  return asked;
}

describe('PhoneHost', () => {
  it('signs in to a line where the node says, asked with its own credentials, and remembers everything but the password', async () => {
    const sip = stack();
    const api = node();
    show(sip, true, api, []);
    expect((screen.getByLabelText('WebSocket') as HTMLInputElement).value).toBe('');
    fireEvent.change(screen.getByLabelText('SIP address'), { target: { value: 'sip:1001@10.101.1.20' } });
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'secret' } });
    fireEvent.click(screen.getByRole('button', { name: 'Register' }));

    await vi.waitFor(() => expect(sip.agents).toHaveLength(1));
    expect(api.asked).toEqual(['1001@10.101.1.20 secret']);
    expect(sip.agents[0].configuration).toMatchObject({ uri: 'sip:1001@10.101.1.20', password: 'secret' });
    expect(loadSettings()).toEqual({ uri: 'sip:1001@10.101.1.20' });
    expect(JSON.stringify(window.localStorage)).not.toContain('secret');
  });

  it('does not register when the node refuses the line, and says so', async () => {
    const sip = stack();
    const api = {
      subscriberConfig: async () => { throw new ApiError('unauthorized', 401, 'unauthorized'); },
      listCallRecords: async () => [],
      listRegistrations: async () => [],
    } as unknown as AdminApi;
    show(sip, true, api);
    fireEvent.change(screen.getByLabelText('SIP address'), { target: { value: 'sip:1001@10.101.1.20' } });
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'wrong' } });
    fireEvent.click(screen.getByRole('button', { name: 'Register' }));

    expect((await screen.findByRole('alert')).textContent).toBe('The node did not accept that SIP address and password.');
    expect(sip.agents).toHaveLength(0);
  });

  it('dials an extension from the pad in its own realm, with the node ICE servers', async () => {
    const { sip } = await registered();
    const pad = screen.getByRole('group', { name: 'Dial pad' });
    for (const key of ['1', '0', '0', '2']) fireEvent.click(within(pad).getByRole('button', { name: key }));
    expect((screen.getByLabelText('Number or address') as HTMLInputElement).value).toBe('1002');
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));

    await vi.waitFor(() => expect(sip.agents[0].calls).toHaveLength(1));
    expect(sip.agents[0].calls[0].target).toBe('sip:1002@10.101.1.20');
    expect(sip.agents[0].calls[0].options.pcConfig).toEqual({ iceServers: [{ urls: 'stun:10.101.1.20:3478' }] });
    expect(sip.agents[0].calls[0].options.mediaConstraints).toEqual({ audio: true, video: false });
  });

  it('rings in the top bar, and answers with video or declines', async () => {
    const { sip, slot } = await registered();
    const session = new FakeSession();
    act(() => { sip.agents[0].emit('newRTCSession', { originator: 'remote', session, request: {} }); });

    expect(slot.textContent).toBe('Incoming call from athenaphone');
    fireEvent.click(screen.getByRole('button', { name: 'Answer with video' }));
    await vi.waitFor(() => expect(session.log).toEqual(['answer {"audio":true,"video":true}']));

    const second = new FakeSession();
    act(() => { session.emit('ended', { cause: 'Terminated' }); });
    act(() => { sip.agents[0].emit('newRTCSession', { originator: 'remote', session: second, request: {} }); });
    fireEvent.click(screen.getByRole('button', { name: 'Decline' }));
    expect(second.log).toEqual(['terminate 603']);
  });

  it('on a connected call, sends the pad as tones, and mutes and holds', async () => {
    const { sip } = await registered();
    fireEvent.change(screen.getByLabelText('Number or address'), { target: { value: 'athenaphone' } });
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));
    await vi.waitFor(() => expect(sip.agents[0].sessions).toHaveLength(1));
    const [session] = sip.agents[0].sessions;
    act(() => { session.emit('accepted', {}); });

    fireEvent.click(within(screen.getByRole('group', { name: 'Send tones' })).getByRole('button', { name: '5' }));
    fireEvent.keyDown(window, { key: '#' });
    fireEvent.click(screen.getByRole('button', { name: 'Mute' }));
    fireEvent.click(screen.getByRole('button', { name: 'Hold' }));
    expect(screen.getByLabelText('Tones sent').textContent).toBe('5#');
    expect(screen.getByRole('status').textContent).toContain('on hold, muted');
    expect(session.log).toEqual(['dtmf 5', 'dtmf #', 'mute', 'hold']);

    fireEvent.click(screen.getByRole('button', { name: 'Hang up' }));
    expect(session.log).toContain('terminate');
  });

  it('asks for the microphone before a line, then calls with the one chosen, remembered', async () => {
    const asked = withDevices();
    const sip = stack();
    saveSettings({ uri: 'sip:1001@10.101.1.20', socket: 'ws://10.101.1.20:8088' });
    show(sip);
    fireEvent.click(screen.getByRole('button', { name: 'Allow the microphone' }));
    const microphone = await screen.findByLabelText('Microphone') as HTMLSelectElement;
    expect(asked).toEqual([{ audio: true, video: false }]);
    expect([...microphone.options].map((option) => option.text)).toEqual(['Browser default', 'MacBook Pro Microphone', 'BlackHole 2ch']);

    fireEvent.change(microphone, { target: { value: 'mic-builtin' } });
    expect(loadSettings().microphone).toBe('mic-builtin');

    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'pw' } });
    fireEvent.click(screen.getByRole('button', { name: 'Register' }));
    act(() => { sip.agents[0].emit('registered', {}); });
    fireEvent.change(await screen.findByLabelText('Number or address'), { target: { value: '1002' } });
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));
    await vi.waitFor(() => expect(sip.agents[0].calls).toHaveLength(1));
    expect(sip.agents[0].calls[0].options.mediaConstraints).toEqual({ audio: { deviceId: { exact: 'mic-builtin' } }, video: false });
  });

  it('lists this line recent calls and the realm directory, and calls back from either', async () => {
    const records: CallRecord[] = [
      { id: 'a', caller: 'sip:1001@10.101.1.20', callee: 'sip:athenaphone@10.101.1.20', created_at: '2026-10-03T19:00:00Z', answered_at: '2026-10-03T19:00:05Z', ended_at: '2026-10-03T19:03:09Z', duration: 184, nodes: [], media_engine: null },
      { id: 'b', caller: 'sip:1002@10.101.1.20;transport=ws', callee: 'sip:1001@10.101.1.20', created_at: null, answered_at: null, ended_at: '2026-10-03T18:00:00Z', duration: 0, nodes: [], media_engine: null },
      { id: 'c', caller: 'sip:1002@10.101.1.20', callee: 'sip:1003@10.101.1.20', created_at: null, answered_at: null, ended_at: null, duration: 0, nodes: [], media_engine: null },
    ];
    const subscribers = ['1001', '1002', 'athenaphone'].map((user) => ({ uri: `sip:${user}@10.101.1.20`, user, realm: '10.101.1.20' })) as Subscriber[];
    const registrations = [{ subscriber: 'sip:athenaphone@10.101.1.20' }] as Registration[];
    const api = {
      subscriberConfig: async () => CONFIG,
      listCallRecords: async () => records,
      listSubscribers: async (realm: string) => (realm === '10.101.1.20' ? subscribers : []),
      listRegistrations: async () => registrations,
    } as unknown as AdminApi;
    const sip = stack();
    await registered(sip, api, ['view-cluster-status', 'manage-realm-subscribers']);

    const recent = await screen.findByRole('list', { name: 'Recent calls' });
    expect([...recent.querySelectorAll('li')].map((row) => row.querySelector('.phone-list-detail')!.textContent!.replace(/, [^,]+ago$|, (\d+ \w+|\w+ \d+)$/, ''))).toEqual(['Outgoing, 3:04', 'Missed']);
    const directory = await screen.findByRole('list', { name: 'Directory' });
    expect(within(directory).getAllByRole('listitem').map((row) => row.querySelector('.phone-list-detail')!.textContent)).toEqual(['Not registered', 'Registered']);

    fireEvent.click(within(directory).getByRole('button', { name: 'Call athenaphone' }));
    await vi.waitFor(() => expect(sip.agents[0].calls).toHaveLength(1));
    expect(sip.agents[0].calls[0].target).toBe('sip:athenaphone@10.101.1.20');
  });

  it('says why there is no history or directory when the console user has no role for them', async () => {
    await registered(stack(), node(), []);
    expect(screen.getByText(/call records, which need the View cluster status role/)).toBeTruthy();
    expect(screen.getByText(/needs the Manage subscribers or View cluster status role/)).toBeTruthy();
  });

  it('calls with the node ICE servers for a console user with no roles, asked with the line', async () => {
    const api = node();
    const sip = stack();
    await registered(sip, api, []);
    fireEvent.change(screen.getByLabelText('Number or address'), { target: { value: '1002' } });
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));
    await vi.waitFor(() => expect(sip.agents[0].calls).toHaveLength(1));
    expect(sip.agents[0].calls[0].options.pcConfig).toEqual({ iceServers: [{ urls: 'stun:10.101.1.20:3478' }] });
    expect(api.asked).toEqual(['1001@10.101.1.20 pw']);
  });

  it('keeps the call, and its indicator, while another screen is shown', async () => {
    const sip = stack();
    const { slot, view } = await registered(sip);
    fireEvent.change(screen.getByLabelText('Number or address'), { target: { value: 'athenaphone' } });
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));
    await vi.waitFor(() => expect(sip.agents[0].sessions).toHaveLength(1));
    act(() => { sip.agents[0].sessions[0].emit('accepted', {}); });

    view.rerender(<MemoryRouter><PhoneHost api={node()} roles={STATUS} visible={false} indicator={slot} stack={sip} /></MemoryRouter>);
    expect(screen.queryByRole('heading', { name: 'Phone' })).toBeNull();
    expect(slot.textContent).toMatch(/^Connected athenaphone 0:0\d$/);
    expect(sip.agents[0].sessions[0].log).toEqual([]);
  });
});
