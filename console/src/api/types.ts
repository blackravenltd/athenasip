/**
 * The admin API's records, as the server's `docs/api/openapi.yaml` describes
 * them. Field names are the wire names, snake_case.
 *
 * Ids are 64-bit on the server and arrive rounded in a JavaScript number, so
 * never key on one: a realm is keyed by its name, a subscriber by its user.
 */

export type SipTransport = 'udp' | 'tcp' | 'tls' | 'ws' | 'wss';

/**
 * What a user may do, from the server's `docs/authentication.md`. Roles are
 * independent permissions, not ranks: a user holds exactly those given,
 * possibly none.
 */
export type Role =
  | 'view-cluster-status'
  | 'manage-admin-users'
  | 'manage-realms'
  | 'manage-realm-subscribers'
  | 'manage-cluster'
  | 'manage-trunks';

export const ROLES: readonly Role[] = [
  'view-cluster-status',
  'manage-realms',
  'manage-realm-subscribers',
  'manage-admin-users',
  'manage-cluster',
  'manage-trunks',
];

/** `GET /session`: who the presented session token is, and what it may do. */
export interface SessionInfo {
  /** Compared case-insensitively on the node, stored as given. */
  username: string;
  display_name: string;
  roles: Role[];
  /** Unix seconds, the absolute expiry. */
  expires_at?: number;
}

/** `POST /auth/login`. The token is opaque, sent as a bearer, and not retrievable again. */
export interface LoginResult {
  token: string;
  /** Unix seconds. */
  expires_at: number;
  roles: Role[];
}

/** A console user: a person or system that administers the node. Not a realm subscriber. */
export interface AdminUser {
  username: string;
  display_name: string;
  roles: Role[];
  /** Kept for the audit trail; cannot log in. */
  disabled: boolean;
  /** Unix seconds. */
  created_at: number;
  /** Unix seconds; zero when the user has never logged in. */
  last_login_at: number;
}

export interface CreateAdminUser {
  username: string;
  display_name: string;
  password: string;
  /** None by default. */
  roles: Role[];
}

/** Only the fields given are changed. */
export interface UpdateAdminUser {
  display_name?: string;
  roles?: Role[];
  disabled?: boolean;
}

/** `old_password` is required to change your own, and not needed with `manage-admin-users`. */
export interface ChangePassword {
  password: string;
  old_password?: string;
}

/** `GET /health`. Open, with no token. */
export interface Health {
  /** `degraded` when the datastore is not connected; the server answers that with a 503. */
  status: 'ok' | 'degraded';
  node: string;
  version: string;
  /** The datastore driver and its version, as it describes itself. */
  datastore: string;
}

export interface NodeTransport {
  transport: SipTransport;
  address: string;
  port: number;
  /** The SIP URI a client would use for this transport. */
  uri: string;
}

/**
 * One entry of `GET /subscriber/{realm}/config`'s `ice_servers`, shaped as `RTCIceServer`.
 * A `turn:` entry carries a credential minted per request under the coturn
 * shared-secret scheme, or none when the node has no secret configured.
 */
export interface IceServer {
  urls: string;
  /** The credential's own expiry, optionally with a name after a colon. Not an identity. */
  username?: string;
  credential?: string;
  /** Unix seconds, when the credential stops working. */
  expires_at?: number;
}

/** A subscriber's own credentials, which sign the routes under `/subscriber/{realm}/`. Held in memory only. */
export interface SubscriberLine {
  realm: string;
  user: string;
  password: string;
}

/** What a realm expects of a client, in `GET /subscriber/{realm}/config`. */
export interface RealmClientPolicy {
  name: string;
  /** Seconds granted when a client asks for none, and the shortest taken (0 for no minimum). */
  registration: { expires?: number; minimum?: number };
  /** How many flows to keep, each to a different node (RFC 5626). */
  outbound: { flows?: number };
  /** The push services the node runs (RFC 8599); empty for none. */
  push: Array<{ service: string; vapid?: string; minimum_expires: number }>;
}

/** `GET /subscriber/{realm}/config`: what a subscriber's browser needs to register and place a call. */
export interface ClientConfig {
  /** The secure WebSocket to signal over; absent when the node has no `wss` listener. */
  websocket_uri?: string;
  /** Every node's secure WebSocket, this node first, to fail over to. */
  websocket_uris?: string[];
  /** The same list `/nodes` gives. */
  transports: NodeTransport[];
  ice_servers: IceServer[];
  realm?: RealmClientPolicy;
}

