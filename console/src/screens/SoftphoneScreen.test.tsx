// @vitest-environment jsdom
import { EventEmitter } from 'node:events';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { UA, UAConfiguration } from 'jssip/lib/UA';
import { READOUT_KEY } from '../softphone/page';
import type { SipStack } from '../softphone/Softphone';
import type { AdminApi } from '../api/AdminApi';
import type { ClientConfig, SubscriberLine } from '../api/types';
import { SoftphoneScreen, callInProgress, defaultConnection, describeCallState, describeVideoLine, signallingUri } from './SoftphoneScreen';

class FakeAgent extends EventEmitter {
  sessions: FakeSession[] = [];
  stopped = 0;
  calls: Array<{ target: string; options: { pcConfig?: RTCConfiguration; mediaConstraints?: MediaStreamConstraints } }> = [];
  constructor(readonly configuration: UAConfiguration) { super(); }
  start() {}
  stop() { this.stopped += 1; }
  call(target: string, options: { pcConfig?: RTCConfiguration; mediaConstraints?: MediaStreamConstraints }) {
    this.calls.push({ target, options });
    const session = new FakeSession();
    this.sessions.push(session);
    this.emit('newRTCSession', { originator: 'local', session, request: {} });
  }
}

class FakeSession extends EventEmitter {
  answered = 0;
  terminated = 0;
  remote_identity = { uri: { toString: () => 'sip:1002@example.com' } };
  answer() { this.answered += 1; }
  terminate() { this.terminated += 1; }
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

beforeEach(() => {
  // jsdom has no media devices and no AudioContext, so the meters warn.
  vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
  vi.restoreAllMocks();
});

/** Only what the softphone asks of the node. */
function node(config: ClientConfig): AdminApi & { asked: string[] } {
  const api = { asked: [] as string[], subscriberConfig: async (line: SubscriberLine) => { api.asked.push(`${line.user}@${line.realm} ${line.password}`); return config; } };
  return api as unknown as AdminApi & { asked: string[] };
}

describe('signallingUri', () => {
  const config: ClientConfig = {
    websocket_uri: 'wss://203.0.113.5:8089',
    transports: [
      { transport: 'udp', address: '203.0.113.5', port: 5060, uri: 'sip:203.0.113.5:5060;transport=udp' },
      { transport: 'ws', address: '203.0.113.5', port: 8088, uri: 'sip:203.0.113.5:8088;transport=ws' },
    ],
    ice_servers: [],
  };

  it('takes the secure WebSocket for an https page, which may not open ws://', () => {
    expect(signallingUri(config, true)).toBe('wss://203.0.113.5:8089');
    expect(signallingUri({ ...config, websocket_uri: undefined }, true)).toBeUndefined();
  });

  it('takes the plain WebSocket for an http page, which need not trust the node certificate', () => {
    expect(signallingUri(config, false)).toBe('ws://203.0.113.5:8088');
    expect(signallingUri({ ...config, transports: [] }, false)).toBeUndefined();
  });
});

describe('describeVideoLine', () => {
  it('names a decline by its port, and an accepted line by port and direction', () => {
    expect(describeVideoLine({ port: 0, direction: 'inactive', bundled: true })).toBe('declined (port 0)');
    expect(describeVideoLine({ port: 9, direction: 'sendrecv', bundled: false })).toBe('port 9, sendrecv');
    expect(describeVideoLine({ port: 9, direction: 'sendrecv', bundled: true })).toBe('bundled, sendrecv');
    expect(describeVideoLine({ port: 51454, direction: 'sendrecv', bundled: true })).toBe('port 51454 (bundled), sendrecv');
    expect(describeVideoLine(undefined)).toBe('none');
  });
});

describe('SoftphoneScreen', () => {
  it("signals where the node says, and calls with the node's ICE servers fetched at the moment of calling, signed with the line", async () => {
    const sip = stack();
    const api = node({
      websocket_uri: 'wss://node:9443',
      transports: [
        { transport: 'ws', address: 'node', port: 8088, uri: 'sip:node:8088;transport=ws' },
        { transport: 'wss', address: 'node', port: 9443, uri: 'sips:node:9443;transport=wss' },
      ],
      ice_servers: [
        { urls: 'stun:node:3478' },
        { urls: 'turn:node:3478' },
        { urls: 'turn:node:3478', username: '4102444800', credential: 'c', expires_at: 4_102_444_800 },
      ],
    });
    render(<SoftphoneScreen api={api} stack={sip} options={{ uri: 'sip:1001@example.com', password: 'pw', target: 'sip:1002@example.com', register: false, answer: false }} />);
    const socket = screen.getByTestId('softphone-socket') as HTMLInputElement;

    fireEvent.click(screen.getByRole('button', { name: 'Register' }));
    await vi.waitFor(() => expect(sip.agents).toHaveLength(1));
    // jsdom serves the page over http, so the plain WebSocket.
    expect(socket.value).toBe('ws://node:8088');
    const [agent] = sip.agents;
    act(() => { agent.emit('registered', {}); });
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));

    await vi.waitFor(() => expect(agent.calls).toHaveLength(1));
    expect(api.asked).toEqual(['1001@example.com pw', '1001@example.com pw']);
    // The TURN entry the node could mint no credential for is not handed to the browser.
    expect(agent.calls[0].options.pcConfig).toEqual({ iceServers: [
      { urls: 'stun:node:3478' },
      { urls: 'turn:node:3478', username: '4102444800', credential: 'c' },
    ] });
    expect(screen.getByTestId('softphone-ice-servers').textContent).toBe('stun:node:3478, turn:node:3478');
  });

