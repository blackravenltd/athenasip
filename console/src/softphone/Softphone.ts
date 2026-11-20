/**
 * A WebRTC endpoint over SIP, free of React: registers one subscriber over a
 * WebSocket, places or answers one call, and publishes an immutable snapshot
 * of its state after every change.
 *
 * The snapshot carries the session descriptions and the ICE and DTLS state, so
 * a call that connects without media can be diagnosed. JsSIP is injected (see
 * `SipStack`), so tests open no socket and only the softphone bundle loads it.
 */
import type { DTMF_TRANSPORT } from 'jssip/lib/Constants';
import type { RTCSession, EndEvent, PeerConnectionEvent, SDPEvent } from 'jssip/lib/RTCSession';
import type { UA, UAConfiguration, RTCSessionEvent, UnRegisteredEvent } from 'jssip/lib/UA';

export type RegistrationState = 'unregistered' | 'connecting' | 'registered' | 'failed';
export type CallState = 'idle' | 'calling' | 'ringing' | 'incoming' | 'connected' | 'ended' | 'failed';
export type CallDirection = 'outgoing' | 'incoming';

/** One end of the candidate pair ICE selected. */
export interface CandidateEnd {
  address: string;
  port: number;
  type: string;
  protocol: string;
}

/** A summary of one `getStats()` report. */
export interface MediaStats {
  packetsSent: number;
  packetsReceived: number;
  bytesSent: number;
  bytesReceived: number;
  packetsLost: number;
  /** The far end's level, 0 to 1, when the browser reports one. */
  audioLevel?: number;
  /**
   * The far end's accumulated energy. Unlike `audioLevel`, it cannot miss
   * sound by sampling a gap.
   */
  totalAudioEnergy?: number;
  /**
   * This end's accumulated microphone energy. Standing still on a connected
   * call means a muted, missing or silent microphone.
   */
  sentAudioEnergy?: number;
  codec?: string;
  /** Video counters, kept apart from the audio figures above. */
  video?: VideoStats;
  dtlsState?: string;
  candidatePair?: { local: CandidateEnd; remote: CandidateEnd; state: string };
}

/** Present when a call carries video. */
export interface VideoStats {
  packetsSent: number;
  packetsReceived: number;
  framesDecoded: number;
  codec?: string;
}

/** A description's video m-line. Port 0 means declined. */
export interface VideoLine {
  port: number;
  direction: 'sendrecv' | 'sendonly' | 'recvonly' | 'inactive';
  /** In the BUNDLE group: it shares a transport, so its port may be a placeholder. */
  bundled: boolean;
}

export interface SoftphoneState {
  registration: RegistrationState;
  call: CallState;
  direction?: CallDirection;
  /** The far end's identity, as the request named it. */
  remoteIdentity?: string;
  /** Why the call or the registration ended, in JsSIP's words. */
  cause?: string;
  localSdp?: string;
  remoteSdp?: string;
  signalingState?: RTCSignalingState;
  iceGatheringState?: RTCIceGatheringState;
  iceConnectionState?: RTCIceConnectionState;
  connectionState?: RTCPeerConnectionState;
  /** When the call was answered, by the controller's clock. */
  connectedAt?: number;
  muted?: boolean;
  /** This end put the call on hold. */
  held?: boolean;
  /** The far end put the call on hold. */
  heldByFarEnd?: boolean;
  /** A message for the person at the keyboard. */
  notice?: string;
}

/** What a call sends. Devices are `deviceId`s; omitted means the default. */
export interface CallMedia {
  video?: boolean;
  microphone?: string;
  camera?: string;
}

/** The dial pad keys: RFC 4733 events 0 to 15. */
export const DTMF_TONES = '0123456789*#ABCD';

export interface SoftphoneConfig {
  socket: string;
  uri: string;
  password: string;
}

/** One entry in the state history. */
export interface Transition {
  at: number;
  registration: RegistrationState;
  call: CallState;
  iceConnectionState?: RTCIceConnectionState;
  connectionState?: RTCPeerConnectionState;
  cause?: string;
}

/** The part of JsSIP the controller uses; tests supply a fake. */
export interface SipStack {
  createUserAgent(configuration: UAConfiguration): UA;
  createSocket(url: string): UAConfiguration['sockets'];
}

export type Listener = (state: SoftphoneState) => void;