/**
 * `GET /nodes`: this node first, marked `self`, then every other node as it
 * last described itself on the event bus.
 */
export interface ClusterNode {
  id: string;
  self: boolean;
  status?: 'ok' | 'degraded' | 'stopped' | 'down';
  /** Absent for this node; true when another node's report is more than three status intervals old. */
  stale?: boolean;
  version?: string;
  /** When the node composed its report, ISO 8601; absent for this node. */
  at?: string;
  /** One per enabled SIP transport. A transport that is off is absent, not listed as off. */
  transports: NodeTransport[];
}

/** What the media engine is asked to produce for a leg that has not said what it speaks. */
export type MediaProfile = 'mirror' | 'transport' | 'rtp' | 'webrtc' | 'srtp';

export const MEDIA_PROFILES: readonly MediaProfile[] = ['mirror', 'transport', 'rtp', 'webrtc', 'srtp'];

/**
 * A realm's overrides of the server's `behaviour:` section. Null inherits the
 * server's; in an update, null goes back to inheriting and a setting left out
 * is left alone.
 */
export interface Behaviour {
  /** Whether this node puts itself in the media path when a media engine is configured. */
  media_anchor?: boolean | null;
  media_profile?: MediaProfile | null;
  /** Seconds between OPTIONS to each registered client; 0 for never, otherwise 5 to 86400. */
  qualify_interval?: number | null;
  /**
   * Rewrite the Contact of each forwarded message to the address and port it
   * came from, as Asterisk's `rewrite_contact`. Never applied to WebSocket clients.
   */
  rewrite_contact?: boolean | null;
}

/** A complete behaviour: what a realm's comes to, or the server's own. */
export interface BehaviourEffective {
  media_anchor: boolean;
  media_profile: MediaProfile;
  qualify_interval: number;
  rewrite_contact: boolean;
}

/** The bounds the node puts on `qualify_interval` other than 0. */
export const QUALIFY_MIN = 5;
export const QUALIFY_MAX = 86400;

/** A SIP domain this server is responsible for. Addressed by `name`. */
export interface Realm {
  name: string;
  /** Rounded; see the top of this file. */
  id: number;
  /** How long a Digest nonce stays good, in seconds. 3600 when not given. */
  nonce_expiry: number;
  /** The longest registration this realm grants, in seconds. 5000 when not given. */
  registration_timeout: number;
  /** The shortest registration it accepts, in seconds. Zero accepts any. */
  registration_minimum: number;
  /** As the realm set it: every setting present, null where it inherits. */
  behaviour: Required<Behaviour>;
  behaviour_effective: BehaviourEffective;
  /** The `behaviour:` section of the node that answered: what a null setting inherits. */
  behaviour_default: BehaviourEffective;
}

export interface RealmSettings {
  nonce_expiry?: number;
  registration_timeout?: number;
  registration_minimum?: number;
  behaviour?: Behaviour;
}

/** The server generates `nonce_secret` and never returns it. */
export interface CreateRealm extends RealmSettings {
  name: string;
}

/** Only the fields given are changed. A realm cannot be renamed. */
export type UpdateRealm = RealmSettings;

/** One subscriber in one realm. Addressed by `user` within its realm. */
export interface Subscriber {
  /** Derived from the URI, so every node agrees on it. Rounded; see the top of this file. */
  id: number;
  uri: string;
  user: string;
  realm: string;
  /** As the subscriber set it: null where it takes its realm's. */
  behaviour: Required<SubscriberBehaviour>;
}

/**
 * The one behaviour setting that belongs to an endpoint: the media profile
 * first offered to it. Overrides the realm's and yields to what the endpoint
 * has itself said. Null takes the realm's; null in an update goes back to it.
 */
export interface SubscriberBehaviour {
  media_profile?: MediaProfile | null;
}

/** The server computes HA1 from the password and keeps only that. */
export interface CreateSubscriber {
  user: string;
  password: string;
  behaviour?: SubscriberBehaviour;
}

/** Either or both. A password left out is left alone. */
export interface UpdateSubscriber {
  password?: string;
  behaviour?: SubscriberBehaviour;
}

