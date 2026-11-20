/**
 * The contract for driving the softphone from outside: the query string a
 * harness opens the page with, and the readout on `window` that a test reads
 * instead of scraping the screen. Both are documented in `docs/softphone.md`.
 */
import type { IceServer } from '../api/types';
import { videoLine, type CallMedia, type MediaStats, type Softphone, type SoftphoneState, type Transition, type VideoLine } from './Softphone';

export interface PageOptions {
  socket?: string;
  uri?: string;
  password?: string;
  target?: string;
  /** Register as soon as the page loads. */
  register: boolean;
  /** Answer an incoming call as soon as it arrives. */
  answer: boolean;
  /**
   * ICE servers, as `GET /subscriber/{realm}/config` gives them. The harness fetches
   * them, so the page needs no token.
   */
  ice?: IceServer[];
  /** Media only through the TURN server, `iceTransportPolicy: "relay"`. */
  relay?: boolean;
  /** Send the camera on calls placed and answered. */
  video?: boolean;
}

/**
 * Parses the query string. `ws`, `uri`, `password` and `target` prefill the
 * fields; `register=1`, `answer=1`, `relay=1` and `video=1` set the flags;
 * `ice` is a JSON array of ICE servers. The page strips `password` and `ice`
 * from the address bar once read (see `withoutSecrets`).
 */
export function pageOptions(search: string): PageOptions {
  const params = new URLSearchParams(search);
  const text = (key: string) => {
    const value = params.get(key);
    return value === null || value === '' ? undefined : value;
  };
  const flag = (key: string) => {
    const value = params.get(key);
    return value === '1' || value === 'true';
  };
  return {
    socket: text('ws'),
    uri: text('uri'),
    password: text('password'),
    target: text('target'),
    register: flag('register'),
    answer: flag('answer'),
    ice: iceServers(text('ice')),
    relay: flag('relay'),
    video: flag('video'),
  };
}

/** Keeps entries with a string `urls`. Anything but a JSON array yields `undefined`. */
function iceServers(json: string | undefined): IceServer[] | undefined {
  if (json === undefined) return undefined;
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch {
    return undefined;
  }
  if (!Array.isArray(parsed)) return undefined;
  return parsed.filter((entry): entry is IceServer => typeof entry === 'object' && entry !== null && typeof (entry as IceServer).urls === 'string');
}

/** The URL without `password` and `ice`, or `undefined` when it had neither. */
export function withoutSecrets(url: string): string | undefined {
  const parsed = new URL(url);
  const secrets = ['password', 'ice'].filter((key) => parsed.searchParams.has(key));
  if (secrets.length === 0) return undefined;
  for (const key of secrets) parsed.searchParams.delete(key);
  return parsed.toString();
}

/**
 * What the far end's description did with the video line: `absent` when there
 * is none, `declined` at port 0, `bundled` when accepted on the bundle's
 * transport (port 9, a placeholder), `accepted` otherwise.
 */
export type VideoOutcome = 'absent' | 'declined' | 'bundled' | 'accepted';

export interface VideoNegotiation {
  local?: VideoLine;
  remote?: VideoLine;
  outcome: VideoOutcome;
}

export function videoNegotiation(state: SoftphoneState): VideoNegotiation {
  const local = videoLine(state.localSdp);
  const remote = videoLine(state.remoteSdp);
  const outcome: VideoOutcome = !remote ? 'absent' : remote.port === 0 ? 'declined' : remote.bundled && remote.port === 9 ? 'bundled' : 'accepted';
  return { local, remote, outcome };
}

/** What a harness can read from `window.__athenaSoftphone`. */
export interface SoftphoneReadout {
  /** The contract's version, bumped when a field changes meaning. */
  readonly version: 1;
  state(): SoftphoneState;
  history(): readonly Transition[];
  /** Audio counters at the top level; `video` carries the video's own, when the call has any. */
  stats(): Promise<MediaStats | undefined>;
  /** Each end's video line and what the far end did with it. */
  video(): VideoNegotiation;
  call(target: string): void;
  answer(): void;
  hangUp(): void;
}

export const READOUT_KEY = '__athenaSoftphone';

declare global {
  interface Window {
    [READOUT_KEY]?: SoftphoneReadout;
  }
}

/**
 * Puts the readout on the window and returns the function that removes it.
 * `media` is what the readout's calls and answers send.
 */
export function expose(target: Window, phone: Softphone, media: CallMedia = {}): () => void {
  const readout: SoftphoneReadout = {
    version: 1,
    state: () => phone.state,
    history: () => phone.history,
    stats: () => phone.stats(),
    video: () => videoNegotiation(phone.state),
    call: (destination) => phone.call(destination, undefined, media),
    answer: () => phone.answer(undefined, media),
    hangUp: () => phone.hangUp(),
  };
  target[READOUT_KEY] = readout;
  return () => {
    if (target[READOUT_KEY] === readout) delete target[READOUT_KEY];
  };
}
