import type { BehaviourEffective, MediaProfile } from '../api/types';
import { MEDIA_PROFILES, QUALIFY_MAX, QUALIFY_MIN } from '../api/types';
import { MEDIA_PROFILE_TEXT, describeQualify, type PolicyForm } from './policy';

/** The registration half of a realm's settings. */
export function RegistrationFields({ form, defaults, busy, onChange }: {
  form: PolicyForm;
  /** The server's values, where known. See `serverDefaults`. */
  defaults: Partial<BehaviourEffective>;
  busy: boolean;
  onChange: (form: PolicyForm) => void;
}) {
  const number = (key: 'registration_timeout' | 'registration_minimum' | 'nonce_expiry', label: string) => (
    <label className="field">
      <span>{label}</span>
      <input
        type="text"
        inputMode="numeric"
        value={form[key]}
        disabled={busy}
        onChange={(event) => onChange({ ...form, [key]: event.target.value })}
      />
    </label>
  );

  return (
    <fieldset className="policy-fieldset">
      <legend>Registration, in seconds</legend>
      <div className="field-row">
        {number('registration_timeout', 'Longest registration')}
        {number('registration_minimum', 'Shortest registration')}
        {number('nonce_expiry', 'Nonce lifetime')}
      </div>
      <p className="field-hint">
        A phone asking for longer than the longest is given the longest. One asking for less than
        the shortest is refused with 423 and told the minimum; zero accepts any. The nonce is the
        one-time value a password challenge is built on, and this is how long one stays good.
      </p>
      <label className="field">
        <span>Probe each client every</span>
        <input
          type="text"
          inputMode="numeric"
          value={form.qualify_interval}
          disabled={busy}
          placeholder={inheritLabel(defaults.qualify_interval === undefined ? undefined : describeQualify(defaults.qualify_interval).toLowerCase())}
          onChange={(event) => onChange({ ...form, qualify_interval: event.target.value })}
        />
      </label>
      <p className="field-hint">
        Seconds between OPTIONS sent to each registered client, down the connection it registered
        on: 0 for never, otherwise {QUALIFY_MIN} to {QUALIFY_MAX}. Empty takes the server&apos;s. A
        client that answers with a session description says what media it takes, which the
        Registrations screen shows.
      </p>
      <label className="field">
        <span>Rewrite Contact to source address (NAT)</span>
        <select
          value={form.rewrite_contact === null ? '' : form.rewrite_contact ? 'on' : 'off'}
          disabled={busy}
          onChange={(event) => onChange({ ...form, rewrite_contact: event.target.value === '' ? null : event.target.value === 'on' })}
        >
          <option value="">{inheritLabel(defaults.rewrite_contact === undefined ? undefined : defaults.rewrite_contact ? 'on' : 'off')}</option>
          <option value="on">On</option>
          <option value="off">Off</option>
        </select>
      </label>
      <p className="field-hint">
        On, the node replaces the Contact of each message it forwards with the address and port
        the message actually came from, as Asterisk&apos;s <code>rewrite_contact</code> does. For
        phones behind a NAT that put their private address in the Contact. WebSocket clients are
        never rewritten.
      </p>
    </fieldset>
  );
}

function inheritLabel(value: string | undefined): string {
  return value === undefined ? 'Server default' : `Server default (${value})`;
}

/** The media half of a realm's settings. "Server default" sends null, which makes the realm inherit. */
export function MediaFields({ form, defaults, busy, onChange }: {
  form: PolicyForm;
  /** The server's values, where known. See `serverDefaults`. */
  defaults: Partial<BehaviourEffective>;
  busy: boolean;
  onChange: (form: PolicyForm) => void;
}) {
  const anchor = form.media_anchor ?? defaults.media_anchor;
  const profile = form.media_profile ?? defaults.media_profile;
  const anchorDefault = defaults.media_anchor === undefined ? undefined : defaults.media_anchor ? 'relayed' : 'direct';
  const profileDefault = defaults.media_profile && MEDIA_PROFILE_TEXT[defaults.media_profile].label.toLowerCase();

  return (
    <fieldset className="policy-fieldset">
      <legend>Media</legend>
      <label className="field">
        <span>Relaying</span>
        <select
          value={form.media_anchor === null ? '' : form.media_anchor ? 'on' : 'off'}
          disabled={busy}
          onChange={(event) => onChange({ ...form, media_anchor: event.target.value === '' ? null : event.target.value === 'on' })}
        >
          <option value="">{inheritLabel(anchorDefault)}</option>
          <option value="on">Relay through this node&apos;s media engine</option>
          <option value="off">Direct between the phones</option>
        </select>
      </label>
      <p className="field-hint">
        Direct leaves every call&apos;s media going between the two phones. That is fine when they
        can reach each other and fails for anything behind a NAT.
      </p>
      <label className="field">
        <span>Media profile</span>
        <select
          value={form.media_profile ?? ''}
          disabled={busy || anchor === false}
          onChange={(event) => onChange({ ...form, media_profile: event.target.value === '' ? null : event.target.value as MediaProfile })}
        >
          <option value="">{inheritLabel(profileDefault)}</option>
          {MEDIA_PROFILES.map((option) => (
            <option key={option} value={option}>{MEDIA_PROFILE_TEXT[option].label}</option>
          ))}
        </select>
      </label>
      <p className="field-hint">
        {profile ? `${MEDIA_PROFILE_TEXT[profile].detail} ` : 'The server\u2019s own profile decides. '}
        Once a phone has sent its own description, the node answers it in its own terms; this
        decides only for one it has not heard from yet.
      </p>
    </fieldset>
  );
}

/** A subscriber's media profile, overriding its realm's. "Realm default" sends null, which inherits. */
export function SubscriberMediaField({ value, realmProfile, busy, onChange }: {
  value: MediaProfile | null;
  /** The realm's effective profile, when the realm is known. */
  realmProfile?: MediaProfile;
  busy: boolean;
  onChange: (value: MediaProfile | null) => void;
}) {
  const profile = value ?? realmProfile;
  const realmLabel = realmProfile && MEDIA_PROFILE_TEXT[realmProfile].label.toLowerCase();
  return (
    <>
      <label className="field">
        <span>Media profile</span>
        <select
          value={value ?? ''}
          disabled={busy}
          onChange={(event) => onChange(event.target.value === '' ? null : event.target.value as MediaProfile)}
        >
          <option value="">{realmLabel ? `Realm default (${realmLabel})` : 'Realm default'}</option>
          {MEDIA_PROFILES.map((option) => (
            <option key={option} value={option}>{MEDIA_PROFILE_TEXT[option].label}</option>
          ))}
        </select>
      </label>
      <p className="field-hint">
        {profile ? `${MEDIA_PROFILE_TEXT[profile].detail} ` : ''}
        What this subscriber&apos;s phone is offered first. Set it for a phone the realm&apos;s profile
        gets wrong, such as AthenaPhone over TCP, which speaks WebRTC. Whatever the phone says in
        its own description still wins, and it matters only when the realm relays media.
      </p>
    </>
  );
}
