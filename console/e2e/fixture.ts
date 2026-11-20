/**
 * Where the fixture is and who is provisioned on it, from the environment
 * `test/interop/up.sh` in the server's checkout exports. Defaults are the
 * fixture's own.
 */
export interface Fixture {
  /** Where the softphone page is opened from. Loopback, so the origin is a secure context. */
  pageUrl: string;
  /** The node's API, on loopback. */
  apiUrl: string;
  socket: string;
  /** The address the node advertises. */
  publicAddress: string;
  /** The address rtpengine advertises for media: the public address unless overridden. */
  advertise: string;
  /**
   * Set when the engine advertises an address other than the node's, which
   * only the TURN server can reach: the relay phase. A relayed pair's local
   * port falls inside this range.
   */
  relay?: { min: number; max: number };
  /**
   * The administrator the fixture makes, `ATHENA_INTEROP_API_USER` and
   * `_API_PASSWORD`, for the specs that drive the API. `up.sh` generates the
   * password per run, so there is no default.
   */
  apiUser?: { username: string; password: string };
  /**
   * Set when the suite runner (`npm run test:athenasip`) calls: the phase it
   * brought the fixture up for, from `ATHENA_SUITE_PHASE`.
   */
  phase?: 'direct' | 'relay';
  /** A person or a device is present, so specs that need one run: `ATHENA_SUITE_DEVICE=1`. */
  device: boolean;
  realm: string;
  /** The first two for a browser calling a browser; the first alone when a browser calls a phone. Others are AthenaPhone's. */
  subscribers: string[];
  password: string;
  resultsDir: string;
  /**
   * A phone to call, as a SIP URI: `ATHENA_INTEROP_TARGET`. When set,
   * `phone-call.spec.ts` runs and the browser-to-browser spec is skipped.
   */
  target?: string;
  /** Accept the node's certificate unverified, for a snakeoil CA: `ATHENA_INTEROP_IGNORE_TLS=1`. */
  ignoreTls: boolean;
  /** How long the phone has to answer, `ATHENA_INTEROP_ANSWER_SECONDS`, 45 by default. */
  answerSeconds: number;
  /** How long media runs before its counters are read, `ATHENA_INTEROP_MEDIA_SECONDS`, 5 by default. */
  mediaSeconds: number;
  /** Video frames the browser must decode from the phone in that time, `ATHENA_INTEROP_MIN_FRAMES`, 30 by default. */
  minFrames: number;
}

function seconds(value: string | undefined, fallback: number, name: string): number {
  if (value === undefined || value === '') return fallback;
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < 0) throw new Error(`${name} is ${value}, which is not a number of seconds`);
  return parsed;
}

export function fixtureFromEnvironment(env: NodeJS.ProcessEnv = process.env): Fixture {
  const apiPort = env.ATHENA_INTEROP_API_PORT ?? '8080';
  const wsPort = env.ATHENA_INTEROP_WS_PORT ?? '8088';
  const publicAddress = env.ATHENA_INTEROP_PUBLIC_ADDRESS ?? '127.0.0.1';
  const subscribers = (env.ATHENA_INTEROP_SUBSCRIBERS ?? '1001,1002').split(',').map((subscriber) => subscriber.trim()).filter(Boolean);
  if (subscribers.length === 0) throw new Error('ATHENA_INTEROP_SUBSCRIBERS names nobody to register as');
  const advertise = env.ATHENA_INTEROP_RTPENGINE_ADVERTISE || publicAddress;
  let relay: Fixture['relay'];
  if (advertise !== publicAddress) {
    const min = Number(env.ATHENA_INTEROP_TURN_MIN);
    const max = Number(env.ATHENA_INTEROP_TURN_MAX);
    if (!Number.isInteger(min) || !Number.isInteger(max) || min > max) {
      throw new Error(`The engine advertises ${advertise}, which only TURN can reach, but ATHENA_INTEROP_TURN_MIN and _MAX do not give a relay range`);
    }
    relay = { min, max };
  }
  const given = env.ATHENA_SUITE_PHASE || undefined;
  if (given !== undefined && given !== 'direct' && given !== 'relay') throw new Error(`ATHENA_SUITE_PHASE is ${given}, not direct or relay`);
  const phase = given as Fixture['phase'];
  if (phase === 'relay' && !relay) throw new Error('ATHENA_SUITE_PHASE is relay, but the engine advertises the public address, so nothing needs TURN');
  if (phase === 'direct' && relay) throw new Error(`ATHENA_SUITE_PHASE is direct, but the engine advertises ${advertise}, which only TURN can reach`);
  const suiteResults = env.ATHENA_SUITE_RESULTS && phase ? `${env.ATHENA_SUITE_RESULTS.replace(/\/+$/, '')}/admin` : undefined;
  return {
    pageUrl: (env.ATHENA_INTEROP_PAGE_URL ?? `http://127.0.0.1:${apiPort}`).replace(/\/+$/, ''),
    apiUrl: `http://127.0.0.1:${apiPort}/api/v1`,
    socket: env.ATHENA_INTEROP_WS_URL ?? `ws://127.0.0.1:${wsPort}`,
    publicAddress,
    advertise,
    relay,
    apiUser: env.ATHENA_INTEROP_API_USER && env.ATHENA_INTEROP_API_PASSWORD
      ? { username: env.ATHENA_INTEROP_API_USER, password: env.ATHENA_INTEROP_API_PASSWORD }
      : undefined,
    phase,
    device: env.ATHENA_SUITE_DEVICE === '1' || env.ATHENA_SUITE_DEVICE === 'true',
    realm: env.ATHENA_INTEROP_REALM ?? publicAddress,
    subscribers,
    password: env.ATHENA_INTEROP_PASSWORD ?? 'athenaphone',
    resultsDir: suiteResults ?? env.ATHENA_INTEROP_RESULTS ?? 'e2e/results',
    target: env.ATHENA_INTEROP_TARGET || undefined,
    ignoreTls: env.ATHENA_INTEROP_IGNORE_TLS === '1' || env.ATHENA_INTEROP_IGNORE_TLS === 'true',
    answerSeconds: seconds(env.ATHENA_INTEROP_ANSWER_SECONDS, 45, 'ATHENA_INTEROP_ANSWER_SECONDS'),
    mediaSeconds: seconds(env.ATHENA_INTEROP_MEDIA_SECONDS, 5, 'ATHENA_INTEROP_MEDIA_SECONDS'),
    minFrames: seconds(env.ATHENA_INTEROP_MIN_FRAMES, 30, 'ATHENA_INTEROP_MIN_FRAMES'),
  };
}

export function sipUri(fixture: Fixture, user: string): string {
  return `sip:${user}@${fixture.realm}`;
}

/** The softphone page's URL, with the query string `pageOptions` reads. */
export function softphoneUrl(fixture: Fixture, user: string, options: { target?: string; answer?: boolean; ice?: unknown[]; video?: boolean }): string {
  const url = new URL('/softphone.html', `${fixture.pageUrl}/`);
  url.searchParams.set('ws', fixture.socket);
  url.searchParams.set('uri', sipUri(fixture, user));
  url.searchParams.set('password', fixture.password);
  url.searchParams.set('register', '1');
  if (options.target) url.searchParams.set('target', options.target);
  if (options.answer) url.searchParams.set('answer', '1');
  if (options.ice) url.searchParams.set('ice', JSON.stringify(options.ice));
  if (fixture.relay) url.searchParams.set('relay', '1');
  if (options.video) url.searchParams.set('video', '1');
  return url.toString();
}
