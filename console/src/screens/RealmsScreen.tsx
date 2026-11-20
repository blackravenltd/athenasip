import { useCallback, useState } from 'react';
import { Link } from 'react-router-dom';
import type { AdminApi } from '../api/AdminApi';
import { isConflict } from '../api/errors';
import type { BehaviourEffective, Realm } from '../api/types';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { usePagination } from '../hooks/usePagination';
import { useSubmit } from '../hooks/useSubmit';
import { ConfirmModal, FormModal } from '../components/Modal';
import { Empty, ErrorMessage, Loading } from '../components/Status';
import { realmSubscribers } from '../app/routes';
import { DEFAULT_POLICY, describeBehaviour, describeSeconds, policyForm, policySettings, serverDefaults } from '../realms/policy';
import { MediaFields, RegistrationFields } from '../realms/PolicyFields';

const PAGE_SIZE = 10;

function RealmDialogue({ api, realm, defaults, onClose, onSaved }: {
  api: AdminApi;
  /** Absent when creating. A realm cannot be renamed. */
  realm?: Realm;
  /** The server's media settings; see `serverDefaults`. */
  defaults: Partial<BehaviourEffective>;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [name, setName] = useState('');
  const [form, setForm] = useState(realm ? policyForm(realm) : DEFAULT_POLICY);
  const { busy, error, failure, clearError, run } = useSubmit();
  const checked = policySettings(form);
  const problem = 'problem' in checked ? checked.problem : undefined;
  // A taken name belongs beside the name, not in the dialogue's general slot.
  const nameTaken = !realm && isConflict(failure);

  const submit = () => {
    if (!('settings' in checked)) return;
    void run(() => (realm
      ? api.updateRealm(realm.name, checked.settings)
      : api.createRealm({ name: name.trim(), ...checked.settings }))).then((saved) => {
      if (!saved) return;
      onSaved();
      onClose();
    });
  };

  return (
    <FormModal
      open
      title={realm ? `Edit ${realm.name}` : 'Add a realm'}
      submitLabel={realm ? 'Save realm' : 'Add realm'}
      busy={busy}
      submitDisabled={(!realm && !name.trim()) || Boolean(problem)}
      error={nameTaken ? undefined : error}
      onSubmit={submit}
      onCancel={() => { clearError(); onClose(); }}
    >
      {!realm && (
        <label className="field">
          <span>Domain</span>
          <input
            type="text"
            value={name}
            disabled={busy}
            autoComplete="off"
            spellCheck={false}
            placeholder="sip.example.org"
            aria-invalid={nameTaken || undefined}
            onChange={(event) => { setName(event.target.value); if (nameTaken) clearError(); }}
          />
          {nameTaken && <span className="field-error" role="alert">{error}</span>}
        </label>
      )}
      {!realm && (
        <p className="field-hint">
          The domain phones put after the @ in their address. A realm cannot be renamed, because
          every subscriber in it is named by it.
        </p>
      )}
      <RegistrationFields form={form} defaults={defaults} busy={busy} onChange={setForm} />
      <MediaFields form={form} defaults={defaults} busy={busy} onChange={setForm} />
      {problem && <p className="field-hint field-problem" role="status">{problem}</p>}
    </FormModal>
  );
}

function DeleteRealm({ api, realm, onClose, onDeleted }: {
  api: AdminApi;
  realm: Realm;
  onClose: () => void;
  onDeleted: () => void;
}) {
  const subscribers = useRefreshableAsync((signal) => api.listSubscribers(realm.name, signal), [api, realm.name]);
  const { busy, error, clearError, run } = useSubmit();
  const count = subscribers.value?.length ?? 0;

  const remove = () => {
    void run(() => api.deleteRealm(realm.name)).then((removed) => {
      if (!removed) return;
      onDeleted();
      onClose();
    });
  };

  return (
    <ConfirmModal
      open
      title={`Delete ${realm.name}?`}
      confirmLabel="Delete realm"
      destructive
      busy={busy}
      onCancel={() => { clearError(); onClose(); }}
      onConfirm={remove}
    >
      <p>
        <strong>{realm.name}</strong> will be removed, and this node will stop answering for it.
        This cannot be undone.
      </p>
      {count > 0 && (
        <p className="field-problem">
          Its {count === 1 ? 'subscriber goes' : `${count} subscribers go`} with it, and every
          registration they hold ends.
        </p>
      )}
      {error && <p className="error-message" role="alert">{error}</p>}
    </ConfirmModal>
  );
}

function RealmRow({ api, realm, onChanged }: { api: AdminApi; realm: Realm; onChanged: () => void }) {
  const [dialogue, setDialogue] = useState<'edit' | 'delete'>();
  const close = () => setDialogue(undefined);
  const behaviour = describeBehaviour(realm);
  const registration = realm.registration_minimum > 0
    ? `Registrations ${describeSeconds(realm.registration_minimum)} to ${describeSeconds(realm.registration_timeout)}`
    : `Registrations up to ${describeSeconds(realm.registration_timeout)}`;

  return (
    <li className="record-row">
      <div className="record-main" role="group" aria-label={realm.name}>
        <span className="record-name">{realm.name}</span>
        <span className="record-detail">
          {registration}, nonces last {describeSeconds(realm.nonce_expiry)}
          {realm.behaviour_effective.qualify_interval > 0 && `, probed every ${describeSeconds(realm.behaviour_effective.qualify_interval)}`}
          {realm.behaviour_effective.rewrite_contact && ', Contact rewritten to source'}
        </span>
      </div>
      <span className="record-tag">
        {behaviour.relayed ? behaviour.profile.label : 'Media direct'}
      </span>
      <div className="record-actions">
        <Link className="secondary-button" to={realmSubscribers(realm.name)} aria-label={`Subscribers in ${realm.name}`}>Subscribers</Link>
        {/* The accessible name says which realm, for a screen reader. */}
        <button className="secondary-button" type="button" aria-label={`Edit ${realm.name}`} onClick={() => setDialogue('edit')}>
          Edit
        </button>
        <button
          className="secondary-button danger-button"
          type="button"
          aria-label={`Delete ${realm.name}`}
          onClick={() => setDialogue('delete')}
        >
          Delete
        </button>
      </div>

      {dialogue === 'edit' && <RealmDialogue api={api} realm={realm} defaults={serverDefaults(realm)} onClose={close} onSaved={onChanged} />}
      {dialogue === 'delete' && <DeleteRealm api={api} realm={realm} onClose={close} onDeleted={onChanged} />}
    </li>
  );
}

export function RealmsScreen({ api }: { api: AdminApi }) {
  const result = useRefreshableAsync((signal) => api.listRealms(signal), [api]);
  const refresh = result.refresh;
  const onChanged = useCallback(() => refresh(), [refresh]);
  const [adding, setAdding] = useState(false);
  const realms = result.value ?? [];
  const { page, pageIndex, pageCount, next, previous } = usePagination(realms, PAGE_SIZE);

  return (
    <>
      <h1>Realms</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>SIP domains</h2>
            <p>
              A realm is a SIP domain this node is responsible for. In the identity below, the
              realm is the domain part.
            </p>
            <code className="example">&quot;John Smith&quot; &lt;sip:john.smith@sip.athenasip.org&gt;</code>
            <p>
              Subscribers belong to exactly one realm, and authentication is scoped to it: the same
              name in two realms is two different subscribers. A realm also says how long its phones
              may stay registered and how their calls' media is handled.
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
              <button className="primary-button" type="button" onClick={() => setAdding(true)}>Add a realm</button>
            </div>
            {adding && <RealmDialogue api={api} defaults={serverDefaults(realms[0])} onClose={() => setAdding(false)} onSaved={onChanged} />}

            {realms.length === 0 ? (
              <Empty>No realms yet. Add one, and this node will start answering for that domain.</Empty>
            ) : (
              <>
                <ul className="record-list">
                  {page.map((realm) => <RealmRow key={realm.name} api={api} realm={realm} onChanged={onChanged} />)}
                </ul>
                {pageCount > 1 && (
                  <div className="pagination">
                    <button className="secondary-button" type="button" onClick={previous} disabled={pageIndex === 0}>
                      Previous
                    </button>
                    <span>Page {pageIndex + 1} of {pageCount}</span>
                    <button className="secondary-button" type="button" onClick={next} disabled={pageIndex === pageCount - 1}>
                      Next
                    </button>
                  </div>
                )}
              </>
            )}
          </>
        )}
      </section>
    </>
  );
}
