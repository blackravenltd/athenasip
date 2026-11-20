import { afterEach, describe, expect, it, vi } from 'vitest';
import type { SessionInfo } from '../api/types';
import { Session } from './Session';

const ops: SessionInfo = { username: 'ops', display_name: 'Operations', roles: ['view-cluster-status'] };

afterEach(() => { vi.useRealTimers(); });

describe('Session', () => {
  it('holds a token and who it is, and tells its listeners', () => {
    const session = new Session();
    const listener = vi.fn();
    session.subscribe(listener);
    session.signIn('t', ops);
    expect(session.token).toBe('t');
    expect(session.has('view-cluster-status')).toBe(true);
    expect(session.has('manage-realms')).toBe(false);
    expect(listener).toHaveBeenCalledOnce();
  });

  it('takes new roles mid-session, which is how a removed role reaches the navigation', () => {
    const session = new Session();
    session.signIn('t', ops);
    session.update({ ...ops, roles: [] });
    expect(session.roles).toEqual([]);
    expect(session.token).toBe('t');
  });

  it('forgets the token on sign out and keeps the reason for the sign-in screen', () => {
    const session = new Session();
    session.signIn('t', ops);
    session.signOut('Signed out: expired.');
    expect(session.state).toEqual({ ended: 'Signed out: expired.' });
  });

  it('ignores a second sign out, so a burst of 401s says one thing', () => {
    const session = new Session();
    const listener = vi.fn();
    session.signIn('t', ops);
    session.subscribe(listener);
    session.signOut('first');
    session.signOut('second');
    expect(listener).toHaveBeenCalledOnce();
    expect(session.state.ended).toBe('first');
  });

  it('ends itself at the expiry the node gave', () => {
    vi.useFakeTimers({ now: 1_000_000 });
    const session = new Session();
    session.signIn('t', ops, 1_000 + 60);
    vi.advanceTimersByTime(59_000);
    expect(session.token).toBe('t');
    vi.advanceTimersByTime(1_000);
    expect(session.token).toBeUndefined();
    expect(session.state.ended).toBe('Your session expired. Sign in again.');
  });

  it('does not sign in with a session that has already expired', () => {
    const session = new Session(() => 2_000_000);
    session.signIn('t', ops, 1_000);
    expect(session.token).toBeUndefined();
  });
});
