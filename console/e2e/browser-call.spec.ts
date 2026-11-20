import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { expect, test, type Page } from '@playwright/test';
import { fixtureFromEnvironment, sipUri, softphoneUrl, type Fixture } from './fixture';
import { authorization, digestHashes, parseChallenges } from '../src/api/digest';
import type { MediaStats, SoftphoneState, Transition } from '../src/softphone/Softphone';
import type { SoftphoneReadout } from '../src/softphone/page';

/**
 * A browser calls a browser through AthenaSIP and rtpengine, with no phone.
 * The server's harness asserts on rtpengine's counters; this asserts on the
 * browsers' own.
 *
 * Every wait is a predicate over the page's readout, except the fixed pause
 * that lets media flow before its counters are read.
 */

declare global {
  interface Window { __athenaSoftphone?: SoftphoneReadout }
}

interface Record_ {
  test: string;
  fixture: Omit<Fixture, 'password' | 'apiUser'>;
  caller: EndRecord;
  callee: EndRecord;
}

interface EndRecord {
  user: string;
  state: SoftphoneState;
  history: readonly Transition[];
  stats?: MediaStats;
}

const fixture = fixtureFromEnvironment();

// Outside the suite, a phone target selects the phone run instead (`phone-call.spec.ts`).
test.skip(!!fixture.target && !fixture.phase, 'ATHENA_INTEROP_TARGET is set, so this is the phone run');

/**
 * In the relay phase, the `ice_servers` from `/subscriber/{realm}/config`, signed with the first
 * subscriber's SIP credentials and fetched here, so the page holds no API credential.
 */
let iceServers: unknown[] | undefined;

test.beforeAll(async () => {
  if (fixture.subscribers.length < 2) {
    throw new Error(`ATHENA_INTEROP_SUBSCRIBERS names ${fixture.subscribers.length} subscriber(s); a browser calling a browser needs two`);
  }
  let health: Response | undefined;
  try {
    health = await fetch(`${fixture.apiUrl}/health`);
  } catch {
    // Reported below.
  }
  if (!health?.ok) {
    throw new Error(
      `No AthenaSIP node is serving at ${fixture.apiUrl}. Bring the interop fixture up first: `
      + `(cd .. && test/interop/up.sh --rtpengine), and build this client so the node has a page to serve.`,
    );
  }
  if (fixture.relay) {
    const path = `/subscriber/${encodeURIComponent(fixture.realm)}/config`;
    const url = `${fixture.apiUrl}${path}`;
    const challenge = await fetch(url);
    expect(challenge.status, `GET ${path} unsigned answered ${challenge.status}, not a Digest challenge`).toBe(401);
    const signature = await authorization(parseChallenges(challenge.headers.get('WWW-Authenticate') ?? ''), {
      username: fixture.subscribers[0], password: fixture.password, method: 'GET', uri: new URL(url).pathname,
    }, digestHashes());
    expect(signature, `GET ${path} offered no Digest challenge this client can answer`).toBeDefined();
    const response = await fetch(url, { headers: { Authorization: signature! } });
    expect(response.ok, `GET ${path} as ${fixture.subscribers[0]} answered ${response.status}`).toBe(true);
    const config = await response.json() as { ice_servers?: Array<{ urls: string; credential?: string }> };
    iceServers = config.ice_servers ?? [];
    expect(iceServers.some((server) => /^turns?:/.test((server as { urls: string }).urls) && (server as { credential?: string }).credential), 'the node offers no TURN server with a credential').toBe(true);
  }
});

/** Every page a test opened, left after it whatever the outcome. */
const opened: Page[] = [];
test.afterEach(async () => {
  for (const page of opened.splice(0)) await leave(page);
});

test('the first subscriber calls the second, and the caller hangs up', async ({ browser }, info) => {
  const [callerUser, calleeUser] = fixture.subscribers;
  const callee = await open(browser, calleeUser, { answer: true });
  const caller = await open(browser, callerUser, { target: sipUri(fixture, calleeUser) });

  await caller.page.getByTestId('softphone-call').click();
  await connected(caller.page);
  await connected(callee.page);

  await mediaFlowed(caller.page, callee.page);
  await writeRecord(info.title, caller, callee);

  await caller.page.getByTestId('softphone-hangup').click();
  await ended(caller.page);
  await ended(callee.page);
});

