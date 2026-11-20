import { randomBytes } from 'node:crypto';
import { expect, test, type Browser, type Page } from '@playwright/test';
import { HttpAdminApi } from '../src/api/HttpAdminApi';
import { ApiError } from '../src/api/errors';
import { authorization, digestHashes, parseChallenges } from '../src/api/digest';
import type { Role } from '../src/api/types';
import type { SoftphoneState } from '../src/softphone/Softphone';
import type { SoftphoneReadout } from '../src/softphone/page';
import { fixtureFromEnvironment, softphoneUrl } from './fixture';

/**
 * The console against a live node: its API client, provisioning, and its own
 * Phone driven in a browser, with the node's event stream watched alongside.
 *
 * The node allows five sign-ins a minute per address and one per username,
 * so the fixture's administrator signs in once, every other sign-in is a user
 * made for it, and a 429 is waited out. Everything made here is removed.
 */

declare global {
  interface Window { __athenaSoftphone?: SoftphoneReadout }
}

const fixture = fixtureFromEnvironment();
const base = fixture.apiUrl.replace(/\/api\/v1$/, '');
const run = randomBytes(3).toString('hex');
const secret = () => `suite-${randomBytes(12).toString('hex')}`;

test.skip(!fixture.apiUser, 'ATHENA_INTEROP_API_USER and _API_PASSWORD are not set; up.sh writes both into generated/fixture.env');
test.describe.configure({ mode: 'serial', timeout: 180_000 });

let admin: HttpAdminApi;
/** The administrator's token, also for the event stream: a second sign-in would wait out the per-username limit. */
let adminToken: string;
const users: string[] = [];
const realms: string[] = [];

/** Signs in, waiting out the node's rate limit at most twice. */
async function signIn(username: string, password: string): Promise<string> {
  const anonymous = new HttpAdminApi({ baseUrl: base });
  for (let attempt = 0; ; attempt += 1) {
    try {
      return (await anonymous.login(username, password)).token;
    } catch (cause) {
      if (!(cause instanceof ApiError) || cause.status !== 429 || attempt >= 2) throw cause;
      await new Promise((resolve) => setTimeout(resolve, (cause.retryAfter ?? 60) * 1000));
    }
  }
}

function as(token: string): HttpAdminApi {
  return new HttpAdminApi({ baseUrl: base, token: () => token });
}

async function makeUser(roles: Role[]): Promise<{ username: string; password: string }> {
  const user = { username: `suite-${run}-${users.length}`, password: secret() };
  await admin.createUser({ ...user, display_name: 'Suite', roles });
  users.push(user.username);
  return user;
}

async function failure(promise: Promise<unknown>): Promise<number | undefined> {
  return promise.then(() => undefined, (cause: unknown) => (cause instanceof ApiError ? cause.status : -1));
}

interface StreamEvent { name: string; data: string }

/** `GET /events`, read as it arrives. `fetch` rather than `EventSource`, which cannot send the bearer. */
function events(token: string) {
  const seen: StreamEvent[] = [];
  const controller = new AbortController();
  const opened = fetch(`${fixture.apiUrl}/events`, { headers: { Authorization: `Bearer ${token}` }, signal: controller.signal })
    .then(async (response) => {
      expect(response.status, 'GET /events').toBe(200);
      expect(response.headers.get('Content-Type') ?? '').toContain('text/event-stream');
      const reader = response.body!.getReader();
      const decoder = new TextDecoder();
      let buffer = '';
      void (async () => {
        for (;;) {
          const { done, value } = await reader.read().catch(() => ({ done: true, value: undefined }));
          if (done) return;
          buffer += decoder.decode(value, { stream: true });
          let end: number;
          while ((end = buffer.indexOf('\n\n')) >= 0) {
            const block = buffer.slice(0, end);
            buffer = buffer.slice(end + 2);
            const name = /^event: ?(.*)$/m.exec(block)?.[1];
            const data = [...block.matchAll(/^data: ?(.*)$/gm)].map((line) => line[1]).join('\n');
            if (name) seen.push({ name, data });
          }
        }
      })();
    });
  return {
    opened,
    async expect(pattern: RegExp, what: string) {
      await expect.poll(() => seen.some((event) => pattern.test(event.name)), { message: `no ${what} event on /events`, timeout: 20_000 }).toBe(true);
    },
    close: () => controller.abort(),
  };
}