const INITIAL: SoftphoneState = { registration: 'unregistered', call: 'idle' };

/** With no ICE servers, the media engine's advertised address is the only remote candidate. */
const peerConfiguration = (iceServers: RTCIceServer[], relayOnly: boolean): RTCConfiguration =>
  (relayOnly ? { iceServers, iceTransportPolicy: 'relay' } : { iceServers });

/** What ICE may use for one call. `relayOnly` forces media through TURN. */
export interface IceOptions {
  servers: RTCIceServer[];
  relayOnly?: boolean;
}

const NO_ICE: IceOptions = { servers: [] };

/**
 * Why no call can start on this page, if it is not a secure context: a
 * browser gives such a page no microphone, and JsSIP then waits for one for
 * ever. Undefined where the runtime does not say, as in a test.
 */
const INSECURE = 'This page is not a secure context, so the browser gives it no microphone. Open it over https, or through localhost.';
function insecure(): boolean {
  return (globalThis as { isSecureContext?: boolean }).isSecureContext === false;
}

function constraints({ video, microphone, camera }: CallMedia): MediaStreamConstraints {
  return {
    audio: microphone ? { deviceId: { exact: microphone } } : true,
    video: video ? (camera ? { deviceId: { exact: camera } } : true) : false,
  };
}

export class Softphone {
  private current: SoftphoneState = INITIAL;
  private readonly listeners = new Set<Listener>();
  private readonly transitions: Transition[] = [];
  private agent?: UA;
  private session?: RTCSession;
  private peer?: RTCPeerConnection;
  private remote?: MediaStream;
  private local?: MediaStream;
  private ice: IceOptions = NO_ICE;

  constructor(private readonly sip: SipStack, private readonly clock: () => number = Date.now) {
    this.record();
  }

  get state(): SoftphoneState {
    return this.current;
  }

  /** The far end's audio and video, once any arrives. */
  get remoteStream(): MediaStream | undefined {
    return this.remote;
  }

  /** The tracks this end sends, once the local description is set. */
  get localStream(): MediaStream | undefined {
    return this.local;
  }

  /** Every state change since construction, oldest first. */
  get history(): readonly Transition[] {
    return this.transitions;
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    return () => { this.listeners.delete(listener); };
  }