test('the second subscriber calls the first, and the callee hangs up', async ({ browser }, info) => {
  const [calleeUser, callerUser] = fixture.subscribers;
  const callee = await open(browser, calleeUser, {});
  const caller = await open(browser, callerUser, { target: sipUri(fixture, calleeUser) });

  await caller.page.evaluate((target) => window.__athenaSoftphone!.call(target), sipUri(fixture, calleeUser));
  await callee.page.waitForFunction(() => window.__athenaSoftphone?.state().call === 'incoming');
  await callee.page.getByTestId('softphone-answer').click();
  await connected(caller.page);
  await connected(callee.page);

  await mediaFlowed(caller.page, callee.page);
  await writeRecord(info.title, caller, callee);

  await callee.page.getByTestId('softphone-hangup').click();
  await ended(callee.page);
  await ended(caller.page);
});

interface End {
  user: string;
  page: Page;
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

/** A registered softphone in its own context, so the two share no socket or media stream. */
async function open(browser: Parameters<Parameters<typeof test>[2]>[0]['browser'], user: string, options: { target?: string; answer?: boolean }): Promise<End> {
  const context = await browser.newContext();
  const page = await context.newPage();
  opened.push(page);
  page.on('pageerror', (error) => { throw error; });
  await page.goto(softphoneUrl(fixture, user, { ...options, ice: iceServers }));
  await page.waitForFunction(() => {
    const state = window.__athenaSoftphone?.state();
    return state?.registration === 'registered' || state?.registration === 'failed';
  });
  const state = await readState(page);
  expect(state.registration, `${user}: ${state.notice ?? 'registration failed'}`).toBe('registered');
  return { user, page };
}

async function connected(page: Page): Promise<void> {
  await page.waitForFunction(() => {
    const state = window.__athenaSoftphone?.state();
    return state?.call === 'connected' || state?.call === 'failed';
  });
  const state = await readState(page);
  expect(state.call, state.notice ?? state.cause ?? 'call failed').toBe('connected');
  await page.waitForFunction(() => {
    const ice = window.__athenaSoftphone?.state().iceConnectionState;
    return ice === 'connected' || ice === 'completed';
  });
}

async function ended(page: Page): Promise<void> {
  await page.waitForFunction(() => window.__athenaSoftphone?.state().call === 'ended');
}

/**
 * Asserts media: packets both ways, sound rather than silence, DTLS complete.
 * When the engine advertises a non-loopback address, each browser must be
 * sending there, which shows the engine anchored the call. In the relay phase
 * the pair's local port must be in the TURN relay range; the candidate type
 * is not used, since Chrome reports a relayed pair as `prflx` once checks run.
 */
async function mediaFlowed(caller: Page, callee: Page): Promise<void> {
  await caller.waitForTimeout(2000);
  for (const [name, page] of [['caller', caller], ['callee', callee]] as const) {
    const stats = await readStats(page);
    expect(stats, `${name} has no peer connection to read`).toBeDefined();
    expect(stats!.dtlsState, `${name} DTLS`).toBe('connected');
    expect(stats!.packetsSent, `${name} sent nothing`).toBeGreaterThan(0);
    expect(stats!.packetsReceived, `${name} received nothing`).toBeGreaterThan(0);
    // Energy, not the instantaneous level: the fake device beeps, and an
    // instant can land between beeps.
    expect(stats!.totalAudioEnergy ?? 0, `${name} received only silence`).toBeGreaterThan(0);
    expect(stats!.candidatePair, `${name} has no selected candidate pair`).toBeDefined();
    if (!fixture.advertise.startsWith('127.')) {
      expect(stats!.candidatePair!.remote.address, `${name} is not sending to the engine's advertised address`).toBe(fixture.advertise);
    }
    if (fixture.relay) {
      const port = stats!.candidatePair!.local.port;
      expect(port >= fixture.relay.min && port <= fixture.relay.max, `${name} sent from port ${port}, outside the relay range ${fixture.relay.min}-${fixture.relay.max}`).toBe(true);
    }
  }
}

function readState(page: Page): Promise<SoftphoneState> {
  return page.evaluate(() => window.__athenaSoftphone!.state());
}

function readStats(page: Page): Promise<MediaStats | undefined> {
  return page.evaluate(() => window.__athenaSoftphone!.stats());
}

/** Writes both descriptions, the state history and each end's counters to the results directory. */
async function writeRecord(title: string, caller: End, callee: End): Promise<void> {
  const end = async (side: End): Promise<EndRecord> => ({
    user: side.user,
    state: await readState(side.page),
    history: await side.page.evaluate(() => window.__athenaSoftphone!.history()),
    stats: await readStats(side.page),
  });
  const { password: _password, apiUser: _apiUser, ...visible } = fixture;
  const record: Record_ = { test: title, fixture: visible, caller: await end(caller), callee: await end(callee) };
  await mkdir(fixture.resultsDir, { recursive: true });
  const file = join(fixture.resultsDir, `${title.replace(/[^a-z0-9]+/gi, '-').toLowerCase()}.json`);
  await writeFile(file, JSON.stringify(record, null, 2));
}
