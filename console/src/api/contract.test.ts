import { existsSync, readFileSync } from 'node:fs';
import { describe, expect, it, vi } from 'vitest';
import { HttpAdminApi } from './HttpAdminApi';

/**
 * Every request `HttpAdminApi` makes is one the server's OpenAPI document
 * describes. The document lives in the server's checkout, beside this one;
 * without that checkout the suite skips.
 */
const DOCUMENT = new URL('../../../docs/api/openapi.yaml', import.meta.url);
const present = existsSync(DOCUMENT);

/** `path template -> methods`, read from the document's `paths:` block without a YAML parser. */
function operations(yaml: string): Map<string, Set<string>> {
  const result = new Map<string, Set<string>>();
  const block = yaml.slice(yaml.indexOf('\npaths:'), yaml.indexOf('\ncomponents:'));
  let current: Set<string> | undefined;
  for (const line of block.split('\n')) {
    const path = /^ {2}(\/\S*):\s*$/.exec(line);
    if (path) {
      current = new Set();
      result.set(path[1], current);
      continue;
    }
    const method = /^ {4}(get|put|post|delete|patch):\s*$/.exec(line);
    if (method && current) current.add(method[1].toUpperCase());
  }
  return result;
}

/**
 * Requests this client makes that the server's document does not describe
 * yet, as `METHOD /path`. The last test fails once the document has one.
 */
const PENDING = new Set<string>([]);

/** A request's path with its user segment put back to the template, to look it up in PENDING. */
function pendingKey(method: string, path: string): string {
  return `${method} ${path.replace(/^\/users\/[^/]+/, '/users/{username}')}`;
}

function matches(template: string, path: string): boolean {
  const pattern = template.replace(/\{[^}]+\}/g, '[^/]+');
  return new RegExp(`^${pattern}$`).test(path);
}

describe.skipIf(!present)('HttpAdminApi against the server OpenAPI document', () => {
  it('makes no request the document does not describe', async () => {
    const seen: Array<{ method: string; path: string }> = [];
    const fetch = vi.fn(async (url: string, init: RequestInit) => {
      seen.push({ method: init.method ?? 'GET', path: new URL(url, 'http://node').pathname.replace(/^\/api\/v1/, '') });
      return new Response('[]', { status: 200 });
    }) as unknown as typeof globalThis.fetch;
    const api = new HttpAdminApi({ fetch });

    await api.login('u', 'p');
    await api.logout();
    await api.session('t');
    await api.health();
    await api.nodes();
    await api.subscriberConfig({ realm: 'r', user: 'u', password: 'p' });
    await api.listRealms();
    await api.createRealm({ name: 'r' });
    await api.updateRealm('r', { behaviour: { media_anchor: false } });
    await api.deleteRealm('r');
    await api.listSubscribers('r');
    await api.createSubscriber('r', { user: 'u', password: 'p' });
    await api.updateSubscriber('r', 'u', { password: 'p', behaviour: { media_profile: null } });
    await api.deleteSubscriber('r', 'u');
    await api.listRegistrations('r');
    await api.listCalls();
    await api.listCallRecords(50);
    await api.getCall('a84b4c76e66710@pc33.atlanta.com');
    await api.mediaEngine();
    await api.listMediaReoffers();
    await api.listQualifiedClients();
    await api.listUsers();
    await api.createUser({ username: 'u', display_name: '', password: 'p', roles: [] });
    await api.updateUser('u', { roles: [] });
    await api.deleteUser('u');
    await api.changePassword('u', { password: 'p' });
    await api.revokeSessions('u');

    const described = operations(readFileSync(DOCUMENT, 'utf8'));
    expect(described.size).toBeGreaterThan(0);
    for (const request of seen) {
      if (PENDING.has(pendingKey(request.method, request.path))) continue;
      const template = [...described.keys()].find((candidate) => matches(candidate, decodeURIComponent(request.path)));
      expect(template, `${request.method} ${request.path} is not in the document`).toBeDefined();
      expect(described.get(template!), `${request.method} ${request.path}`).toContain(request.method);
    }
  });

  it('lists nothing as pending that the document now describes', () => {
    const described = operations(readFileSync(DOCUMENT, 'utf8'));
    for (const pending of PENDING) {
      const [method, path] = pending.split(' ');
      expect(described.get(path)?.has(method) ?? false, `${pending} is documented now; take it out of PENDING`).toBe(false);
    }
  });
});
