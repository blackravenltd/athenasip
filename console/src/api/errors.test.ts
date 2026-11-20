import { describe, expect, it } from 'vitest';
import { ApiError, DEFAULT_RETRY_AFTER_S, errorMessage, isAbort, isConflict, retryAfter } from './errors';

describe('errorMessage', () => {
  it('uses the server message when there is one', () => {
    expect(errorMessage(new ApiError('realm sip.example.org already exists', 409, 'conflict')))
      .toBe('realm sip.example.org already exists');
  });

  it('reads an ordinary Error, which is what an unreachable server produces', () => {
    expect(errorMessage(new TypeError('Failed to fetch'))).toBe('Failed to fetch');
  });

  it('says how long to wait when the node is rate limiting', () => {
    expect(errorMessage(new ApiError('too many requests', 429, undefined, 30))).toBe('The node is limiting requests. Try again in 30 seconds.');
    expect(errorMessage(new ApiError('too many requests', 429))).toBe('The node is limiting requests. Wait a little and try again.');
  });

  it('never renders [object Object] at somebody trying to fix a server', () => {
    expect(errorMessage({ oops: true })).not.toContain('[object Object]');
  });
});

describe('ApiError', () => {
  it('tells an ended session from a token without the role', () => {
    // A 401 signs the console out; a 403 does not, because the token is fine
    // and only this screen is not for it.
    expect(new ApiError('no', 401, 'unauthorized').isUnauthorized).toBe(true);
    expect(new ApiError('no', 403, 'forbidden').isUnauthorized).toBe(false);
    expect(new ApiError('no', 403, 'forbidden').isForbidden).toBe(true);
  });
});

describe('retryAfter', () => {
  it('holds off for what the node asked, or a default, and only on a 429', () => {
    expect(retryAfter(new ApiError('slow', 429, undefined, 5))).toBe(5);
    expect(retryAfter(new ApiError('slow', 429))).toBe(DEFAULT_RETRY_AFTER_S);
    expect(retryAfter(new ApiError('down', 503, 'unavailable'))).toBeUndefined();
    expect(retryAfter(new TypeError('Failed to fetch'))).toBeUndefined();
  });
});

describe('isConflict', () => {
  it('recognises the server refusing a duplicate, by code or by status', () => {
    expect(isConflict(new ApiError('taken', 409, 'conflict'))).toBe(true);
    expect(isConflict(new ApiError('taken', 409))).toBe(true);
    expect(isConflict(new ApiError('missing', 404, 'not_found'))).toBe(false);
    expect(isConflict(new Error('taken'))).toBe(false);
  });
});

describe('isAbort', () => {
  it('recognises a cancelled request, which is ordinary and not a failure', () => {
    expect(isAbort(new DOMException('Aborted', 'AbortError'))).toBe(true);
    expect(isAbort(new Error('Aborted'))).toBe(false);
  });
});
