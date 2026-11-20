import ReactDOM from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import '@fontsource-variable/roboto/wght.css';
import App from './App';
import { FakeAdminApi } from './api/FakeAdminApi';
import { HttpAdminApi } from './api/HttpAdminApi';
import type { AdminApi } from './api/AdminApi';
import type { ApiError } from './api/errors';
import { Session } from './auth/Session';
import './styles.css';

const session = new Session();

/** A 401 from any request ends the session, whichever screen made it. */
const onUnauthorized = (error: ApiError) => session.signOut(`Signed out: ${error.message}.`);

/**
 * A 403 may mean a role was taken away since sign-in. Re-read the session's
 * roles, one request at a time, so the navigation matches them.
 */
let refreshing = false;
const onForbidden = () => {
  if (refreshing || !session.token) return;
  refreshing = true;
  void api.session()
    .then((info) => session.update(info))
    .catch(() => undefined)
    .finally(() => { refreshing = false; });
};

/**
 * Which node this client talks to.
 *
 * `VITE_ATHENASIP_LIVE=true` selects the real node at same-origin `/api/v1`
 * (proxied by the dev server). Anything else selects the in-memory node, which
 * enforces the same users, roles and tokens. Its latency is deliberate, so
 * loading states are seen in development.
 */
function selectApi(): { api: AdminApi; hint?: string } {
  if (import.meta.env.VITE_ATHENASIP_LIVE === 'true') {
    return { api: new HttpAdminApi({ token: () => session.token, onUnauthorized, onForbidden }) };
  }
  return {
    api: new FakeAdminApi({
      latencyMs: 220,
      secured: true,
      token: () => session.token,
      onUnauthorized,
      onForbidden,
    }),
    hint: 'This is the in-memory development node. Each password is the username: admin has every role, ops views status, helpdesk manages subscribers, newhire has none.',
  };
}

const { api, hint } = selectApi();

const rootElement = document.getElementById('root');
if (!rootElement) throw new Error('Missing #root element');

ReactDOM.createRoot(rootElement).render(
  <BrowserRouter>
    <App api={api} session={session} loginHint={hint} />
  </BrowserRouter>,
);
