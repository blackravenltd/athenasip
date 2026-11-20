import { EventEmitter } from 'node:events';
import { describe, expect, it, vi } from 'vitest';
import type { UA, UAConfiguration } from 'jssip/lib/UA';
import { Softphone, summarise, videoLine, type SipStack } from './Softphone';

/** A user agent that never opens a socket. Tests fire JsSIP's events in JsSIP's order. */
class FakeAgent extends EventEmitter {
  started = 0;
  stopped = 0;
  calls: string[] = [];
  callOptions: Array<Record<string, unknown>> = [];
  sessions: FakeSession[] = [];
  constructor(readonly configuration: UAConfiguration) { super(); }
  start() { this.started += 1; }
  stop() { this.stopped += 1; }
  /** The next outgoing session arrives with this connection already made, as JsSIP does. */
  nextConnection?: FakePeer;
  call(target: string, options: Record<string, unknown> = {}) {
    this.calls.push(target);
    this.callOptions.push(options);
    const session = new FakeSession();
    session.connection = this.nextConnection;
    this.sessions.push(session);
    this.emit('newRTCSession', { originator: 'local', session, request: {} });
    return session;
  }
}

class FakeSession extends EventEmitter {
  answered = 0;
  answerOptions: Array<Record<string, unknown>> = [];
  terminated: unknown[] = [];
  remote_identity = { uri: { toString: () => 'sip:1002@example.com' } };
  connection?: FakePeer;
  answer(options: Record<string, unknown> = {}) { this.answered += 1; this.answerOptions.push(options); }
  log: string[] = [];
  mute(options: unknown) { this.log.push(`mute ${JSON.stringify(options)}`); }
  unmute(options: unknown) { this.log.push(`unmute ${JSON.stringify(options)}`); }
  hold() { this.log.push('hold'); return true; }
  unhold() { this.log.push('unhold'); return true; }
  sendDTMF(tone: string, options: { transportType?: string }) { this.log.push(`dtmf ${tone} ${options.transportType}`); }
  terminate(options?: unknown) { this.terminated.push(options ?? null); }
}

class FakePeer extends EventTarget {
  signalingState = 'stable';
  iceGatheringState = 'new';
  iceConnectionState = 'new';
  connectionState = 'new';
  report = new Map<string, Record<string, unknown>>();
  getStats() { return Promise.resolve(this.report as unknown as RTCStatsReport); }
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

const CONFIG = { socket: 'ws://node:8088', uri: 'sip:1001@example.com', password: 'secret' };

describe('Softphone', () => {
  it('starts unregistered and idle, with that as its first recorded state', () => {
    const phone = new Softphone(stack(), () => 1000);
    expect(phone.state).toEqual({ registration: 'unregistered', call: 'idle' });
    expect(phone.history).toEqual([{ at: 1000, registration: 'unregistered', call: 'idle' }]);
  });

  it('registers with what it was given and reports the outcome', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);

    const [agent] = sip.agents;
    expect(agent.configuration.uri).toBe(CONFIG.uri);
    expect(agent.configuration.password).toBe(CONFIG.password);
    expect(agent.started).toBe(1);
    expect(phone.state.registration).toBe('connecting');

    agent.emit('registered', {});
    expect(phone.state.registration).toBe('registered');
  });