  it('on the harness page, calls with the ICE servers and relay policy the query string gave', () => {
    const sip = stack();
    const ice = [{ urls: 'stun:node:3478' }, { urls: 'turn:node:3478' }, { urls: 'turn:node:3478', username: '4102444800:token', credential: 'c', expires_at: 4_102_444_800 }];
    render(<SoftphoneScreen stack={sip} options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', target: 'sip:1002@example.com', register: true, answer: false, ice, relay: true }} />);
    const [agent] = sip.agents;
    act(() => { agent.emit('registered', {}); });
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));

    expect(agent.calls[0].options.pcConfig).toEqual({
      iceServers: [{ urls: 'stun:node:3478' }, { urls: 'turn:node:3478', username: '4102444800:token', credential: 'c' }],
      iceTransportPolicy: 'relay',
    });
    expect(screen.getByTestId('softphone-ice-servers').textContent).toBe('stun:node:3478, turn:node:3478, relay only');
    // The harness page has no console to ask, so it offers no relay switch of its own.
    expect(screen.queryByTestId('softphone-relay-only')).toBeNull();
  });

  it('sends the camera when Video is ticked', async () => {
    const sip = stack();
    render(<SoftphoneScreen api={node({ transports: [], ice_servers: [] })} stack={sip} options={{ socket: 'wss://node:9443', uri: 'sip:1001@example.com', target: 'sip:athenaphone@10.101.1.20', register: true, answer: false }} />);
    const [agent] = sip.agents;
    act(() => { agent.emit('registered', {}); });
    fireEvent.click(screen.getByTestId('softphone-video'));
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));

    await vi.waitFor(() => expect(agent.calls).toHaveLength(1));
    expect(agent.calls[0].options.mediaConstraints).toEqual({ audio: true, video: true });
  });

  it('forces media through TURN when asked, and says so', async () => {
    const sip = stack();
    const turn = { urls: 'turn:node:3478', username: '4102444800', credential: 'c', expires_at: 4_102_444_800 };
    render(<SoftphoneScreen api={node({ transports: [], ice_servers: [turn] })} stack={sip} options={{ socket: 'wss://node:9443', uri: 'sip:1001@example.com', target: 'sip:1002@example.com', register: true, answer: false }} />);
    const [agent] = sip.agents;
    act(() => { agent.emit('registered', {}); });
    fireEvent.click(screen.getByTestId('softphone-relay-only'));
    fireEvent.click(screen.getByRole('button', { name: 'Call' }));

    await vi.waitFor(() => expect(agent.calls).toHaveLength(1));
    expect(agent.calls[0].options.pcConfig).toEqual({ iceServers: [{ urls: 'turn:node:3478', username: '4102444800', credential: 'c' }], iceTransportPolicy: 'relay' });
    expect(screen.getByTestId('softphone-ice-servers').textContent).toBe('turn:node:3478, relay only');
  });


  it('registers with what was typed and shows the registration state', async () => {
    const sip = stack();
    render(<SoftphoneScreen stack={sip} />);

    fireEvent.change(screen.getByLabelText('WebSocket URL'), { target: { value: 'ws://node:8088' } });
    fireEvent.change(screen.getByLabelText('SIP URI'), { target: { value: 'sip:1001@example.com' } });
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'secret' } });
    fireEvent.click(screen.getByRole('button', { name: 'Register' }));

    expect(sip.agents[0].configuration.uri).toBe('sip:1001@example.com');
    expect(screen.getByTestId('softphone-registration').textContent).toBe('Registering...');

    act(() => { sip.agents[0].emit('registered', {}); });
    expect(screen.getByTestId('softphone-registration').textContent).toBe('Registered');
    expect(screen.getByTestId('softphone-call-state').getAttribute('data-state')).toBe('idle');
  });

  it('reports a refused registration where a person will see it', () => {
    const sip = stack();
    render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', password: 'wrong', register: true, answer: false }} stack={sip} />);
    act(() => { sip.agents[0].emit('registrationFailed', { cause: 'Unauthorized' }); });
    expect(screen.getByRole('alert').textContent).toBe('Registration failed: Unauthorized');
    expect(screen.getByTestId('softphone-registration').getAttribute('data-state')).toBe('failed');
  });

  it('says plainly that no call can start on a page that is not a secure context', () => {
    Object.defineProperty(window, 'isSecureContext', { value: false, configurable: true });
    try {
      render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', password: 'p', register: false, answer: false }} stack={stack()} />);
      expect(screen.getByTestId('softphone-insecure').textContent).toContain('not a secure context');
      expect(screen.getByTestId('softphone-register')).toHaveProperty('disabled', true);
    } finally {
      delete (window as { isSecureContext?: boolean }).isSecureContext;
    }
  });

  it('registers on load when the page asks for it, prefilled from the query string', () => {
    const sip = stack();
    render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', password: 'pw', target: 'sip:1002@example.com', register: true, answer: false }} stack={sip} />);
    expect(sip.agents).toHaveLength(1);
    expect(sip.agents[0].configuration.password).toBe('pw');
    expect((screen.getByLabelText('Call target') as HTMLInputElement).value).toBe('sip:1002@example.com');
  });

  it('places a call, shows the negotiation, and hangs up', () => {
    const sip = stack();
    render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', password: 'pw', target: 'sip:1002@example.com', register: true, answer: false }} stack={sip} />);
    const [agent] = sip.agents;
    act(() => { agent.emit('registered', {}); });

    fireEvent.click(screen.getByRole('button', { name: 'Call' }));
    const [session] = agent.sessions;
    expect(screen.getByTestId('softphone-call-state').textContent).toBe('Calling... (sip:1002@example.com)');
    expect(screen.getByRole('button', { name: 'Call' })).toHaveProperty('disabled', true);

    act(() => {
      session.emit('sdp', { originator: 'local', type: 'offer', sdp: 'v=0 offer' });
      session.emit('sdp', { originator: 'remote', type: 'answer', sdp: 'v=0 answer' });
      session.emit('accepted', {});
    });
    expect(screen.getByTestId('softphone-call-state').getAttribute('data-state')).toBe('connected');
    expect(screen.getByTestId('softphone-local-sdp').textContent).toBe('v=0 offer');
    expect(screen.getByTestId('softphone-remote-sdp').textContent).toBe('v=0 answer');

    fireEvent.click(screen.getByRole('button', { name: 'Hang up' }));
    expect(session.terminated).toBe(1);
    act(() => { session.emit('ended', { originator: 'local', cause: 'Terminated' }); });
    expect(screen.getByTestId('softphone-call-state').textContent).toBe('Call ended (sip:1002@example.com)');
  });

  it('answers on arrival when the page asks for it, and offers the button otherwise', () => {
    const sip = stack();
    const { unmount } = render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', register: true, answer: false }} stack={sip} />);
    act(() => { sip.agents[0].emit('registered', {}); });
    const session = new FakeSession();
    act(() => { sip.agents[0].emit('newRTCSession', { originator: 'remote', session, request: {} }); });
    expect(session.answered).toBe(0);
    fireEvent.click(screen.getByRole('button', { name: 'Answer' }));
    expect(session.answered).toBe(1);
    unmount();

    render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', register: true, answer: true }} stack={sip} />);
    act(() => { sip.agents[1].emit('registered', {}); });
    const auto = new FakeSession();
    act(() => { sip.agents[1].emit('newRTCSession', { originator: 'remote', session: auto, request: {} }); });
    expect(auto.answered).toBe(1);
  });

  it('exposes the readout while mounted and removes it on unmount, stopping the agent', () => {
    const sip = stack();
    const { unmount } = render(<SoftphoneScreen options={{ socket: 'ws://node:8088', uri: 'sip:1001@example.com', register: true, answer: false }} stack={sip} />);
    expect(window[READOUT_KEY]?.state().registration).toBe('connecting');
    unmount();
    expect(window[READOUT_KEY]).toBeUndefined();
    expect(sip.agents[0].stopped).toBe(1);
  });
});

describe('the pure parts', () => {
  it('names every call state', () => {
    expect(describeCallState('ringing')).toBe('Ringing...');
    expect(callInProgress('incoming')).toBe(true);
    expect(callInProgress('ended')).toBe(false);
  });

  it('defaults the socket to the page host when nothing says otherwise', () => {
    const connection = defaultConnection({ register: false, answer: false });
    expect(connection.socket).toMatch(/^ws:\/\/.*:8088$/);
    expect(connection.password).toBe('');
  });
});
