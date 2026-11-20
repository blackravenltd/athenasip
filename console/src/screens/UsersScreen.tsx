import { useCallback, useState } from 'react';
import type { AdminApi } from '../api/AdminApi';
import { isConflict } from '../api/errors';
import type { AdminUser, Role } from '../api/types';
import { ROLES } from '../api/types';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { useSubmit } from '../hooks/useSubmit';
import { ConfirmModal, FormModal } from '../components/Modal';
import { Empty, ErrorMessage, Loading } from '../components/Status';
import { ROLE_TEXT, describeRoles } from '../auth/roles';

/** When a user last signed in, in words. */
export function describeLastLogin(lastLoginAt: number, nowSeconds: number): string {
  if (!lastLoginAt) return 'Never signed in';
  const ago = Math.max(0, Math.round(nowSeconds - lastLoginAt));
  if (ago < 60) return 'Signed in just now';
  if (ago < 3600) return `Signed in ${Math.round(ago / 60)}m ago`;
  if (ago < 86_400) return `Signed in ${Math.round(ago / 3600)}h ago`;
  return `Signed in ${Math.round(ago / 86_400)}d ago`;
}

function same(a: string, b: string | null): boolean {
  return b !== null && a.toLowerCase() === b.toLowerCase();
}

/** The roles as checkboxes. No role implies another. */
function RoleFields({ roles, busy, locked, onChange }: {
  roles: readonly Role[];
  busy: boolean;
  /** Roles that cannot be changed here, with the reason. */
  locked?: Partial<Record<Role, string>>;
  onChange: (roles: Role[]) => void;
}) {
  return (
    <fieldset className="policy-fieldset">
      <legend>Roles</legend>
      {ROLES.map((role) => (
        <div key={role} className="role-option">
          <label className="checkbox-field">
            <input
              type="checkbox"
              checked={roles.includes(role)}
              disabled={busy || Boolean(locked?.[role])}
              onChange={(event) => onChange(event.target.checked
                ? ROLES.filter((candidate) => candidate === role || roles.includes(candidate))
                : roles.filter((candidate) => candidate !== role))}
            />
            <span>{ROLE_TEXT[role].label}</span>
          </label>
          <p className="field-hint">{locked?.[role] ?? ROLE_TEXT[role].detail}</p>
        </div>
      ))}
    </fieldset>
  );
}

function NewUserDialogue({ api, onClose, onSaved }: { api: AdminApi; onClose: () => void; onSaved: () => void }) {
  const [username, setUsername] = useState('');
  const [displayName, setDisplayName] = useState('');
  const [password, setPassword] = useState('');
  const [roles, setRoles] = useState<Role[]>([]);
  const { busy, error, failure, clearError, run } = useSubmit();
  const taken = isConflict(failure);

  const submit = () => {
    void run(() => api.createUser({ username: username.trim(), display_name: displayName.trim(), password, roles })).then((saved) => {
      if (!saved) return;
      onSaved();
      onClose();
    });
  };

  return (
    <FormModal
      open
      title="Add a user"
      submitLabel="Add user"
      busy={busy}
      submitDisabled={!username.trim() || !password}
      error={taken ? undefined : error}
      onSubmit={submit}
      onCancel={() => { clearError(); onClose(); }}
    >
      <label className="field">
        <span>Username</span>
        <input
          type="text"
          value={username}
          disabled={busy}
          autoComplete="off"
          spellCheck={false}
          aria-invalid={taken || undefined}
          onChange={(event) => { setUsername(event.target.value); if (taken) clearError(); }}
        />
        {taken && <span className="field-error" role="alert">{error}</span>}
      </label>
      <label className="field">
        <span>Display name</span>
        <input type="text" value={displayName} disabled={busy} onChange={(event) => setDisplayName(event.target.value)} />
      </label>
      <label className="field">
        <span>Password</span>
        <input type="password" value={password} disabled={busy} autoComplete="new-password" onChange={(event) => setPassword(event.target.value)} />
      </label>
      <p className="field-hint">
        A user signs in to this console and the API. It is not a subscriber, and adding one does not
        add the other. Usernames ignore case: the maintainer and ada are the same user.
      </p>
      <RoleFields roles={roles} busy={busy} onChange={setRoles} />
      {roles.length === 0 && (
        <p className="field-hint field-problem">With no roles this user can sign in and do nothing, which is the usual start.</p>
      )}
    </FormModal>
  );
}

