import { describe, expect, it } from 'vitest';
import { ApiError } from '../api/errors';
import { describeRefusal } from './LoginScreen';

describe('describeRefusal', () => {
  it('gives one answer for every refused password, as the node does', () => {
    expect(describeRefusal(new ApiError('no', 401, 'unauthorized'))).toBe('That username and password were not accepted.');
  });

  it('says a datastore outage is not a bad password', () => {
    const outage = new ApiError('the datastore could not be asked', 503, 'unavailable');
    expect(describeRefusal(outage)).toMatch(/cannot reach its datastore/);
  });

  it('says a node without logins cannot be signed in to, rather than offering another way', () => {
    expect(describeRefusal(new ApiError('not found', 404, 'not_found'))).toMatch(/predates user logins/);
  });

  it('says how long to wait when rate limited', () => {
    expect(describeRefusal(new ApiError('slow down', 429, undefined, 1))).toBe('Too many attempts. Try again in 1 second.');
  });
});