  it('says why a registration failed, in the words JsSIP used', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    sip.agents[0].emit('registrationFailed', { cause: 'Unauthorized' });
    expect(phone.state.registration).toBe('failed');
    expect(phone.state.notice).toBe('Registration failed: Unauthorized');
  });

  it('on a page that is not a secure context, fails a call at once rather than wait for a microphone', () => {
    vi.stubGlobal('isSecureContext', false);
    try {
      const sip = stack();
      const phone = new Softphone(sip);
      phone.register(CONFIG);
      const [agent] = sip.agents;
      agent.emit('registered', {});
      phone.call('sip:1002@example.com');
      expect(agent.calls).toEqual([]);
      expect(phone.state.call).toBe('failed');
      expect(phone.state.notice).toMatch(/^Could not call: This page is not a secure context/);

      const session = new FakeSession();
      agent.emit('newRTCSession', { originator: 'remote', session, request: {} });
      phone.answer();
      expect(session.answered).toBe(0);
      expect(session.terminated).toEqual([{ status_code: 488 }]);
      expect(phone.state.notice).toMatch(/^Could not answer: This page is not a secure context/);
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it('treats a dropped socket as the end of the registration', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    sip.agents[0].emit('registered', {});
    sip.agents[0].emit('disconnected', { error: true });
    expect(phone.state.registration).toBe('unregistered');
  });

  it('follows an outgoing call from dialling to hanging up', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});

    phone.call('sip:1002@example.com');
    expect(agent.calls).toEqual(['sip:1002@example.com']);
    expect(phone.state.call).toBe('calling');
    expect(phone.state.direction).toBe('outgoing');
    expect(phone.state.remoteIdentity).toBe('sip:1002@example.com');

    const session = lastSession(agent);
    session.emit('sdp', { originator: 'local', type: 'offer', sdp: 'v=0 local' });
    session.emit('progress', {});
    expect(phone.state.call).toBe('ringing');
    session.emit('sdp', { originator: 'remote', type: 'answer', sdp: 'v=0 remote' });
    session.emit('accepted', {});
    expect(phone.state.call).toBe('connected');
    expect(phone.state.localSdp).toBe('v=0 local');
    expect(phone.state.remoteSdp).toBe('v=0 remote');

    phone.hangUp();
    expect(session.terminated).toHaveLength(1);
    session.emit('ended', { originator: 'local', cause: 'Terminated' });
    expect(phone.state.call).toBe('ended');
    expect(phone.state.cause).toBe('Terminated');
  });

  it('answers an incoming call only while it is incoming', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});

    phone.answer();
    const session = new FakeSession();
    agent.emit('newRTCSession', { originator: 'remote', session, request: {} });
    expect(phone.state.call).toBe('incoming');
    expect(phone.state.direction).toBe('incoming');

    // A 180 going out is not the far end ringing.
    session.emit('progress', {});
    expect(phone.state.call).toBe('incoming');

    phone.answer();
    expect(session.answered).toBe(1);
    session.emit('accepted', {});
    expect(phone.state.call).toBe('connected');
  });

  it('asks for the camera only when told to, calling or answering', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});

    phone.call('sip:1002@example.com');
    expect(agent.callOptions[0]).toMatchObject({ mediaConstraints: { audio: true, video: false }, rtcOfferConstraints: { offerToReceiveVideo: false } });
    lastSession(agent).emit('ended', { originator: 'local', cause: 'Terminated' });

    phone.call('sip:1002@example.com', { servers: [] }, { video: true });
    expect(agent.callOptions[1]).toMatchObject({ mediaConstraints: { audio: true, video: true }, rtcOfferConstraints: { offerToReceiveVideo: true } });
    lastSession(agent).emit('ended', { originator: 'local', cause: 'Terminated' });

    const session = new FakeSession();
    agent.emit('newRTCSession', { originator: 'remote', session, request: {} });
    phone.answer({ servers: [] }, { video: true });
    expect(session.answerOptions[0]).toMatchObject({ mediaConstraints: { audio: true, video: true } });
  });

  it('chooses the devices it was told to, by id', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    phone.call('sip:1002@example.com', { servers: [] }, { video: true, microphone: 'mic-2', camera: 'cam-1' });
    expect(agent.callOptions[0]).toMatchObject({ mediaConstraints: { audio: { deviceId: { exact: 'mic-2' } }, video: { deviceId: { exact: 'cam-1' } } } });
  });

  it('mutes, holds and sends dial pad keys only on a connected call', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    phone.call('sip:1002@example.com');
    const session = lastSession(agent);

    phone.mute(true);
    phone.sendDtmf('1');
    expect(session.log).toEqual([]);

    session.emit('accepted', {});
    phone.mute(true);
    expect(phone.state.muted).toBe(true);
    phone.mute(false);
    phone.hold(true);
    expect(phone.state.held).toBe(true);
    phone.sendDtmf('#');
    phone.sendDtmf('b');
    phone.sendDtmf('X');
    phone.sendDtmf('12');
    expect(session.log).toEqual(['mute {"audio":true}', 'unmute {"audio":true}', 'hold', 'dtmf # RFC2833', 'dtmf B RFC2833']);

    session.emit('hold', { originator: 'remote' });
    expect(phone.state.heldByFarEnd).toBe(true);
    session.emit('unhold', { originator: 'remote' });
    expect(phone.state.heldByFarEnd).toBe(false);
  });

  it('declines a ringing call with 603, and only a ringing one', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    const session = new FakeSession();
    agent.emit('newRTCSession', { originator: 'remote', session, request: {} });
    phone.decline();
    expect(session.terminated).toEqual([{ status_code: 603, reason_phrase: 'Decline' }]);
  });

  it('refuses a second call with 486 rather than replacing the first', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});

    const first = new FakeSession();
    agent.emit('newRTCSession', { originator: 'remote', session: first, request: {} });
    const second = new FakeSession();
    agent.emit('newRTCSession', { originator: 'remote', session: second, request: {} });

    expect(second.terminated).toEqual([{ status_code: 486 }]);
    expect(first.terminated).toHaveLength(0);
    expect(phone.state.remoteIdentity).toBe('sip:1002@example.com');
  });

  it('reports a failed call with its cause', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    phone.call('sip:1002@example.com');
    lastSession(agent).emit('failed', { originator: 'remote', cause: 'Busy' });
    expect(phone.state.call).toBe('failed');
    expect(phone.state.notice).toBe('Call failed: Busy');
  });

  it('watches the peer connection and records each state it passes through', async () => {
    const sip = stack();
    const clock = { now: 0 };
    const phone = new Softphone(sip, () => clock.now);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    phone.call('sip:1002@example.com');
    const session = lastSession(agent);
    const peer = new FakePeer();
    session.emit('peerconnection', { peerconnection: peer });
    expect(phone.state.iceConnectionState).toBe('new');

    clock.now = 5;
    peer.iceConnectionState = 'checking';
    peer.dispatchEvent(new Event('iceconnectionstatechange'));
    clock.now = 9;
    peer.iceConnectionState = 'connected';
    peer.connectionState = 'connected';
    peer.dispatchEvent(new Event('connectionstatechange'));

    expect(phone.state.iceConnectionState).toBe('connected');
    expect(phone.history.map((row) => [row.at, row.registration, row.call, row.iceConnectionState])).toEqual([
      [0, 'unregistered', 'idle', undefined],
      [0, 'connecting', 'idle', undefined],
      [0, 'registered', 'idle', undefined],
      [0, 'registered', 'calling', undefined],
      [0, 'registered', 'calling', 'new'],
      [5, 'registered', 'calling', 'checking'],
      [9, 'registered', 'calling', 'connected'],
    ]);

    peer.report.set('t', { type: 'transport', dtlsState: 'connected' });
    expect((await phone.stats())?.dtlsState).toBe('connected');
  });

  it('watches an outgoing call\'s connection, which JsSIP makes before it announces the session', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    const peer = new FakePeer();
    agent.nextConnection = peer;
    phone.call('sip:1002@example.com');

    expect(phone.state.iceConnectionState).toBe('new');
    peer.iceConnectionState = 'connected';
    peer.dispatchEvent(new Event('iceconnectionstatechange'));
    expect(phone.state.iceConnectionState).toBe('connected');
  });

  it('resolves waitFor on the first matching state and times out honestly', async () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const waiting = phone.waitFor((state) => state.registration === 'registered');
    sip.agents[0].emit('registered', {});
    await expect(waiting).resolves.toMatchObject({ registration: 'registered' });

    await expect(phone.waitFor((state) => state.call === 'connected', 5))
      .rejects.toThrow('registration registered, call idle');
  });

  it('stops the agent and the call when unregistered or disposed', () => {
    const sip = stack();
    const phone = new Softphone(sip);
    phone.register(CONFIG);
    const [agent] = sip.agents;
    agent.emit('registered', {});
    phone.call('sip:1002@example.com');
    const session = lastSession(agent);

    phone.unregister();
    expect(session.terminated).toHaveLength(1);
    expect(agent.stopped).toBe(1);
    expect(phone.state).toEqual({ registration: 'unregistered', call: 'idle' });

    phone.register(CONFIG);
    phone.dispose();
    expect(sip.agents[1].stopped).toBe(1);
  });
});