  /** Resolves with the first state that satisfies `predicate`, or rejects after `timeoutMs`. */
  waitFor(predicate: (state: SoftphoneState) => boolean, timeoutMs = 10_000): Promise<SoftphoneState> {
    if (predicate(this.current)) return Promise.resolve(this.current);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        unsubscribe();
        reject(new Error(`Timed out after ${timeoutMs}ms waiting for the softphone; it is ${describe(this.current)}`));
      }, timeoutMs);
      const unsubscribe = this.subscribe((state) => {
        if (!predicate(state)) return;
        clearTimeout(timer);
        unsubscribe();
        resolve(state);
      });
    });
  }

  register(config: SoftphoneConfig): void {
    this.stopAgent();
    this.reset({ registration: 'connecting' });
    try {
      const agent = this.sip.createUserAgent({
        sockets: this.sip.createSocket(config.socket),
        uri: config.uri,
        password: config.password,
      });
      agent.on('registered', () => this.update({ registration: 'registered', notice: undefined }));
      agent.on('unregistered', () => this.update({ registration: 'unregistered' }));
      agent.on('registrationFailed', (event: UnRegisteredEvent) => {
        const cause = event.cause ?? 'no reason given';
        this.update({ registration: 'failed', cause, notice: `Registration failed: ${cause}` });
      });
      agent.on('disconnected', () => {
        // A dropped socket ends the registration, whatever the registrar still holds.
        if (this.current.registration !== 'failed') this.update({ registration: 'unregistered' });
      });
      agent.on('newRTCSession', (event: RTCSessionEvent) => this.adopt(event));
      agent.start();
      this.agent = agent;
    } catch (cause) {
      this.update({ registration: 'failed', notice: `Could not start: ${String(cause)}` });
    }
  }

  unregister(): void {
    this.stopAgent();
    this.reset({ registration: 'unregistered' });
  }

  /** The default ICE options for calls and answers that pass none. */
  useIce(ice: IceOptions): void {
    this.ice = ice;
  }

  /** Audio only unless `media.video` is set. */
  call(target: string, ice: IceOptions = this.ice, media: CallMedia = {}): void {
    if (!this.agent || this.session) return;
    if (insecure()) {
      this.update({ call: 'failed', notice: `Could not call: ${INSECURE}` });
      return;
    }
    this.update({ notice: undefined, cause: undefined });
    try {
      this.agent.call(target, {
        mediaConstraints: constraints(media),
        pcConfig: peerConfiguration(ice.servers, ice.relayOnly ?? false),
        rtcOfferConstraints: { offerToReceiveAudio: true, offerToReceiveVideo: media.video ?? false },
      });
    } catch (cause) {
      this.update({ call: 'failed', notice: `Could not call: ${String(cause)}` });
    }
  }

  /** Without `media.video`, an offered video line is answered receive-only. */
  answer(ice: IceOptions = this.ice, media: CallMedia = {}): void {
    if (!this.session || this.current.call !== 'incoming') return;
    if (insecure()) {
      this.session.terminate({ status_code: 488 });
      this.update({ notice: `Could not answer: ${INSECURE}` });
      return;
    }
    this.session.answer({ mediaConstraints: constraints(media), pcConfig: peerConfiguration(ice.servers, ice.relayOnly ?? false) });
  }

  /** Stops or restarts sending the microphone, without renegotiating. */
  mute(on: boolean): void {
    if (!this.session || this.current.call !== 'connected') return;
    if (on) this.session.mute({ audio: true });
    else this.session.unmute({ audio: true });
    this.update({ muted: on });
  }

  /** Holds or resumes the call with a re-INVITE. */
  hold(on: boolean): void {
    if (!this.session || this.current.call !== 'connected') return;
    const accepted = on ? this.session.hold() : this.session.unhold();
    if (accepted) this.update({ held: on });
  }

  /** Sends one dial pad key in the media as an RFC 4733 telephone event, not as SIP INFO. */
  sendDtmf(tone: string): void {
    if (!this.session || this.current.call !== 'connected') return;
    if (tone.length !== 1 || !DTMF_TONES.includes(tone.toUpperCase())) return;
    this.session.sendDTMF(tone.toUpperCase(), { transportType: 'RFC2833' as DTMF_TRANSPORT });
  }

  hangUp(): void {
    this.session?.terminate();
  }

  /** Refuses a ringing incoming call with 603 Decline. */
  decline(): void {
    if (!this.session || this.current.call !== 'incoming') return;
    this.session.terminate({ status_code: 603, reason_phrase: 'Decline' });
  }

  /** The browser's media statistics, or nothing when there is no connection. */
  async stats(): Promise<MediaStats | undefined> {
    if (!this.peer) return undefined;
    return summarise(await this.peer.getStats());
  }

  /** Stops everything. The instance is not reusable afterwards. */
  dispose(): void {
    this.stopAgent();
    this.listeners.clear();
  }

  private adopt({ session, originator }: RTCSessionEvent): void {
    // One call at a time: a second is refused with 486.
    if (this.session && this.session !== session) {
      session.terminate({ status_code: 486 });
      return;
    }
    this.session = session;
    const incoming = originator === 'remote';
    const remoteIdentity = session.remote_identity?.uri?.toString();
    this.update({
      call: incoming ? 'incoming' : 'calling',
      direction: incoming ? 'incoming' : 'outgoing',
      remoteIdentity,
      cause: undefined,
      localSdp: undefined,
      remoteSdp: undefined,
      signalingState: undefined,
      iceGatheringState: undefined,
      iceConnectionState: undefined,
      connectionState: undefined,
      muted: false,
      held: false,
      heldByFarEnd: false,
      connectedAt: undefined,
    });

    // JsSIP announces an outgoing call's connection before the session, so
    // its 'peerconnection' event has already fired: take the connection
    // directly. The event covers an incoming call, whose connection is
    // created on answer.
    if (session.connection) this.observe(session.connection);
    session.on('peerconnection', (event: PeerConnectionEvent) => this.observe(event.peerconnection));
    session.on('sdp', (event: SDPEvent) => {
      // The tracks are on the connection once the local description is set.
      if (event.originator === 'local') this.local = senderStream(this.peer ?? session.connection);
      this.update(event.originator === 'local' ? { localSdp: event.sdp } : { remoteSdp: event.sdp });
    });
    session.on('progress', () => {
      if (this.current.direction === 'outgoing') this.update({ call: 'ringing' });
    });
    session.on('hold', (event: { originator: string }) => this.update(event.originator === 'remote' ? { heldByFarEnd: true } : { held: true }));
    session.on('unhold', (event: { originator: string }) => this.update(event.originator === 'remote' ? { heldByFarEnd: false } : { held: false }));
    const connected = () => this.update({ call: 'connected', connectedAt: this.current.connectedAt ?? this.clock() });
    session.on('accepted', connected);
    session.on('confirmed', connected);
    session.on('ended', (event: EndEvent) => this.finish('ended', event.cause));
    session.on('failed', (event: EndEvent) => this.finish('failed', event.cause));
  }

  private observe(peer: RTCPeerConnection): void {
    if (this.peer === peer) return;
    this.peer = peer;
    const snapshot = () => this.update({
      signalingState: peer.signalingState,
      iceGatheringState: peer.iceGatheringState,
      iceConnectionState: peer.iceConnectionState,
      connectionState: peer.connectionState,
    });
    peer.addEventListener('signalingstatechange', snapshot);
    peer.addEventListener('icegatheringstatechange', snapshot);
    peer.addEventListener('iceconnectionstatechange', snapshot);
    peer.addEventListener('connectionstatechange', snapshot);
    peer.addEventListener('track', (event: RTCTrackEvent) => {
      // Audio and video arrive as two events. With no stream named, the second
      // track joins the first.
      if (event.streams[0]) this.remote = event.streams[0];
      else if (this.remote) this.remote.addTrack(event.track);
      else this.remote = new MediaStream([event.track]);
      // The stream is not in the snapshot, so re-emit for a screen to attach it.
      this.emit();
    });
    snapshot();
  }

  private finish(call: 'ended' | 'failed', cause: string | undefined): void {
    this.session = undefined;
    this.peer = undefined;
    this.remote = undefined;
    this.local = undefined;
    const notice = call === 'failed' && cause ? `Call failed: ${cause}` : undefined;
    this.update({ call, cause, notice });
  }

  private stopAgent(): void {
    this.session?.terminate();
    this.session = undefined;
    this.peer = undefined;
    this.remote = undefined;
    this.local = undefined;
    this.agent?.stop();
    this.agent = undefined;
  }

  private reset(patch: Partial<SoftphoneState>): void {
    this.current = { ...INITIAL, ...patch };
    this.record();
    this.emit();
  }

  private update(patch: Partial<SoftphoneState>): void {
    this.current = { ...this.current, ...patch };
    this.record();
    this.emit();
  }

  private record(): void {
    const last = this.transitions[this.transitions.length - 1];
    const next: Transition = {
      at: this.clock(),
      registration: this.current.registration,
      call: this.current.call,
      iceConnectionState: this.current.iceConnectionState,
      connectionState: this.current.connectionState,
      cause: this.current.cause,
    };
    // Record only a change of state, not an SDP arriving.
    if (last && last.registration === next.registration && last.call === next.call
      && last.iceConnectionState === next.iceConnectionState && last.connectionState === next.connectionState
      && last.cause === next.cause) return;
    this.transitions.push(next);
  }

  private emit(): void {
    for (const listener of this.listeners) listener(this.current);
  }
}

