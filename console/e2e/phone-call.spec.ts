import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { expect, test, type Page } from '@playwright/test';
import { fixtureFromEnvironment, softphoneUrl, type Fixture } from './fixture';
import type { MediaStats, SoftphoneState, Transition } from '../src/softphone/Softphone';
import type { SoftphoneReadout, VideoNegotiation } from '../src/softphone/page';

/**
 * A browser calls a phone with video, through a real AthenaSIP node.
 *
 * One page registers and calls `ATHENA_INTEROP_TARGET`, a device that answers
 * by itself, sending Chromium's fake camera and microphone. It asserts ICE
 * and DTLS connected, audio packets both ways, video sent, and frames decoded
 * from the phone. The phone's audio need not carry sound: it may be silent.
 *
 * The server's run provides the node, its engine and the phone, sets the
 * environment (see `docs/softphone.md`) and serves `build/`.
 */

declare global {
  interface Window { __athenaSoftphone?: SoftphoneReadout }
}

const fixture = fixtureFromEnvironment();

// In the suite it needs a device present as well as named.
test.skip(!!fixture.phase && !fixture.device, 'needs a phone: ATHENA_SUITE_DEVICE is not set');
test.skip(!fixture.target, 'ATHENA_INTEROP_TARGET names no phone to call');

test.use({
  ignoreHTTPSErrors: fixture.ignoreTls,
  // `ignoreHTTPSErrors` covers the page only; the WebSocket needs the browser flag.
  launchOptions: {
    args: [
      '--use-fake-device-for-media-stream',
      '--use-fake-ui-for-media-stream',
      '--autoplay-policy=no-user-gesture-required',
      ...(fixture.ignoreTls ? ['--ignore-certificate-errors'] : []),
    ],
  },
});

test('a browser calls the phone with video, and hangs up', async ({ browser }, info) => {
  test.setTimeout((fixture.answerSeconds + fixture.mediaSeconds + 60) * 1000);
  const [user] = fixture.subscribers;
  const page_ = new URL(fixture.pageUrl);
  if (page_.protocol !== 'https:' && !['127.0.0.1', 'localhost', '[::1]'].includes(page_.hostname)) {
    throw new Error(`ATHENA_INTEROP_PAGE_URL is ${fixture.pageUrl}: a browser gives a plain-http page on another host no microphone, so no call can start. Use the node's https listener (such as https://${page_.hostname}:8443) with ATHENA_INTEROP_IGNORE_TLS=1 for a snakeoil certificate.`);
  }
  const context = await browser.newContext({ ignoreHTTPSErrors: fixture.ignoreTls });
  const page = await context.newPage();
  page.on('pageerror', (error) => { throw error; });
  await page.goto(softphoneUrl(fixture, user, { target: fixture.target, video: true }));

  await page.waitForFunction(() => {
    const state = window.__athenaSoftphone?.state();
    return state?.registration === 'registered' || state?.registration === 'failed';
  }, undefined, { timeout: 30_000 });
  const registration = await readState(page);
  expect(registration.registration, `${user}: ${registration.notice ?? 'registration failed'}${fixture.socket.startsWith('wss:') && !fixture.ignoreTls ? ' (a snakeoil certificate needs ATHENA_INTEROP_IGNORE_TLS=1)' : ''}`).toBe('registered');

  await page.getByTestId('softphone-call').click();
  await page.waitForFunction(() => {
    const call = window.__athenaSoftphone?.state().call;
    return call === 'connected' || call === 'failed' || call === 'ended';
  }, undefined, { timeout: fixture.answerSeconds * 1000 });
  const answered = await readState(page);
  if (answered.call !== 'connected') await writeRecord(info.title, page, user, await readStats(page), await page.evaluate(() => window.__athenaSoftphone!.video()));
  expect(answered.call, `the call to ${fixture.target}: ${answered.notice ?? answered.cause ?? 'not answered'}`).toBe('connected');
  await page.waitForFunction(() => {
    const ice = window.__athenaSoftphone?.state().iceConnectionState;
    return ice === 'connected' || ice === 'completed';
  }, undefined, { timeout: 15_000 });

  await page.waitForTimeout(fixture.mediaSeconds * 1000);
  const stats = await readStats(page);
  const video = await page.evaluate(() => window.__athenaSoftphone!.video());
  await writeRecord(info.title, page, user, stats, video);

  expect(stats, 'no peer connection to read').toBeDefined();
  expect(stats!.dtlsState, 'DTLS').toBe('connected');
  expect(stats!.packetsSent, 'sent no audio').toBeGreaterThan(0);
  expect(stats!.packetsReceived, 'received no audio from the phone').toBeGreaterThan(0);
  expect(['accepted', 'bundled'], `the phone ${video.outcome} the video line`).toContain(video.outcome);
  expect(stats!.video, 'the call carries no video').toBeDefined();
  expect(stats!.video!.packetsSent, 'sent no video').toBeGreaterThan(0);
  expect(stats!.video!.framesDecoded, `decoded ${stats!.video!.framesDecoded} frames from the phone in ${fixture.mediaSeconds}s`).toBeGreaterThanOrEqual(fixture.minFrames);
  expect(stats!.candidatePair, 'no selected candidate pair').toBeDefined();
  // Only when the run says where the engine is: the browser must be sending there.
  if (process.env.ATHENA_INTEROP_RTPENGINE_ADVERTISE) {
    expect(stats!.candidatePair!.remote.address, 'not sending to the engine\'s advertised address').toBe(fixture.advertise);
  }

  await page.getByTestId('softphone-hangup').click();
  await page.waitForFunction(() => window.__athenaSoftphone?.state().call === 'ended', undefined, { timeout: 15_000 });
  await context.close();
});

function readState(page: Page): Promise<SoftphoneState> {
  return page.evaluate(() => window.__athenaSoftphone!.state());
}

function readStats(page: Page): Promise<MediaStats | undefined> {
  return page.evaluate(() => window.__athenaSoftphone!.stats());
}

interface Record_ {
  test: string;
  fixture: Omit<Fixture, 'password' | 'apiUser'>;
  user: string;
  state: SoftphoneState;
  history: readonly Transition[];
  stats?: MediaStats;
  video: VideoNegotiation;
}

/** Written before the assertions, so a failed call leaves its descriptions and counters behind. */
async function writeRecord(title: string, page: Page, user: string, stats: MediaStats | undefined, video: VideoNegotiation): Promise<void> {
  const { password: _password, apiUser: _apiUser, ...visible } = fixture;
  const record: Record_ = {
    test: title,
    fixture: visible,
    user,
    state: await readState(page),
    history: await page.evaluate(() => window.__athenaSoftphone!.history()),
    stats,
    video,
  };
  await mkdir(fixture.resultsDir, { recursive: true });
  await writeFile(join(fixture.resultsDir, `${title.replace(/[^a-z0-9]+/gi, '-').toLowerCase()}.json`), JSON.stringify(record, null, 2));
}
