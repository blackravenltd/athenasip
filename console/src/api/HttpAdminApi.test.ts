import { describe, expect, it, vi } from 'vitest';
import { HttpAdminApi } from './HttpAdminApi';
import { ApiError } from './errors';
import { authorization, digestHashes, parseChallenges } from './digest';

/**
 * A fetch that answers once. `null` rather than `''` for a body-less status:
 * the Response constructor rejects any body at all on a 204.
 */
function respond(body: string | null, init: ResponseInit): typeof globalThis.fetch {
  return vi.fn(async () => new Response(body, init)) as unknown as typeof globalThis.fetch;
}

/** A fetch that answers by path, for the calls that make more than one request. */
function route(answers: Record<string, [number, unknown]>): typeof globalThis.fetch {
  return vi.fn(async (url: string) => {
    const [status, body] = answers[url] ?? [404, { error: { code: 'not_found', message: url } }];
    return new Response(JSON.stringify(body), { status });
  }) as unknown as typeof globalThis.fetch;
}

function envelope(code: string, message: string): string {
  return JSON.stringify({ error: { code, message } });
}

describe('HttpAdminApi', () => {
  it('addresses /api/v1 on the same origin by default', async () => {
    const fetch = respond('[]', { status: 200 });
    await new HttpAdminApi({ fetch }).listRealms();
    expect(fetch).toHaveBeenCalledWith('/api/v1/realms', expect.anything());
  });

  it('does not produce a double slash when a base URL has a trailing one', async () => {
    const fetch = respond('[]', { status: 200 });
    await new HttpAdminApi({ baseUrl: 'https://sip.example.org/', fetch }).listRealms();
    expect(fetch).toHaveBeenCalledWith('https://sip.example.org/api/v1/realms', expect.anything());
  });

  it('sends the token that is current at request time, not at construction time', async () => {
    let token = 'first';
    const fetch = respond('[]', { status: 200 });
    const api = new HttpAdminApi({ fetch, token: () => token });
    await api.listRealms();
    token = 'second';
    await api.listRealms();

    const calls = (fetch as unknown as { mock: { calls: [string, RequestInit][] } }).mock.calls;
    const headerOf = (index: number) => (calls[index][1].headers as Record<string, string>).Authorization;
    expect(headerOf(0)).toBe('Bearer first');
    expect(headerOf(1)).toBe('Bearer second');
  });

  it('addresses a subscriber by realm and user, each one path segment', async () => {
    // Per the OpenAPI document a user with an @ or a / in it is one segment,
    // and a realm with a slash must not reach another route.
    const fetch = respond(null, { status: 204 });
    await new HttpAdminApi({ fetch }).deleteSubscriber('a/../b', 'x@y');
    expect(fetch).toHaveBeenCalledWith('/api/v1/realms/a%2F..%2Fb/subscribers/x%40y', expect.objectContaining({ method: 'DELETE' }));
  });

  it('addresses a call by its Call-ID as one segment, even with a slash in it', async () => {
    // RFC 3261 lets a Call-ID hold `@` and `/`, and the node routes `%2F` as one segment.
    const fetch = respond('{}', { status: 200 });
    await new HttpAdminApi({ fetch }).getCall('a/b@host');
    expect(fetch).toHaveBeenCalledWith('/api/v1/calls/a%2Fb%40host', expect.objectContaining({ method: 'GET' }));
  });

  it('filters registrations by realm only when asked', async () => {
    const fetch = respond('[]', { status: 200 });
    const api = new HttpAdminApi({ fetch });
    await api.listRegistrations();
    await api.listRegistrations('example.com');
    const calls = (fetch as unknown as { mock: { calls: [string][] } }).mock.calls;
    expect(calls.map(([url]) => url)).toEqual(['/api/v1/registrations', '/api/v1/registrations?realm=example.com']);
  });

  it("reads the server's error envelope, code and message", async () => {
    const fetch = respond(envelope('conflict', 'realm example.com already exists'), { status: 409 });
    await expect(new HttpAdminApi({ fetch }).createRealm({ name: 'example.com' })).rejects.toMatchObject({
      message: 'realm example.com already exists',
      status: 409,
      code: 'conflict',
    });
  });

  it('keeps the status line when the body is not the envelope', async () => {
    const fetch = respond('<html>502 Bad Gateway</html>', { status: 502, statusText: 'Bad Gateway' });
    const failure = await new HttpAdminApi({ fetch }).listRealms().catch((cause: unknown) => cause);
    expect(failure).toBeInstanceOf(ApiError);
    expect((failure as ApiError).message).toBe('502 Bad Gateway');
  });

  it('survives a success with no body at all', async () => {
    const fetch = respond(null, { status: 204 });
    await expect(new HttpAdminApi({ fetch }).deleteRealm('example.com')).resolves.toBeUndefined();
  });

  it('answers a degraded node with its health rather than failing', async () => {
    // A 503 from health is the node saying its datastore is gone, which the overview shows.
    const body = { status: 'degraded', node: 'n', version: '0.7.0', datastore: 'redis 0.0.1' };
    const fetch = respond(JSON.stringify(body), { status: 503 });
    await expect(new HttpAdminApi({ fetch }).health()).resolves.toEqual(body);
  });

  it('reports a 401 to the session as its end, and a 403 not', async () => {
    const onUnauthorized = vi.fn();
    const unauthorised = new HttpAdminApi({ fetch: respond(envelope('unauthorized', 'nope'), { status: 401 }), onUnauthorized });
    await expect(unauthorised.listRealms()).rejects.toMatchObject({ status: 401 });
    expect(onUnauthorized).toHaveBeenCalledOnce();

    const forbidden = new HttpAdminApi({ fetch: respond(envelope('forbidden', 'nope'), { status: 403 }), onUnauthorized });
    await expect(forbidden.listRealms()).rejects.toMatchObject({ status: 403 });
    expect(onUnauthorized).toHaveBeenCalledOnce();
  });

  it('addresses users one path segment each, with the documented verbs', async () => {
    const fetch = respond(null, { status: 204 });
    const api = new HttpAdminApi({ fetch });
    await api.revokeSessions('a/b');
    await api.changePassword('a/b', { password: 'p' });
    const calls = (fetch as unknown as { mock: { calls: [string, RequestInit][] } }).mock.calls;
    expect(calls.map(([url, init]) => `${init.method} ${url}`)).toEqual([
      'DELETE /api/v1/users/a%2Fb/sessions',
      'POST /api/v1/users/a%2Fb/password',
    ]);
  });

  describe('login and session', () => {
    it('logs in with no bearer, so a stale session token is never sent with a password', async () => {
      const fetch = route({ '/api/v1/auth/login': [200, { token: 'new', expires_at: 1, roles: [] }] });
      await new HttpAdminApi({ fetch, token: () => 'stale' }).login('ops', 'pw');
      const [[url, init]] = (fetch as unknown as { mock: { calls: [string, RequestInit][] } }).mock.calls;
      expect(url).toBe('/api/v1/auth/login');
      expect((init.headers as Record<string, string>).Authorization).toBeUndefined();
      expect(JSON.parse(String(init.body))).toEqual({ username: 'ops', password: 'pw' });
    });

    it('does not end the session on a refused login', async () => {
      const onUnauthorized = vi.fn();
      const fetch = route({ '/api/v1/auth/login': [401, { error: { code: 'unauthorized', message: 'no' } }] });
      await expect(new HttpAdminApi({ fetch, onUnauthorized }).login('ops', 'x')).rejects.toMatchObject({ status: 401 });
      expect(onUnauthorized).not.toHaveBeenCalled();
    });

    it('asks for call records with a limit only when given one', async () => {
      const fetch = vi.fn(async () => new Response('[]', { status: 200 })) as unknown as typeof globalThis.fetch;
      const api = new HttpAdminApi({ fetch });
      await api.listCallRecords();
      await api.listCallRecords(20);
      expect(vi.mocked(fetch).mock.calls.map(([url]) => url)).toEqual(['/api/v1/call-records', '/api/v1/call-records?limit=20']);
    });

    it('never signs out on a 429', async () => {
      const onUnauthorized = vi.fn();
      const fetch = vi.fn(async () => new Response('{}', { status: 429, headers: { 'Retry-After': '5' } })) as unknown as typeof globalThis.fetch;
      await expect(new HttpAdminApi({ fetch, token: () => 't', onUnauthorized }).listRealms()).rejects.toMatchObject({ status: 429, retryAfter: 5 });
      expect(onUnauthorized).not.toHaveBeenCalled();
    });

    it('carries Retry-After on a rate-limited login', async () => {
      const fetch = vi.fn(async () => new Response('{}', { status: 429, headers: { 'Retry-After': '30' } })) as unknown as typeof globalThis.fetch;
      await expect(new HttpAdminApi({ fetch }).login('ops', 'x')).rejects.toMatchObject({ status: 429, retryAfter: 30 });
    });

    it('reads /session, keeping only roles it knows', async () => {
      const fetch = route({ '/api/v1/session': [200, { kind: 'user', username: 'ada', display_name: 'Ada', roles: ['manage-realms', 'superpower'] }] });
      await expect(new HttpAdminApi({ fetch }).session('t')).resolves.toEqual({ username: 'ada', display_name: 'Ada', roles: ['manage-realms'] });
    });

    it("carries a session's expiry", async () => {
      const user = route({ '/api/v1/session': [200, { kind: 'user', username: 'ada', display_name: 'Ada', roles: [], expires_at: 1900000000 }] });
      await expect(new HttpAdminApi({ fetch: user }).session('t')).resolves.toMatchObject({ expires_at: 1900000000 });
    });

    it('neither ends the session nor re-reads its roles on a wrong old password', async () => {
      const onUnauthorized = vi.fn();
      const onForbidden = vi.fn();
      const fetch = route({ '/api/v1/users/ada/password': [403, { error: { code: 'wrong_password', message: 'the old password is not right' } }] });
      await expect(new HttpAdminApi({ fetch, onUnauthorized, onForbidden }).changePassword('ada', { password: 'n', old_password: 'o' }))
        .rejects.toMatchObject({ status: 403, code: 'wrong_password' });
      expect(onUnauthorized).not.toHaveBeenCalled();
      expect(onForbidden).not.toHaveBeenCalled();
    });

    it('rejects a token the node does not know, without ending a session', async () => {
      const onUnauthorized = vi.fn();
      const fetch = route({ '/api/v1/session': [401, { error: { code: 'unauthorized', message: 'no' } }] });
      await expect(new HttpAdminApi({ fetch, onUnauthorized }).session('t')).rejects.toMatchObject({ status: 401 });
      expect(onUnauthorized).not.toHaveBeenCalled();
    });

    it('tells the session about a 403 on its own requests, so it can re-read its roles', async () => {
      const onForbidden = vi.fn();
      const fetch = route({ '/api/v1/realms': [403, { error: { code: 'forbidden', message: 'no' } }] });
      await expect(new HttpAdminApi({ fetch, onForbidden }).listRealms()).rejects.toMatchObject({ status: 403 });
      expect(onForbidden).toHaveBeenCalledOnce();
    });
  });

  describe('a subscriber route', () => {
    const LINE = { realm: 'sip.example.org', user: '1001', password: 'secret' };
    const challenge = (nonce: string, stale = false) => new Response(envelope('unauthorized', 'sign this'), {
      status: 401,
      headers: { 'WWW-Authenticate': `Digest realm="sip.example.org", qop="auth", algorithm=SHA-256, nonce="${nonce}"${stale ? ', stale=true' : ''}, Digest realm="sip.example.org", qop="auth", algorithm=MD5, nonce="${nonce}"` },
    });

    /** A node that challenges, then accepts only a correct SHA-256 answer to `nonces`' last entry, going stale on the others. */
    function signedNode(nonces: string[]) {
      const sent: RequestInit[] = [];
      const fetch = vi.fn(async (url: string, init: RequestInit) => {
        sent.push(init);
        const given = (init.headers as Record<string, string>).Authorization;
        if (!given) return challenge(nonces[0]);
        const nonce = /nonce="([^"]+)"/.exec(given)![1];
        const cnonce = /cnonce="([^"]+)"/.exec(given)![1];
        const expected = await authorization(parseChallenges(challenge(nonce).headers.get('WWW-Authenticate')!), {
          username: LINE.user, password: LINE.password, method: 'GET', uri: new URL(url, 'http://node').pathname, cnonce,
        }, digestHashes());
        if (given !== expected) return challenge(nonce);
        const next = nonces.indexOf(nonce) + 1;
        if (next < nonces.length) return challenge(nonces[next], true);
        return new Response(JSON.stringify({ transports: [], ice_servers: [] }), { status: 200 });
      });
      return { fetch: fetch as unknown as typeof globalThis.fetch, sent };
    }

    it('signs with the line, never the session, and keeps the browser from prompting', async () => {
      const { fetch, sent } = signedNode(['n1']);
      const onUnauthorized = vi.fn();
      const api = new HttpAdminApi({ fetch, token: () => 'session-token', onUnauthorized });
      expect(await api.subscriberConfig(LINE)).toEqual({ transports: [], ice_servers: [] });
      expect(sent).toHaveLength(2);
      expect(sent.every((init) => init.credentials === 'omit')).toBe(true);
      expect(JSON.stringify(sent)).not.toContain('session-token');
      expect((sent[1].headers as Record<string, string>).Authorization).toMatch(/^Digest username="1001", realm="sip.example.org", nonce="n1", uri="\/api\/v1\/subscriber\/sip.example.org\/config", algorithm=SHA-256/);
      expect(onUnauthorized).not.toHaveBeenCalled();
    });

    it('answers a stale nonce once more', async () => {
      const { fetch, sent } = signedNode(['n1', 'n2']);
      await new HttpAdminApi({ fetch }).subscriberConfig(LINE);
      expect(sent).toHaveLength(3);
    });

    it('rejects a wrong password with the 401, ending no session', async () => {
      const { fetch, sent } = signedNode(['n1']);
      const onUnauthorized = vi.fn();
      const failure = await new HttpAdminApi({ fetch, onUnauthorized }).subscriberConfig({ ...LINE, password: 'wrong' }).catch((cause: unknown) => cause);
      expect(failure).toBeInstanceOf(ApiError);
      expect((failure as ApiError).status).toBe(401);
      expect(sent).toHaveLength(2);
      expect(onUnauthorized).not.toHaveBeenCalled();
    });
  });
});
