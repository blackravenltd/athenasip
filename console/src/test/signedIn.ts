import { FakeAdminApi, type FakeAdminApiOptions } from '../api/FakeAdminApi';
import { Session } from '../auth/Session';

/**
 * A secured in-memory node with a session wired to it as `main.tsx` does: the
 * token read per request, a 401 ending the session, a 403 re-reading its roles.
 */
export function securedNode(options: FakeAdminApiOptions = {}): { api: FakeAdminApi; session: Session } {
  const session = new Session();
  const api: FakeAdminApi = new FakeAdminApi({
    secured: true,
    token: () => session.token,
    onUnauthorized: (error) => session.signOut(`Signed out: ${error.message}.`),
    onForbidden: () => { void api.session().then((info) => session.update(info)).catch(() => undefined); },
    ...options,
  });
  return { api, session };
}

/** The same, signed in as one of the seeded users, whose password is their username. */
export async function signedInAs(username: string, options: FakeAdminApiOptions = {}) {
  const node = securedNode(options);
  const result = await node.api.login(username, username);
  node.session.signIn(result.token, await node.api.session(result.token), result.expires_at);
  return node;
}