describe('videoLine', () => {
  const sdp = (video: string) => ['v=0', 'o=- 1 1 IN IP4 10.0.0.5', 's=-', 't=0 0', 'm=audio 9 UDP/TLS/RTP/SAVPF 111', 'a=sendrecv', video].join('\r\n');

  it('reads the video m-line port and direction, and nothing when there is none', () => {
    expect(videoLine(sdp('m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=recvonly'))).toEqual({ port: 9, direction: 'recvonly', bundled: false });
    expect(videoLine(sdp('m=video 51000 UDP/TLS/RTP/SAVPF 96'))).toEqual({ port: 51000, direction: 'sendrecv', bundled: false });
    expect(videoLine(sdp('m=video 0 UDP/TLS/RTP/SAVPF 96\r\na=inactive'))).toEqual({ port: 0, direction: 'inactive', bundled: false });
    expect(videoLine(sdp(''))).toBeUndefined();
    expect(videoLine(undefined)).toBeUndefined();
  });

  it('knows a video line in the BUNDLE group', () => {
    const bundled = ['v=0', 'a=group:BUNDLE 0 1', 'm=audio 9 UDP/TLS/RTP/SAVPF 111', 'a=mid:0', 'm=video 9 UDP/TLS/RTP/SAVPF 96', 'a=mid:1'].join('\r\n');
    expect(videoLine(bundled)).toEqual({ port: 9, direction: 'sendrecv', bundled: true });
    expect(videoLine(bundled.replace('a=group:BUNDLE 0 1', 'a=group:BUNDLE 0'))?.bundled).toBe(false);
  });

  it('does not take the audio section direction for the video one', () => {
    expect(videoLine(sdp('m=video 9 UDP/TLS/RTP/SAVPF 96'))?.direction).toBe('sendrecv');
    expect(videoLine(['v=0', 'm=audio 9 RTP/AVP 0', 'a=recvonly', 'm=video 9 RTP/AVP 96'].join('\n'))?.direction).toBe('sendrecv');
  });
});

