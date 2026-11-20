import type { AdminApi } from './AdminApi';
import { authorization, digestHashes, parseChallenges } from './digest';
import { ApiError } from './errors';
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
import { ROLES, type Role } from './types';

export interface HttpAdminApiOptions {
  /**
   * Where `/api/v1` lives. Empty by default, meaning same-origin: the server
   * serves this bundle and the API, and a dev server proxies `/api`.
   */
  baseUrl?: string;
  /** Read on every request, so a new token is picked up. */
  token?: () => string | undefined;
  /**
   * Called with every 401 on the session's own requests, before the request
   * rejects, so the shell can end the session whichever screen asked.
   */
  onUnauthorized?: (error: ApiError) => void;
  /**
   * Called with every 403 on the session's own requests, except a
   * `wrong_password`. The node re-checks roles on every request, so a 403 is
   * the moment to re-read what the session may do.
   */
  onForbidden?: (error: ApiError) => void;
  fetch?: typeof globalThis.fetch;
}

interface RequestOptions {
  body?: unknown;
  signal?: AbortSignal;
  /**
   * A token to send instead of the session's, or `''` for none. Either way
   * the request is not the session's: its 401 and 403 reach neither callback.
   */
  token?: string;
  /** Statuses that are an answer rather than a failure, as a 503 from health is. */
  accept?: readonly number[];
}

/**
 * The only file that knows this client talks HTTP. Everything above it holds
 * an `AdminApi` and never sees a URL, a verb or a status code.
 */
export class HttpAdminApi implements AdminApi {
  private readonly baseUrl: string;
  private readonly token: () => string | undefined;
  private readonly onUnauthorized: (error: ApiError) => void;
  private readonly onForbidden: (error: ApiError) => void;
  private readonly http: typeof globalThis.fetch;

  constructor(options: HttpAdminApiOptions = {}) {
    // A trailing slash would produce `//api/v1`, which some proxies treat as a different path.
    this.baseUrl = (options.baseUrl ?? '').replace(/\/+$/, '');
    this.token = options.token ?? (() => undefined);
    this.onUnauthorized = options.onUnauthorized ?? (() => undefined);
    this.onForbidden = options.onForbidden ?? (() => undefined);
    this.http = options.fetch ?? globalThis.fetch.bind(globalThis);
  }

  private async request<T>(method: string, path: string, options: RequestOptions = {}): Promise<T> {
    const headers: Record<string, string> = { Accept: 'application/json' };
    const token = options.token === undefined ? this.token() : options.token;
    if (token) headers.Authorization = `Bearer ${token}`;
    if (options.body !== undefined) headers['Content-Type'] = 'application/json';

    const response = await this.http(`${this.baseUrl}/api/v1${path}`, {
      method,
      headers,
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
      signal: options.signal,
    });

    if (!response.ok && !options.accept?.includes(response.status)) {
      const failure = await describeFailure(response);
      // A probe with a candidate token, or a login, is not the session failing.
      if (options.token === undefined) {
        if (failure.isUnauthorized) this.onUnauthorized(failure);
        // A wrong old password is about the body, not about what this session may do.
        if (failure.isForbidden && failure.code !== 'wrong_password') this.onForbidden(failure);
      }
      throw failure;
    }
    // 204, and any other body-less success: parsing an empty body would throw.
    if (response.status === 204) return undefined as T;
    const text = await response.text();
    if (!text) return undefined as T;
    return JSON.parse(text) as T;
  }

  /**
   * A subscriber's own route, signed with HTTP Digest rather than the session.
   * The first request goes unsigned to fetch a nonce; a stale one is answered
   * once more. No 401 here is the session's, so neither callback hears it.
   * `credentials: 'omit'` keeps the browser from offering its own login box.
   */
  private async signed<T>(method: string, path: string, line: SubscriberLine, signal?: AbortSignal): Promise<T> {
    const url = `${this.baseUrl}/api/v1${path}`;
    const target = new URL(url, 'http://node');
    const send = (signature?: string) => this.http(url, {
      method,
      headers: { Accept: 'application/json', ...(signature ? { Authorization: signature } : {}) },
      credentials: 'omit',
      signal,
    });
    let response = await send();
    for (let answered = 0; response.status === 401 && answered < 2; answered += 1) {
      const challenges = parseChallenges(response.headers.get('WWW-Authenticate') ?? '');
      if (answered > 0 && !challenges.some((challenge) => challenge.stale)) break;
      const signature = await authorization(
        challenges,
        { username: line.user, password: line.password, method, uri: target.pathname + target.search },
        digestHashes(),
      );
      if (!signature) break;
      response = await send(signature);
    }
    if (!response.ok) throw await describeFailure(response);
    const text = await response.text();
    return (text ? JSON.parse(text) : undefined) as T;
  }

  login(username: string, password: string, signal?: AbortSignal) {
    return this.request<LoginResult>('POST', '/auth/login', { body: { username, password }, token: '', signal });
  }

  logout(signal?: AbortSignal) {
    return this.request<void>('POST', '/auth/logout', { signal });
  }

  async session(token?: string, signal?: AbortSignal): Promise<SessionInfo> {
    const info = await this.request<Partial<SessionInfo>>('GET', '/session', { token, signal });
    return {
      username: typeof info?.username === 'string' ? info.username : '',
      display_name: typeof info?.display_name === 'string' ? info.display_name : '',
      roles: knownRoles(info?.roles),
      ...(typeof info?.expires_at === 'number' ? { expires_at: info.expires_at } : {}),
    };
  }

