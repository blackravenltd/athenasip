import type {
  Subscriber,
  Call,
  CallRecord,
  AdminUser,
  ChangePassword,
  ClientConfig,
  ClusterNode,
  CreateAdminUser,
  CreateSubscriber,
  CreateRealm,
  Health,
  Realm,
  LoginResult,
  MediaEngine,
  MediaReoffer,
  QualifiedClient,
  Registration,
  SessionInfo,
  SubscriberLine,
  UpdateSubscriber,
  UpdateAdminUser,
  UpdateRealm,
} from './types';

/**
 * Everything this client can ask an AthenaSIP node to do, per the server's
 * `docs/api/openapi.yaml` (version 1, under `/api/v1`) and
 * `docs/authentication.md`. A session token from a login is the bearer on
 * everything but health, the login itself, and a subscriber's own routes.
 *
 * `HttpAdminApi` speaks to a real node and is the only file that knows HTTP.
 * `FakeAdminApi` holds the same records in memory and enforces the same
 * rules; development without a node and every screen test run against it.
 *
 * Every method takes an `AbortSignal` so a component that unmounts
 * mid-flight can cancel.
 */
export interface AdminApi {
  /**
   * Exchanges a username and password for a session. A 401 is a wrong
   * password or a disabled user and does not end any session in progress; a
   * 404 is a node without user logins, which this console cannot use.
   */
  login(username: string, password: string, signal?: AbortSignal): Promise<LoginResult>;
  /** Ends the presented session on the node. */
  logout(signal?: AbortSignal): Promise<void>;
  /** Who a token is and what it may do, without keeping it. The session's own when `token` is left out. */
  session(token?: string, signal?: AbortSignal): Promise<SessionInfo>;

  /** Open. Resolves on a degraded node too; the status says which. */
  health(signal?: AbortSignal): Promise<Health>;
  nodes(signal?: AbortSignal): Promise<ClusterNode[]>;
  /**
   * Signed with the subscriber's own SIP credentials, not the session: a 401
   * is a wrong password for the line and ends no session. The TURN credential
   * is minted per request, so fetch it as a call is placed, never cache it.
   */
  subscriberConfig(line: SubscriberLine, signal?: AbortSignal): Promise<ClientConfig>;

  listRealms(signal?: AbortSignal): Promise<Realm[]>;
  createRealm(realm: CreateRealm, signal?: AbortSignal): Promise<Realm>;
  updateRealm(name: string, changes: UpdateRealm, signal?: AbortSignal): Promise<Realm>;
  deleteRealm(name: string, signal?: AbortSignal): Promise<void>;

  listSubscribers(realm: string, signal?: AbortSignal): Promise<Subscriber[]>;
  createSubscriber(realm: string, subscriber: CreateSubscriber, signal?: AbortSignal): Promise<Subscriber>;
  updateSubscriber(realm: string, user: string, changes: UpdateSubscriber, signal?: AbortSignal): Promise<Subscriber>;
  deleteSubscriber(realm: string, user: string, signal?: AbortSignal): Promise<void>;

  /** Read only. Every realm's when `realm` is left out. */
  listRegistrations(realm?: string, signal?: AbortSignal): Promise<Registration[]>;

  /** The calls this node is carrying. Read only; a call is gone once ended. */
  listCalls(signal?: AbortSignal): Promise<Call[]>;
  /** Calls that have ended, newest first; `limit` 1 to 1000, the node's default 100. */
  listCallRecords(limit?: number, signal?: AbortSignal): Promise<CallRecord[]>;
  /** One live call by its Call-ID; a 404 once it has ended. */
  getCall(id: string, signal?: AbortSignal): Promise<Call>;
  mediaEngine(signal?: AbortSignal): Promise<MediaEngine>;
  /** Subscribers whose endpoint refused the media profile it was offered, most recent first. */
  listMediaReoffers(signal?: AbortSignal): Promise<MediaReoffer[]>;
  /** The registered clients this node is probing with OPTIONS. */
  listQualifiedClients(signal?: AbortSignal): Promise<QualifiedClient[]>;

  listUsers(signal?: AbortSignal): Promise<AdminUser[]>;
  createUser(user: CreateAdminUser, signal?: AbortSignal): Promise<AdminUser>;
  updateUser(username: string, changes: UpdateAdminUser, signal?: AbortSignal): Promise<AdminUser>;
  deleteUser(username: string, signal?: AbortSignal): Promise<void>;
  changePassword(username: string, change: ChangePassword, signal?: AbortSignal): Promise<void>;
  /** Ends every session the user holds. */
  revokeSessions(username: string, signal?: AbortSignal): Promise<void>;
}