describe('summarise', () => {
  it('keeps video counters apart from the audio ones', () => {
    const report = new Map<string, Record<string, unknown>>([
      ['ca', { type: 'codec', mimeType: 'audio/opus' }],
      ['cv', { type: 'codec', mimeType: 'video/VP8' }],
      ['ia', { type: 'inbound-rtp', kind: 'audio', packetsReceived: 100, bytesReceived: 8000, packetsLost: 0, codecId: 'ca' }],
      ['iv', { type: 'inbound-rtp', kind: 'video', packetsReceived: 900, bytesReceived: 900000, packetsLost: 3, framesDecoded: 240, codecId: 'cv' }],
      ['oa', { type: 'outbound-rtp', kind: 'audio', packetsSent: 110, bytesSent: 8800 }],
      ['ov', { type: 'outbound-rtp', kind: 'video', packetsSent: 950, bytesSent: 950000 }],
      ['ma', { type: 'media-source', kind: 'audio', totalAudioEnergy: 0.5 }],
      ['mv', { type: 'media-source', kind: 'video', frames: 300 }],
    ]);
    expect(summarise(report as unknown as RTCStatsReport)).toEqual({
      packetsSent: 110,
      packetsReceived: 100,
      bytesSent: 8800,
      bytesReceived: 8000,
      packetsLost: 0,
      codec: 'audio/opus',
      sentAudioEnergy: 0.5,
      video: { packetsSent: 950, packetsReceived: 900, framesDecoded: 240, codec: 'video/VP8' },
    });
  });

  it('resolves the selected candidate pair, the codec and the counters from a stats report', () => {
    const report = new Map<string, Record<string, unknown>>([
      ['t', { type: 'transport', dtlsState: 'connected', selectedCandidatePairId: 'p' }],
      ['p', { type: 'candidate-pair', state: 'succeeded', nominated: true, localCandidateId: 'l', remoteCandidateId: 'r' }],
      ['l', { type: 'local-candidate', address: '10.0.0.5', port: 51000, candidateType: 'host', protocol: 'udp' }],
      ['r', { type: 'remote-candidate', address: '10.101.1.132', port: 23000, candidateType: 'host', protocol: 'udp' }],
      ['c', { type: 'codec', mimeType: 'audio/opus' }],
      ['in', { type: 'inbound-rtp', kind: 'audio', packetsReceived: 120, bytesReceived: 9600, packetsLost: 2, audioLevel: 0.4, totalAudioEnergy: 0.25, codecId: 'c' }],
      ['out', { type: 'outbound-rtp', kind: 'audio', packetsSent: 130, bytesSent: 10400 }],
    ]);
    expect(summarise(report as unknown as RTCStatsReport)).toEqual({
      packetsSent: 130,
      packetsReceived: 120,
      bytesSent: 10400,
      bytesReceived: 9600,
      packetsLost: 2,
      audioLevel: 0.4,
      totalAudioEnergy: 0.25,
      codec: 'audio/opus',
      dtlsState: 'connected',
      candidatePair: {
        local: { address: '10.0.0.5', port: 51000, type: 'host', protocol: 'udp' },
        remote: { address: '10.101.1.132', port: 23000, type: 'host', protocol: 'udp' },
        state: 'succeeded',
      },
    });
  });

  it('falls back to the nominated pair when there is no transport record, as Firefox reports', () => {
    const report = new Map<string, Record<string, unknown>>([
      ['p', { type: 'candidate-pair', state: 'succeeded', nominated: true, localCandidateId: 'l', remoteCandidateId: 'r' }],
      ['q', { type: 'candidate-pair', state: 'failed', nominated: false, localCandidateId: 'l', remoteCandidateId: 'r' }],
      ['l', { type: 'local-candidate', address: '10.0.0.5', port: 51000, candidateType: 'host', protocol: 'udp' }],
      ['r', { type: 'remote-candidate', ip: '10.101.1.132', port: 23000, candidateType: 'host', protocol: 'udp' }],
    ]);
    const stats = summarise(report as unknown as RTCStatsReport);
    expect(stats.candidatePair?.remote.address).toBe('10.101.1.132');
    expect(stats.packetsReceived).toBe(0);
    expect(stats.totalAudioEnergy).toBeUndefined();
  });
});

function lastSession(agent: FakeAgent): FakeSession {
  const session = agent.sessions[agent.sessions.length - 1];
  if (!session) throw new Error('No session was created');
  return session;
}
