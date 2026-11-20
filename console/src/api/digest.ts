/**
 * HTTP Digest (RFC 7616) with `qop=auth`, for the routes a subscriber signs
 * with its SIP credentials. The node offers SHA-256 then MD5; SHA-256 needs
 * WebCrypto, which a browser gives only a secure context, so a console served
 * over plain http answers with MD5.
 */

export interface DigestChallenge {
  realm: string;
  nonce: string;
  /** `MD5` when the challenge names none, as RFC 7616 3.3 says. */
  algorithm: string;
  qop: string[];
  opaque?: string;
  /** The nonce had expired: the credentials were right, so answer the new one. */
  stale: boolean;
}

/** Lowercase hex of a hash of `text`, by algorithm name. */
export type DigestHashes = Partial<Record<string, (text: string) => Promise<string>>>;

/**
 * Every Digest challenge in a `WWW-Authenticate` value. `fetch` joins several
 * headers into one with commas, so a value can hold more than one challenge;
 * other schemes are skipped.
 */
export function parseChallenges(header: string): DigestChallenge[] {
  const challenges: Array<Record<string, string>> = [];
  let current: Record<string, string> | undefined;
  let at = 0;
  const token = /[A-Za-z0-9!#$%&'*+.^_`|~-]+/y;
  while (at < header.length) {
    while (at < header.length && /[\s,]/.test(header[at])) at += 1;
    token.lastIndex = at;
    const name = token.exec(header)?.[0];
    if (!name) { at += 1; continue; }
    at += name.length;
    while (header[at] === ' ' || header[at] === '\t') at += 1;
    if (header[at] !== '=') {
      current = name.toLowerCase() === 'digest' ? {} : undefined;
      if (current) challenges.push(current);
      continue;
    }
    at += 1;
    while (header[at] === ' ' || header[at] === '\t') at += 1;
    let value = '';
    if (header[at] === '"') {
      at += 1;
      while (at < header.length && header[at] !== '"') {
        if (header[at] === '\\') at += 1;
        value += header[at] ?? '';
        at += 1;
      }
      at += 1;
    } else {
      token.lastIndex = at;
      value = token.exec(header)?.[0] ?? '';
      at += value.length;
    }
    if (current) current[name.toLowerCase()] = value;
  }
  return challenges
    .filter((params) => params.realm !== undefined && params.nonce !== undefined)
    .map((params) => ({
      realm: params.realm,
      nonce: params.nonce,
      algorithm: params.algorithm ?? 'MD5',
      qop: (params.qop ?? '').split(',').map((entry) => entry.trim()).filter(Boolean),
      ...(params.opaque !== undefined ? { opaque: params.opaque } : {}),
      stale: params.stale?.toLowerCase() === 'true',
    }));
}

export interface DigestRequest {
  username: string;
  password: string;
  method: string;
  /** The request target exactly as sent: path and query. */
  uri: string;
  /** Fixed in tests; random otherwise. */
  cnonce?: string;
}

/**
 * The `Authorization` value answering the first challenge offered with
 * `qop=auth` and an algorithm in `hashes`, or undefined when there is none.
 * Each value is answered once, so the nonce count is always 1.
 */
export async function authorization(challenges: DigestChallenge[], request: DigestRequest, hashes: DigestHashes): Promise<string | undefined> {
  const challenge = challenges.find((candidate) => candidate.qop.includes('auth') && hashes[candidate.algorithm]);
  if (!challenge) return undefined;
  const hash = hashes[challenge.algorithm]!;
  const nc = '00000001';
  const cnonce = request.cnonce ?? randomHex(16);
  const ha1 = await hash(`${request.username}:${challenge.realm}:${request.password}`);
  const ha2 = await hash(`${request.method}:${request.uri}`);
  const response = await hash(`${ha1}:${challenge.nonce}:${nc}:${cnonce}:auth:${ha2}`);
  const quoted = (value: string) => `"${value.replace(/[\\"]/g, '\\$&')}"`;
  return [
    `Digest username=${quoted(request.username)}`,
    `realm=${quoted(challenge.realm)}`,
    `nonce=${quoted(challenge.nonce)}`,
    `uri=${quoted(request.uri)}`,
    `algorithm=${challenge.algorithm}`,
    'qop=auth',
    `nc=${nc}`,
    `cnonce=${quoted(cnonce)}`,
    `response=${quoted(response)}`,
    ...(challenge.opaque !== undefined ? [`opaque=${quoted(challenge.opaque)}`] : []),
  ].join(', ');
}

/**
 * The hashes this runtime has: SHA-256 where WebCrypto is available, and MD5
 * always, from JsSIP, which the phone loads anyway and computes SIP Digest with.
 */
export function digestHashes(): DigestHashes {
  const subtle = globalThis.crypto?.subtle;
  return {
    ...(subtle ? {
      'SHA-256': async (text: string) => hex(new Uint8Array(await subtle.digest('SHA-256', new TextEncoder().encode(text)))),
    } : {}),
    MD5: async (text: string) => (await import('jssip')).default.Utils.calculateMD5(text),
  };
}

function randomHex(bytes: number): string {
  return hex(globalThis.crypto.getRandomValues(new Uint8Array(bytes)));
}

function hex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
}
