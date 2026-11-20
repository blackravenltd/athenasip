import { useState } from 'react';
import type { AdminApi } from '../api/AdminApi';
import type { SessionInfo } from '../api/types';
import { useSubmit } from '../hooks/useSubmit';
import { ROLE_TEXT, describeWho } from '../auth/roles';

/** When a session ends, in the browser's local time. */
export function describeSessionEnd(expiresAt: number | undefined): string {
  if (expiresAt === undefined) return 'When you log out.';
  return new Date(expiresAt * 1000).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
}

/** The signed-in user: roles, session expiry and password change. */
export function MeScreen({ api, info, expiresAt, onPasswordChanged }: {
  api: AdminApi;
  info: SessionInfo;
  expiresAt?: number;
  /** A password change ends every session the user holds, this one included. */
  onPasswordChanged: () => void;
}) {
  const [oldPassword, setOldPassword] = useState('');
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const { busy, error, run } = useSubmit();
  const mismatch = Boolean(confirm) && confirm !== password;

  const submit = () => {
    if (!oldPassword || !password || mismatch) return;
    void run(() => api.changePassword(info.username, { old_password: oldPassword, password })).then((done) => {
      if (done) onPasswordChanged();
    });
  };

  return (
    <>
      <h1>You</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>{describeWho(info)}</h2>
            <p>Signed in as {info.username}.</p>
          </div>
        </div>
        <dl className="readout">
          <dt>Roles</dt>
          <dd>{info.roles.length ? info.roles.map((role) => ROLE_TEXT[role].label).join(', ') : 'None'}</dd>
          <dt>Session ends</dt>
          <dd>{describeSessionEnd(expiresAt)}</dd>
        </dl>
      </section>

      {(
        <section className="panel">
          <div className="panel-heading">
            <div>
              <h2>Change your password</h2>
              <p>Your current password is needed to set a new one. Changing it signs you out everywhere, here included.</p>
            </div>
          </div>
          <form className="settings-form" onSubmit={(event) => { event.preventDefault(); submit(); }}>
            <label className="field">
              <span>Current password</span>
              <input type="password" value={oldPassword} disabled={busy} autoComplete="current-password" onChange={(event) => setOldPassword(event.target.value)} />
            </label>
            <label className="field">
              <span>New password</span>
              <input type="password" value={password} disabled={busy} autoComplete="new-password" onChange={(event) => setPassword(event.target.value)} />
            </label>
            <label className="field">
              <span>New password again</span>
              <input
                type="password"
                value={confirm}
                disabled={busy}
                autoComplete="new-password"
                aria-invalid={mismatch || undefined}
                onChange={(event) => setConfirm(event.target.value)}
              />
              {mismatch && <span className="field-error">The two new passwords differ.</span>}
            </label>
            {error && <p className="error-message" role="alert">{error}</p>}
            <div className="button-row">
              <button className="primary-button" type="submit" disabled={busy || !oldPassword || !password || !confirm || mismatch}>
                {busy ? 'Working...' : 'Change password'}
              </button>
            </div>
          </form>
        </section>
      )}
    </>
  );
}
