import { useCallback, useState } from 'react';
import type { AdminApi } from '../api/AdminApi';
import { ApiError } from '../api/errors';
import type { MediaEngine, MediaReoffer, Realm } from '../api/types';
import { MEDIA_PROFILE_TEXT } from '../realms/policy';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { useSubmit } from '../hooks/useSubmit';
import { FormModal } from '../components/Modal';
import { Empty, ErrorMessage, Loading } from '../components/Status';
import { describeBehaviour, policyForm, policySettings, serverDefaults } from '../realms/policy';
import { MediaFields } from '../realms/PolicyFields';

function MediaDialogue({ api, realm, onClose, onSaved }: {
  api: AdminApi;
  realm: Realm;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [form, setForm] = useState(policyForm(realm));
  const { busy, error, clearError, run } = useSubmit();

  const submit = () => {
    const checked = policySettings(form);
    if (!('settings' in checked)) return;
    // Only the media settings: the API changes what it is given and nothing else.
    const { media_anchor, media_profile } = checked.settings.behaviour ?? {};
    void run(() => api.updateRealm(realm.name, { behaviour: { media_anchor, media_profile } })).then((saved) => {
      if (!saved) return;
      onSaved();
      onClose();
    });
  };

  return (
    <FormModal
      open
      title={`Media for ${realm.name}`}
      submitLabel="Save media policy"
      busy={busy}
      error={error}
      onSubmit={submit}
      onCancel={() => { clearError(); onClose(); }}
    >
      <MediaFields form={form} defaults={serverDefaults(realm)} busy={busy} onChange={setForm} />
    </FormModal>
  );
}

/** A subscriber's address of record as the realm and user the API addresses it by, if it parses. */
export function subscriberOf(uri: string): { realm: string; user: string } | undefined {
  const match = /^sips?:([^@;]+)@([^;:>]+)/.exec(uri);
  return match ? { user: decodeURIComponent(match[1]), realm: match[2] } : undefined;
}

/** What the node did for one subscriber, in words. */
export function describeReoffer(reoffer: MediaReoffer): string {
  const word = (profile: 'rtp' | 'webrtc') => MEDIA_PROFILE_TEXT[profile].label;
  const times = reoffer.count === 1 ? 'once' : `${reoffer.count} times`;
  return reoffer.took
    ? `Refused ${word(reoffer.rejected)} and took ${word(reoffer.took)}, ${times}.`
    : `Refused ${word(reoffer.rejected)} and then the other as well, ${times}.`;
}

/**
 * One subscriber that needed the other profile. Setting the subscriber's own
 * profile saves its phone a 488 and a second offer on every call.
 */
function ReofferRow({ api, reoffer }: { api: AdminApi; reoffer: MediaReoffer }) {
  const { busy, error, run } = useSubmit();
  const [done, setDone] = useState(false);
  const subscriber = subscriberOf(reoffer.subscriber);
  const suggested = reoffer.suggested_media_profile;

  const apply = () => {
    if (!subscriber || !suggested) return;
    void run(() => api.updateSubscriber(subscriber.realm, subscriber.user, { behaviour: { media_profile: suggested } })).then((saved) => {
      if (saved) setDone(true);
    });
  };

  return (
    <li className="record-row">
      <div className="record-main" role="group" aria-label={reoffer.subscriber}>
        <span className="record-name">{reoffer.subscriber}</span>
        <span className="record-detail">{describeReoffer(reoffer)} Last {new Date(reoffer.last_at).toLocaleString()}.</span>
        {error && <span className="field-error" role="alert">{error}</span>}
      </div>
      {subscriber && suggested && (
        <div className="record-actions">
          {done ? (
            <span className="record-tag state-tag-ok">Set to {MEDIA_PROFILE_TEXT[suggested].label}</span>
          ) : (
            <button
              className="secondary-button"
              type="button"
              disabled={busy}
              aria-label={`Set ${reoffer.subscriber} to ${MEDIA_PROFILE_TEXT[suggested].label}`}
              onClick={apply}
            >
              Set its profile to {MEDIA_PROFILE_TEXT[suggested].label}
            </button>
          )}
        </div>
      )}
    </li>
  );
}

/** One realm's media settings, each marked when inherited from the server. */
function MediaRow({ realm, onEdit }: { realm: Realm; onEdit: () => void }) {
  const behaviour = describeBehaviour(realm);
  const tag = (label: string, inherited: boolean) => (inherited ? `${label} (server default)` : label);
  return (
    <li className="record-row">
      <div className="record-main" role="group" aria-label={realm.name}>
        <span className="record-name">{realm.name}</span>
        <span className="record-detail">
          {behaviour.relayed ? behaviour.profile.detail : 'Media goes directly between the phones; this node stays out of it.'}
        </span>
      </div>
      <span className="record-tag">{tag(behaviour.anchor.label, behaviour.anchor.inherited)}</span>
      {behaviour.relayed && <span className="record-tag">{tag(behaviour.profile.label, behaviour.profile.inherited)}</span>}
      <div className="record-actions">
        <button className="secondary-button" type="button" aria-label={`Edit media for ${realm.name}`} onClick={onEdit}>
          Edit
        </button>
      </div>
    </li>
  );
}

/** A 403 means the user lacks the role, which is not a fault. */
function notForThisLogin(error: Error | undefined): boolean {
  return error instanceof ApiError && error.isForbidden;
}

/** The engine as a tone and a sentence. No engine is a valid configuration, not a fault. */
export function engineState(engine: MediaEngine): { label: string; tone: 'ok' | 'warn' | 'down' } {
  if (!engine.engine) return { label: 'No media engine: every call’s media goes directly between the phones.', tone: 'warn' };
  return engine.connected
    ? { label: `${engine.engine}, connected`, tone: 'ok' }
    : { label: `${engine.engine}, not connected: anchored calls have no relay`, tone: 'down' };
}

/**
 * The media engine, and how each realm's calls are carried.
 *
 * The API never gives the engine's URL, which can carry credentials; that and
 * the relay's port range are set in the configuration file. The two panels
 * need different roles.
 */
export function MediaScreen({ api }: { api: AdminApi }) {
  const engine = useRefreshableAsync((signal) => api.mediaEngine(signal), [api]);
  const reoffers = useRefreshableAsync((signal) => api.listMediaReoffers(signal), [api]);
  const result = useRefreshableAsync((signal) => api.listRealms(signal), [api]);
  const refresh = result.refresh;
  const onChanged = useCallback(() => refresh(), [refresh]);
  const [editing, setEditing] = useState<Realm>();

  return (
    <>
      <h1>Media</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Media engine</h2>
            <p>
              What relays the audio of the calls this node anchors. Which engine, and where it
              is, are set in the node&apos;s configuration file.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={engine.refresh} disabled={engine.refreshing}>
            {engine.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {notForThisLogin(engine.error)
          ? <p className="field-hint">This login&apos;s roles do not include the node&apos;s status.</p>
          : engine.error && <ErrorMessage error={engine.error} />}
        {!engine.value ? (engine.loading && <Loading />) : (
          <dl className="readout">
            <dt>Engine</dt>
            <dd>
              <span className={`state-dot state-${engineState(engine.value).tone}`} aria-hidden="true" />{' '}
              {engineState(engine.value).label}
            </dd>
            <dt>Can</dt>
            <dd>{engine.value.capabilities.length > 0 ? engine.value.capabilities.join(', ') : '-'}</dd>
          </dl>
        )}
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Media policy by realm</h2>
            <p>
              Whether this node relays each realm&apos;s call audio through its media engine, and
              what it offers a phone it has not heard from yet.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={result.refresh} disabled={result.refreshing}>
            {result.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {notForThisLogin(result.error)
          ? <p className="field-hint">This login&apos;s roles do not include managing realms.</p>
          : result.error && <ErrorMessage error={result.error} />}
        {!result.value ? (result.loading && <Loading />) : result.value.length === 0 ? (
          <Empty>No realms yet. Media policy is set per realm, so there is nothing to show.</Empty>
        ) : (
          <ul className="record-list">
            {result.value.map((realm) => <MediaRow key={realm.name} realm={realm} onEdit={() => setEditing(realm)} />)}
          </ul>
        )}
        {editing && <MediaDialogue api={api} realm={editing} onClose={() => setEditing(undefined)} onSaved={onChanged} />}
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Re-offered media</h2>
            <p>
              Subscribers whose phone refused the media it was offered with 488, since this node
              started. The node then offers the other kind once, WebRTC for plain RTP or back. Each
              time costs the caller a moment&apos;s delay; setting the subscriber&apos;s own profile to
              what its phone took makes the first offer right.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={reoffers.refresh} disabled={reoffers.refreshing}>
            {reoffers.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {notForThisLogin(reoffers.error)
          ? <p className="field-hint">This login&apos;s roles do not include the node&apos;s status.</p>
          : reoffers.error && <ErrorMessage error={reoffers.error} />}
        {!reoffers.value ? (reoffers.loading && <Loading />) : reoffers.value.length === 0 ? (
          <Empty>No phone has refused what it was offered.</Empty>
        ) : (
          <ul className="record-list">
            {reoffers.value.map((reoffer) => <ReofferRow key={reoffer.subscriber} api={api} reoffer={reoffer} />)}
          </ul>
        )}
      </section>
    </>
  );
}