/** A live binding. Read only: only a REGISTER writes one. */
export interface Registration {
  /** The subscriber's URI. */
  subscriber: string;
  subscriber_id: number;
  contact: string;
  /** Unix seconds. */
  registered_at: number;
  /** Unix seconds. */
  expires_at: number;
  /** The contact is on a private address, so replies go back down the flow. */
  nat: boolean;
  /** Which node holds the flow. Empty on a single node. */
  node_id: string;
  /** The connection it was learned over (RFC 5626). Empty on a single node. */
  flow_id: string;
  /** The RFC 3327 Path recorded at registration. */
  path: string;
}

/** Whether a transport carries signalling in the clear. */
export function isEncrypted(transport: SipTransport): boolean {
  return transport === 'tls' || transport === 'wss';
}

/** A call's state, as the server's `Call::state_to_string` spells it. */
export type CallState = 'Initial' | 'Trying' | 'Ringing' | 'Connected' | 'Closing' | 'Closed';

/** One party to a call, as its own signalling described it. */
export interface CallParticipant {
  /** The participant's SIP URI. */
  identity: string;
  /** The one who placed the call. */
  originator: boolean;
  /** What this participant's session description said it is; null until it has said. */
  profile: 'webrtc' | 'plain-rtp' | 'srtp-sdes' | null;
}

/**
 * One end of one stream as the media engine sees it, cumulative over the
 * call. `_in` arrived from that end, `_out` was sent to it. A direction the
 * engine does not report is absent, never zero: rtpengine usually reports
 * only the `_in` pair.
 */
export interface CallLeg {
  /**
   * An index into the call's participants. Always null: a relay knows an end
   * only by its packets' source address, which behind a NAT no participant
   * described.
   */
  participant: number | null;
  packets_in?: number;
  bytes_in?: number;
  packets_out?: number;
  bytes_out?: number;
}

export interface CallMedia {
  engine: string;
  /** Seconds all of the call's media has been silent; null when the engine cannot say. */
  idle_seconds: number | null;
  legs: CallLeg[];
}

/**
 * `GET /call-records`: one call that has ended, newest first, kept for the
 * node's `calls.history_retention` (thirty days by default). A call refused
 * before anything rang has none.
 */
export interface CallRecord {
  /** The Call-ID. */
  id: string;
  created_at: string | null;
  /** Null for a call nobody answered. */
  answered_at: string | null;
  ended_at: string | null;
  /** Seconds from the answer to the end; 0 for a call nobody answered. */
  duration: number;
  caller: string | null;
  callee: string | null;
  /** The nodes that carried it, the caller's first. */
  nodes: string[];
  media_engine: string | null;
}

/** `GET /calls`: one live call. An ended call is gone from the list. */
export interface Call {
  /** The Call-ID. */
  id: string;
  state: CallState;
  /** ISO 8601. */
  created_at: string | null;
  /** ISO 8601; null until answered. */
  answered_at: string | null;
  participants: CallParticipant[];
  /** Null when nothing anchors the call: the realm does not, or the node has no engine. */
  media: CallMedia | null;
}

/** `GET /media`: the engine's name and what it can do, never its URL. */
export interface MediaEngine {
  /** `builtin` or `rtpengine`; null when the node has no engine. */
  engine: string | null;
  connected: boolean;
  capabilities: string[];
}

/**
 * `GET /media/reoffers`: a subscriber whose endpoint answered an offer with 488
 * and was offered the other profile once. The node only suggests; it never
 * sets the subscriber's profile.
 */
export interface MediaReoffer {
  /** The subscriber's address of record, as the call's Request-URI named it. */
  subscriber: string;
  rejected: 'rtp' | 'webrtc';
  /** Null when the other profile was refused as well. */
  took: 'rtp' | 'webrtc' | null;
  count: number;
  /** ISO 8601. */
  last_at: string;
  /** What the subscriber's `behaviour.media_profile` would be set to. */
  suggested_media_profile: 'rtp' | 'webrtc' | null;
}

/** `GET /qualify`: a registered client this node probes with OPTIONS, down the flow it registered on. */
export interface QualifiedClient {
  subscriber: string;
  contact: string;
  /** Seconds between probes. */
  interval: number;
  /** ISO 8601; null until it has answered. */
  answered_at: string | null;
  /** Probes in a row that went unanswered. */
  unanswered: number;
  /** What media the client said it takes, when its answer carried a description. */
  said_media_profile: 'rtp' | 'webrtc' | 'srtp' | null;
}
