import { useSyncExternalStore } from 'react';
import type { Session, SessionState } from './Session';

/** The session's state, re-rendering whenever it signs in or out. */
export function useSession(session: Session): SessionState {
  return useSyncExternalStore(
    (listener) => session.subscribe(listener),
    () => session.state,
  );
}