/** `GET /subscriber/{realm}/config`, signed from here, for a harness page's ICE servers. */
async function iceFor(user: string): Promise<unknown[] | undefined> {
  if (!fixture.relay) return undefined;
  const line = { realm: fixture.realm, user, password: fixture.password };
  return (await new HttpAdminApi({ baseUrl: base }).subscriberConfig(line)).ice_servers;
}

test.beforeAll(async () => {
  adminToken = await signIn(fixture.apiUser!.username, fixture.apiUser!.password);
  admin = as(adminToken);
});

test.afterAll(async () => {
  for (const realm of realms) await admin.deleteRealm(realm).catch(() => undefined);
  for (const username of users) await admin.deleteUser(username).catch(() => undefined);
  await admin?.logout().catch(() => undefined);
});

test('a user signs in, gets what its roles allow, and a signed-out token opens nothing', async () => {
  const user = await makeUser(['view-cluster-status']);
  const token = await signIn(user.username, user.password);
  const api = as(token);

  expect(await api.session()).toMatchObject({ username: user.username, roles: ['view-cluster-status'] });
  expect((await api.nodes()).some((node) => node.self)).toBe(true);
  expect(await failure(api.listRealms()), 'listing realms without Manage realms').toBe(403);
  expect(await failure(signIn(`suite-${run}-nobody`, 'wrong')), 'signing in as nobody').toBe(401);

  await api.logout();
  expect(await failure(new HttpAdminApi({ baseUrl: base }).session(token)), 'the signed-out token').toBe(401);
});

test('a realm and subscriber are provisioned, and the subscriber signs its own config with Digest', async () => {
  const realm = `suite-${run}.invalid`;
  await admin.createRealm({ name: realm });
  realms.push(realm);
  const first = secret();
  await admin.createSubscriber(realm, { user: 'alice', password: first });
  expect((await admin.listSubscribers(realm)).map((subscriber) => subscriber.user)).toEqual(['alice']);

  const config = await new HttpAdminApi({ baseUrl: base }).subscriberConfig({ realm, user: 'alice', password: first });
  expect(config.realm?.name).toBe(realm);
  expect(Array.isArray(config.ice_servers)).toBe(true);
  expect(typeof config.realm?.registration.expires).toBe('number');

  const signed = (password: string) => failure(new HttpAdminApi({ baseUrl: base }).subscriberConfig({ realm, user: 'alice', password }));
  expect(await signed('wrong'), 'a wrong password').toBe(401);

  // A user's bearer opens no subscriber route.
  const bearer = await fetch(`${fixture.apiUrl}/subscriber/${realm}/config`, { headers: { Authorization: 'Bearer nothing' } });
  expect(bearer.status).toBe(401);

  // MD5 as well as SHA-256, as a console served over plain http answers.
  const url = `${fixture.apiUrl}/subscriber/${realm}/config`;
  const challenge = await fetch(url);
  const { MD5 } = digestHashes();
  const md5 = await authorization(parseChallenges(challenge.headers.get('WWW-Authenticate') ?? ''), {
    username: 'alice', password: first, method: 'GET', uri: new URL(url).pathname,
  }, { MD5 });
  expect(md5).toContain('algorithm=MD5');
  expect((await fetch(url, { headers: { Authorization: md5! } })).status, 'an MD5 answer').toBe(200);

  const second = secret();
  await admin.updateSubscriber(realm, 'alice', { password: second });
  expect(await signed(first), 'the old password').toBe(401);
  expect(await signed(second), 'the new password').toBeUndefined();
  expect(await admin.listRegistrations(realm)).toEqual([]);

  await admin.deleteRealm(realm);
  realms.splice(realms.indexOf(realm), 1);
  expect(await signed(second), 'a deleted realm').toBe(404);
});