/** A one-line description of a state, for an error message. */
export function describe(state: SoftphoneState): string {
  const parts = [`registration ${state.registration}`, `call ${state.call}`];
  if (state.iceConnectionState) parts.push(`ice ${state.iceConnectionState}`);
  if (state.cause) parts.push(`cause ${state.cause}`);
  return parts.join(', ');
}

/**
 * The parts of a `getStats()` report that say whether media moved and where.
 * Records reference each other by id: the transport names the selected pair,
 * the pair its two candidates, and the inbound stream its codec.
 */
export function summarise(report: RTCStatsReport): MediaStats {
  const records = new Map<string, Record<string, unknown>>();
  report.forEach((value: Record<string, unknown>, key: string) => records.set(key, value));

  const stats: MediaStats = { packetsSent: 0, packetsReceived: 0, bytesSent: 0, bytesReceived: 0, packetsLost: 0 };
  let selectedPairId: string | undefined;
  const video = (): VideoStats => (stats.video ??= { packetsSent: 0, packetsReceived: 0, framesDecoded: 0 });

  for (const record of records.values()) {
    switch (record.type) {
      case 'outbound-rtp':
        if (record.kind === 'video') {
          video().packetsSent += number(record.packetsSent);
          break;
        }
        stats.packetsSent += number(record.packetsSent);
        stats.bytesSent += number(record.bytesSent);
        break;
      case 'inbound-rtp': {
        if (record.kind === 'video') {
          video().packetsReceived += number(record.packetsReceived);
          video().framesDecoded += number(record.framesDecoded);
          const codec = typeof record.codecId === 'string' ? records.get(record.codecId) : undefined;
          if (codec && typeof codec.mimeType === 'string') video().codec = codec.mimeType;
          break;
        }
        stats.packetsReceived += number(record.packetsReceived);
        stats.bytesReceived += number(record.bytesReceived);
        stats.packetsLost += number(record.packetsLost);
        if (typeof record.audioLevel === 'number') stats.audioLevel = record.audioLevel;
        if (typeof record.totalAudioEnergy === 'number') stats.totalAudioEnergy = (stats.totalAudioEnergy ?? 0) + record.totalAudioEnergy;
        const codec = typeof record.codecId === 'string' ? records.get(record.codecId) : undefined;
        if (codec && typeof codec.mimeType === 'string') stats.codec = codec.mimeType;
        break;
      }
      case 'media-source':
        if (record.kind === 'audio' && typeof record.totalAudioEnergy === 'number') {
          stats.sentAudioEnergy = (stats.sentAudioEnergy ?? 0) + record.totalAudioEnergy;
        }
        break;
      case 'transport':
        if (typeof record.dtlsState === 'string') stats.dtlsState = record.dtlsState;
        if (typeof record.selectedCandidatePairId === 'string') selectedPairId = record.selectedCandidatePairId;
        break;
    }
  }

  // Firefox reports no transport record; fall back to the nominated, succeeded pair.
  const pair = (selectedPairId && records.get(selectedPairId))
    ?? [...records.values()].find((record) => record.type === 'candidate-pair' && record.state === 'succeeded' && record.nominated === true);
  if (pair) {
    const local = typeof pair.localCandidateId === 'string' ? records.get(pair.localCandidateId) : undefined;
    const remote = typeof pair.remoteCandidateId === 'string' ? records.get(pair.remoteCandidateId) : undefined;
    if (local && remote) {
      stats.candidatePair = { local: candidate(local), remote: candidate(remote), state: String(pair.state ?? '') };
    }
  }
  return stats;
}