  health(signal?: AbortSignal) { return this.request<Health>('GET', '/health', { signal, accept: [503] }); }
  nodes(signal?: AbortSignal) { return this.request<ClusterNode[]>('GET', '/nodes', { signal }); }
  subscriberConfig(line: SubscriberLine, signal?: AbortSignal) {
    return this.signed<ClientConfig>('GET', `/subscriber/${segment(line.realm)}/config`, line, signal);
  }

  listRealms(signal?: AbortSignal) { return this.request<Realm[]>('GET', '/realms', { signal }); }
  createRealm(realm: CreateRealm, signal?: AbortSignal) {
    return this.request<Realm>('POST', '/realms', { body: realm, signal });
  }
  updateRealm(name: string, changes: UpdateRealm, signal?: AbortSignal) {
    return this.request<Realm>('PUT', `/realms/${segment(name)}`, { body: changes, signal });
  }
  deleteRealm(name: string, signal?: AbortSignal) {
    return this.request<void>('DELETE', `/realms/${segment(name)}`, { signal });
  }

  listSubscribers(realm: string, signal?: AbortSignal) {
    return this.request<Subscriber[]>('GET', `/realms/${segment(realm)}/subscribers`, { signal });
  }
  createSubscriber(realm: string, subscriber: CreateSubscriber, signal?: AbortSignal) {
    return this.request<Subscriber>('POST', `/realms/${segment(realm)}/subscribers`, { body: subscriber, signal });
  }
  updateSubscriber(realm: string, user: string, changes: UpdateSubscriber, signal?: AbortSignal) {
    return this.request<Subscriber>('PUT', `/realms/${segment(realm)}/subscribers/${segment(user)}`, { body: changes, signal });
  }
  deleteSubscriber(realm: string, user: string, signal?: AbortSignal) {
    return this.request<void>('DELETE', `/realms/${segment(realm)}/subscribers/${segment(user)}`, { signal });
  }

  listRegistrations(realm?: string, signal?: AbortSignal) {
    const query = realm ? `?realm=${encodeURIComponent(realm)}` : '';
    return this.request<Registration[]>('GET', `/registrations${query}`, { signal });
  }

  listCalls(signal?: AbortSignal) { return this.request<Call[]>('GET', '/calls', { signal }); }
  listCallRecords(limit?: number, signal?: AbortSignal) {
    return this.request<CallRecord[]>('GET', limit ? `/call-records?limit=${limit}` : '/call-records', { signal });
  }
  /** A Call-ID can hold `@` and `/`; encoded, it is still one segment. */
  getCall(id: string, signal?: AbortSignal) { return this.request<Call>('GET', `/calls/${segment(id)}`, { signal }); }
  mediaEngine(signal?: AbortSignal) { return this.request<MediaEngine>('GET', '/media', { signal }); }
  listMediaReoffers(signal?: AbortSignal) { return this.request<MediaReoffer[]>('GET', '/media/reoffers', { signal }); }
  listQualifiedClients(signal?: AbortSignal) { return this.request<QualifiedClient[]>('GET', '/qualify', { signal }); }

  listUsers(signal?: AbortSignal) { return this.request<AdminUser[]>('GET', '/users', { signal }); }
  createUser(user: CreateAdminUser, signal?: AbortSignal) {
    return this.request<AdminUser>('POST', '/users', { body: user, signal });
  }
  updateUser(username: string, changes: UpdateAdminUser, signal?: AbortSignal) {
    return this.request<AdminUser>('PUT', `/users/${segment(username)}`, { body: changes, signal });
  }
  deleteUser(username: string, signal?: AbortSignal) {
    return this.request<void>('DELETE', `/users/${segment(username)}`, { signal });
  }
  changePassword(username: string, change: ChangePassword, signal?: AbortSignal) {
    return this.request<void>('POST', `/users/${segment(username)}/password`, { body: change, signal });
  }
  revokeSessions(username: string, signal?: AbortSignal) {
    return this.request<void>('DELETE', `/users/${segment(username)}/sessions`, { signal });
  }
}

/** The roles this client knows, from whatever the node sent. An unknown role is ignored. */
function knownRoles(value: unknown): Role[] {
  const given = Array.isArray(value) ? value : [];
  return ROLES.filter((role) => given.includes(role));
}

/**
 * One path segment. Per the OpenAPI document a user containing `@` or `/` is
 * one segment, and a realm name with a slash must not reach another route.
 */
function segment(value: string): string {
  return encodeURIComponent(value);
}

/**
 * Turns a failed response into an `ApiError` and never throws doing so. The
 * body may not be the `{"error": {"code", "message"}}` envelope (a proxy's
 * HTML 502, an empty body); the status line is then the message.
 */
async function describeFailure(response: Response): Promise<ApiError> {
  let message = `${response.status} ${response.statusText}`.trim();
  let code: string | undefined;
  try {
    const body = await response.text();
    if (body) {
      const parsed: unknown = JSON.parse(body);
      const envelope = parsed && typeof parsed === 'object' ? (parsed as { error?: unknown }).error : undefined;
      if (envelope && typeof envelope === 'object') {
        const { message: said, code: named } = envelope as { message?: unknown; code?: unknown };
        if (typeof said === 'string' && said) message = said;
        if (typeof named === 'string') code = named;
      }
    }
  } catch {
    // Not the envelope: keep the status line.
  }
  const retry = Number(response.headers.get('Retry-After'));
  return new ApiError(message, response.status, code, Number.isFinite(retry) && retry > 0 ? retry : undefined);
}