function EditUserDialogue({ api, user, self, onClose, onSaved }: {
  api: AdminApi;
  user: AdminUser;
  self: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [displayName, setDisplayName] = useState(user.display_name);
  const [roles, setRoles] = useState<Role[]>(user.roles);
  const [disabled, setDisabled] = useState(user.disabled);
  const { busy, error, clearError, run } = useSubmit();

  const submit = () => {
    void run(() => api.updateUser(user.username, { display_name: displayName.trim(), roles, disabled })).then((saved) => {
      if (!saved) return;
      onSaved();
      onClose();
    });
  };

  return (
    <FormModal
      open
      title={`Edit ${user.username}`}
      submitLabel="Save user"
      busy={busy}
      error={error}
      onSubmit={submit}
      onCancel={() => { clearError(); onClose(); }}
    >
      <label className="field">
        <span>Display name</span>
        <input type="text" value={displayName} disabled={busy} onChange={(event) => setDisplayName(event.target.value)} />
      </label>
      <RoleFields
        roles={roles}
        busy={busy}
        // Guards against locking the last administrator out.
        locked={self ? { 'manage-admin-users': 'You cannot remove this role from yourself.' } : undefined}
        onChange={setRoles}
      />
      <label className="checkbox-field">
        <input type="checkbox" checked={disabled} disabled={busy || self} onChange={(event) => setDisabled(event.target.checked)} />
        <span>Disabled</span>
      </label>
      <p className="field-hint">
        {self
          ? 'You cannot disable yourself.'
          : 'A disabled user cannot sign in, and every session it holds ends at once. It is kept, so the record of what it did still names it.'}
      </p>
    </FormModal>
  );
}

function UserRow({ api, user, self, nowSeconds, onChanged }: {
  api: AdminApi;
  user: AdminUser;
  self: boolean;
  nowSeconds: number;
  onChanged: () => void;
}) {
  const [dialogue, setDialogue] = useState<'edit' | 'password' | 'sessions' | 'delete'>();
  const [password, setPassword] = useState('');
  const { busy, error, clearError, run } = useSubmit();
  const close = () => { setPassword(''); clearError(); setDialogue(undefined); };
  const act = (action: () => Promise<unknown>) => {
    void run(action).then((done) => {
      if (!done) return;
      onChanged();
      close();
    });
  };

  return (
    <li className="record-row">
      <div className="record-main" role="group" aria-label={user.username}>
        <span className="record-name">
          <span className={`state-dot state-${user.disabled ? 'down' : 'ok'}`} aria-hidden="true" />
          {user.display_name || user.username}
          {user.display_name && <span className="record-detail">{user.username}</span>}
        </span>
        <span className="record-detail">{user.roles.length ? describeRoles(user.roles) : 'No roles'}</span>
        <span className="record-detail">{describeLastLogin(user.last_login_at, nowSeconds)}</span>
      </div>
      {self && <span className="record-tag">You</span>}
      {user.disabled && <span className="record-tag state-tag-down">Disabled</span>}
      <div className="record-actions">
        <button className="secondary-button" type="button" aria-label={`Edit ${user.username}`} onClick={() => setDialogue('edit')}>Edit</button>
        <button className="secondary-button" type="button" aria-label={`Set password for ${user.username}`} onClick={() => setDialogue('password')}>
          Set password
        </button>
        <button className="secondary-button" type="button" aria-label={`Sign ${user.username} out everywhere`} onClick={() => setDialogue('sessions')}>
          Sign out
        </button>
        <button
          className="secondary-button danger-button"
          type="button"
          aria-label={`Delete ${user.username}`}
          disabled={self}
          title={self ? 'You cannot delete yourself' : undefined}
          onClick={() => setDialogue('delete')}
        >
          Delete
        </button>
      </div>

      {dialogue === 'edit' && <EditUserDialogue api={api} user={user} self={self} onClose={close} onSaved={onChanged} />}

      <FormModal
        open={dialogue === 'password'}
        title={`Set a password for ${user.username}`}
        submitLabel="Change password"
        busy={busy}
        submitDisabled={!password}
        error={error}
        onSubmit={() => act(() => api.changePassword(user.username, { password }))}
        onCancel={close}
      >
        <label className="field">
          <span>New password</span>
          <input type="password" value={password} disabled={busy} autoComplete="new-password" onChange={(event) => setPassword(event.target.value)} />
        </label>
      </FormModal>

      <ConfirmModal
        open={dialogue === 'sessions'}
        title={`Sign ${user.username} out everywhere?`}
        confirmLabel="End every session"
        busy={busy}
        onCancel={close}
        onConfirm={() => act(() => api.revokeSessions(user.username))}
      >
        <p>
          Every session <strong>{user.username}</strong> holds ends now, in every browser and every
          script. {self ? 'That includes this one, so you will be asked to sign in again.' : 'They can sign in again.'}
        </p>
        {error && <p className="error-message" role="alert">{error}</p>}
      </ConfirmModal>

      <ConfirmModal
        open={dialogue === 'delete'}
        title={`Delete ${user.username}?`}
        confirmLabel="Delete user"
        destructive
        busy={busy}
        onCancel={close}
        onConfirm={() => act(() => api.deleteUser(user.username))}
      >
        <p>
          <strong>{user.username}</strong> will be removed and its sessions ended. Disabling keeps the
          record of who did what; deleting does not. This cannot be undone.
        </p>
        {error && <p className="error-message" role="alert">{error}</p>}
      </ConfirmModal>
    </li>
  );
}

/** The users who can sign in to this node, each with its roles. */
export function UsersScreen({ api, username }: { api: AdminApi; username: string | null }) {
  const result = useRefreshableAsync((signal) => api.listUsers(signal), [api]);
  const refresh = result.refresh;
  const onChanged = useCallback(() => refresh(), [refresh]);
  const [adding, setAdding] = useState(false);
  const now = Date.now() / 1000;

  return (
    <>
      <h1>Users</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Who can sign in</h2>
            <p>
              A user is somebody, or some system, that uses this node&apos;s API, of which this console
              is one client. Each has the roles it has been given and no others: there is no
              superuser, and nothing implies anything else. Subscribers, the phones registered on a
              realm, are a different thing and are managed under SIP.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={result.refresh} disabled={result.refreshing}>
            {result.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {result.error && <ErrorMessage error={result.error} />}
        {!result.value ? (result.loading && <Loading />) : (
          <>
            <div className="panel-actions">
              <button className="primary-button" type="button" onClick={() => setAdding(true)}>Add a user</button>
            </div>
            {adding && <NewUserDialogue api={api} onClose={() => setAdding(false)} onSaved={onChanged} />}
            {result.value.length === 0 ? (
              <Empty>No users yet. The first is made with <code>athenasip --add-user</code> on the node&apos;s host.</Empty>
            ) : (
              <ul className="record-list">
                {result.value.map((user) => (
                  <UserRow
                    key={user.username}
                    api={api}
                    user={user}
                    self={same(user.username, username)}
                    nowSeconds={now}
                    onChanged={onChanged}
                  />
                ))}
              </ul>
            )}
          </>
        )}
      </section>
    </>
  );
}
