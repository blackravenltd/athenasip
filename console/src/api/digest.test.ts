import { describe, expect, it } from 'vitest';
import { authorization, digestHashes, parseChallenges } from './digest';

// RFC 7616 3.9.1.
const RFC = {
  username: 'Mufasa',
  password: 'Circle of Life',
  method: 'GET',
  uri: '/dir/index.html',
  cnonce: 'f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ',
};
const NONCE = '7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v';
const OPAQUE = 'FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS';
const HEADER = `Digest realm="http-auth@example.org", qop="auth, auth-int", algorithm=SHA-256, nonce="${NONCE}", opaque="${OPAQUE}", `
  + `Digest realm="http-auth@example.org", qop="auth, auth-int", algorithm=MD5, nonce="${NONCE}", opaque="${OPAQUE}"`;

describe('parseChallenges', () => {
  it('reads both challenges from one joined header, in order', () => {
    const challenges = parseChallenges(HEADER);
    expect(challenges.map((challenge) => challenge.algorithm)).toEqual(['SHA-256', 'MD5']);
    expect(challenges[0]).toEqual({
      realm: 'http-auth@example.org', nonce: NONCE, algorithm: 'SHA-256', qop: ['auth', 'auth-int'], opaque: OPAQUE, stale: false,
    });
  });

  it('skips other schemes, unescapes quoted values and reads stale', () => {
    const challenges = parseChallenges('Basic realm="x", Digest realm="a\\"b", nonce=n1, qop="auth", stale=TRUE');
    expect(challenges).toEqual([{ realm: 'a"b', nonce: 'n1', algorithm: 'MD5', qop: ['auth'], stale: true }]);
  });
});

describe('authorization', () => {
  it('answers SHA-256 as RFC 7616 does', async () => {
    const value = await authorization(parseChallenges(HEADER), RFC, digestHashes());
    expect(value).toContain('algorithm=SHA-256');
    expect(value).toContain('response="753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1"');
    expect(value).toContain(`opaque="${OPAQUE}"`);
    expect(value).toContain('uri="/dir/index.html", algorithm=SHA-256, qop=auth, nc=00000001');
  });

  it('answers MD5 as RFC 7616 does, where SHA-256 is not available', async () => {
    const { MD5 } = digestHashes();
    const value = await authorization(parseChallenges(HEADER), RFC, { MD5 });
    expect(value).toContain('algorithm=MD5');
    expect(value).toContain('response="8ca523f5e9506fed4657c9700eebdbec"');
  });

  it('answers nothing without qop=auth or a known algorithm', async () => {
    expect(await authorization(parseChallenges('Digest realm="r", nonce="n"'), RFC, digestHashes())).toBeUndefined();
    expect(await authorization(parseChallenges('Digest realm="r", nonce="n", qop="auth", algorithm=SHA-512-256'), RFC, digestHashes())).toBeUndefined();
  });
});