test("the console's Phone registers from the node's signed config and calls, for a user with no roles", async ({ browser }) => {
  const [lineUser, calleeUser] = fixture.subscribers;
  test.skip(!calleeUser, 'ATHENA_INTEROP_SUBSCRIBERS names one subscriber; the Phone needs another to call');
  const stream = events(adminToken);
  await stream.opened;
  const user = await makeUser([]);
  const console_ = await signedInConsole(browser, user);
  let callee: Page | undefined;
  try {
    // A user with no roles has no navigation, only the No permissions screen's link.
    await console_.getByRole('link', { name: /^phone$/i }).first().click();
    await console_.getByLabel('SIP address').fill(`sip:${lineUser}@${fixture.realm}`);
    await console_.getByLabel('Password').fill(fixture.password);
    expect(await console_.getByLabel('WebSocket').inputValue(), 'a new browser remembers no WebSocket').toBe('');
    await console_.getByRole('button', { name: 'Register' }).click();
    const registered = console_.getByRole('heading', { name: `Registered as ${lineUser}` });
    await expect(registered.or(console_.getByRole('alert'))).toBeVisible({ timeout: 20_000 });
    if (!await registered.isVisible()) throw new Error(await alertText(console_));
    await stream.expect(new RegExp(`^subscribers/sips?:${lineUser}@[^/]+/status$`), `${lineUser}'s registration`);

    callee = await harness(browser, calleeUser);
    await console_.getByLabel('Number or address').fill(calleeUser);
    await console_.getByRole('button', { name: 'Call', exact: true }).click();
    await callee.waitForFunction(() => {
      const state = window.__athenaSoftphone?.state();
      return state?.call === 'connected' || state?.call === 'failed';
    }, undefined, { timeout: 30_000 });
    const answered = callee;
    const state = await answered.evaluate(() => window.__athenaSoftphone!.state()) as SoftphoneState;
    expect(state.call, state.notice ?? state.cause ?? 'the call failed').toBe('connected');
    await expect(console_.getByRole('button', { name: 'Hang up' })).toBeVisible();
    await stream.expect(/^calls\/.+\/state$/, 'call state');

    await answered.waitForTimeout(2000);
    const stats = await answered.evaluate(() => window.__athenaSoftphone!.stats());
    expect(stats?.packetsReceived ?? 0, 'the callee received nothing from the console').toBeGreaterThan(0);
    expect(stats?.totalAudioEnergy ?? 0, 'the callee received only silence from the console').toBeGreaterThan(0);

    await console_.getByRole('button', { name: 'Hang up' }).click();
    await answered.waitForFunction(() => window.__athenaSoftphone?.state().call === 'ended');
    await console_.getByRole('button', { name: 'Sign out of the line' }).click();
  } finally {
    stream.close();
    if (callee) await leave(callee);
    await console_.context().close();
  }
});

/** The console in its own context, signed in. Fails at once if `build/` is not a live build, which never asks the node. */
async function signedInConsole(browser: Browser, user: { username: string; password: string }): Promise<Page> {
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.goto(`${fixture.pageUrl}/`);
  await page.getByLabel('Username').fill(user.username);
  await page.getByLabel('Password').fill(user.password);
  const login = page.waitForResponse((response) => response.url().includes('/api/v1/auth/login'), { timeout: 10_000 }).catch(() => undefined);
  await page.getByRole('button', { name: 'Sign in' }).click();
  const response = await login;
  expect(response, 'the console never asked the node to sign in: build/ was not built with VITE_ATHENASIP_LIVE=true').toBeDefined();
  expect(response!.status(), 'the console sign-in').toBe(200);
  return page;
}

/** A harness page that registers and answers, in its own context. */
async function harness(browser: Browser, user: string): Promise<Page> {
  const page = await (await browser.newContext()).newPage();
  await page.goto(softphoneUrl(fixture, user, { answer: true, ice: await iceFor(user) }));
  await page.waitForFunction(() => {
    const state = window.__athenaSoftphone?.state();
    return state?.registration === 'registered' || state?.registration === 'failed';
  });
  const state = await page.evaluate(() => window.__athenaSoftphone!.state()) as SoftphoneState;
  expect(state.registration, `${user} on the harness page: ${state.notice ?? 'registration failed'}`).toBe('registered');
  return page;
}

async function alertText(page: Page): Promise<string> {
  return `the Phone did not register${await page.getByRole('alert').first().textContent({ timeout: 1000 }).then((text) => `: ${text}`, () => '')}`;
}

/** Unregisters a harness page, so it answers nothing in a later test, and closes its context. */
async function leave(page: Page): Promise<void> {
  const unregister = page.getByTestId('softphone-unregister');
  if (await unregister.isVisible().catch(() => false)) {
    await unregister.click();
    await page.waitForFunction(() => window.__athenaSoftphone?.state().registration !== 'registered', undefined, { timeout: 5000 }).catch(() => undefined);
  }
  await page.context().close();
}
