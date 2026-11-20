import type { ClientConfig, SubscriberLine } from '../api/types';
import type { CallState, RegistrationState, VideoLine } from './Softphone';

/** The softphone's states and negotiation in words, shared by the harness view and the Phone section. */

const CALL_LABELS: Record<CallState, string> = {
  idle: 'Idle',
  calling: 'Calling...',
  ringing: 'Ringing...',
  incoming: 'Incoming call',
  connected: 'Connected',
  ended: 'Call ended',
  failed: 'Call failed',
};

const REGISTRATION_LABELS: Record<RegistrationState, string> = {
  unregistered: 'Not registered',
  connecting: 'Registering...',
  registered: 'Registered',
  failed: 'Registration failed',
};

export function describeCallState(state: CallState): string {
  return CALL_LABELS[state];
}

export function describeRegistration(state: RegistrationState): string {
  return REGISTRATION_LABELS[state];
}

/** Whether there is a call to hang up. */
export function callInProgress(state: CallState): boolean {
  return state === 'calling' || state === 'ringing' || state === 'incoming' || state === 'connected';
}

/**
 * The credentials a SIP address and password sign the subscriber's own routes
 * with: the realm is the address's host. Undefined for anything that is not
 * `sip:user@realm`.
 */
export function lineOf(uri: string, password: string): SubscriberLine | undefined {
  const match = /^sips?:([^@;>]+)@([^;>?:]+)/i.exec(uri.trim());
  return match ? { user: decodeURIComponent(match[1]), realm: match[2].toLowerCase(), password } : undefined;
}

/**
 * The signalling WebSocket to use, from what the node advertises.
 *
 * An https page must use `wss:`, since browsers block `ws:` as mixed content.
 * An http page uses `ws:`, since `wss:` works only once the browser trusts the
 * node's certificate. Undefined when the node offers nothing suitable.
 */
export function signallingUri(config: ClientConfig, https: boolean): string | undefined {
  if (https) return config.websocket_uri;
  const plain = config.transports.find((entry) => entry.transport === 'ws');
  return plain ? `ws://${plain.address}:${plain.port}` : undefined;
}

/**
 * One end's video m-line in words. A bundled section at port 9, the discard
 * port, takes its address from the bundle's transport, so no port is shown.
 */
export function describeVideoLine(line: VideoLine | undefined): string {
  if (!line) return 'none';
  if (line.port === 0) return 'declined (port 0)';
  if (line.bundled && line.port === 9) return `bundled, ${line.direction}`;
  return `port ${line.port}${line.bundled ? ' (bundled)' : ''}, ${line.direction}`;
}