function candidate(record: Record<string, unknown>): CandidateEnd {
  return {
    address: String(record.address ?? record.ip ?? ''),
    port: number(record.port),
    type: String(record.candidateType ?? ''),
    protocol: String(record.protocol ?? ''),
  };
}

function number(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0;
}

/** The tracks a connection is sending, as one stream. */
function senderStream(peer: RTCPeerConnection | undefined): MediaStream | undefined {
  if (!peer || typeof MediaStream === 'undefined') return undefined;
  const tracks = peer.getSenders().map((sender) => sender.track).filter((track): track is MediaStreamTrack => !!track);
  return tracks.length ? new MediaStream(tracks) : undefined;
}

/**
 * A description's video m-line, or nothing when it has none. Port 0 is a
 * decline whatever the attributes say (RFC 3264); the direction defaults to
 * sendrecv.
 */
export function videoLine(sdp: string | undefined): VideoLine | undefined {
  if (!sdp) return undefined;
  const sections = sdp.split(/\r?\n(?=m=)/);
  const section = sections.find((candidate) => candidate.startsWith('m=video '));
  if (!section) return undefined;
  const port = Number(/^m=video (\d+)/.exec(section)?.[1] ?? 0);
  const named = /^a=(sendrecv|sendonly|recvonly|inactive)\s*$/m.exec(section)?.[1] as VideoLine['direction'] | undefined;
  const mid = /^a=mid:(\S+)/m.exec(section)?.[1];
  const group = /^a=group:BUNDLE((?: \S+)*)\s*$/m.exec(sections[0])?.[1].trim().split(' ') ?? [];
  return { port, direction: named ?? 'sendrecv', bundled: mid !== undefined && group.includes(mid) };
}
