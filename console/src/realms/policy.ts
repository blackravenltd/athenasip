import type { BehaviourEffective, MediaProfile, Realm, RealmSettings } from '../api/types';
import { QUALIFY_MAX, QUALIFY_MIN } from '../api/types';

/**
 * Each media profile in plain words. A profile decides only the first offer
 * to a leg the node has not yet heard describe itself.
 */
export const MEDIA_PROFILE_TEXT: Record<MediaProfile, { label: string; detail: string }> = {
  mirror: {
    label: 'Mirror the caller',
    detail: 'The callee is offered whatever the caller offered. Right when both ends speak alike, browser to browser or phone to phone.',
  },
  transport: {
    label: 'By transport',
    detail: 'WebSocket clients are treated as browsers (WebRTC) and everything else as plain RTP. Right wherever a WebSocket means a browser.',
  },
  rtp: {
    label: 'Plain RTP',
    detail: 'Every leg is plain RTP. For a realm whose WebSocket clients are SIP phones rather than browsers.',
  },
  webrtc: {
    label: 'WebRTC',
    detail: 'Every leg is WebRTC: ICE and DTLS-SRTP. For a realm of browsers and AthenaPhone, whose media is WebRTC on every transport.',
  },
  srtp: {
    label: 'SRTP (SDES)',
    detail: 'Every leg is SRTP with its keys in the description (RFC 4568), for a desk phone that encrypts media and has never heard of DTLS.',
  },
};

/**
 * A realm's settings as form state: numbers as text, and null for a setting
 * inherited from the server's `behaviour:` section.
 */
export interface PolicyForm {
  registration_timeout: string;
  registration_minimum: string;
  nonce_expiry: string;
  media_anchor: boolean | null;
  media_profile: MediaProfile | null;
  /** Empty for the server's. */
  qualify_interval: string;
  rewrite_contact: boolean | null;
}

/** A new realm inherits its media settings from the server. */
export const DEFAULT_POLICY: PolicyForm = {
  registration_timeout: '5000',
  registration_minimum: '0',
  nonce_expiry: '3600',
  media_anchor: null,
  media_profile: null,
  qualify_interval: '',
  rewrite_contact: null,
};

export function policyForm(realm: Realm): PolicyForm {
  return {
    registration_timeout: String(realm.registration_timeout),
    registration_minimum: String(realm.registration_minimum),
    nonce_expiry: String(realm.nonce_expiry),
    media_anchor: realm.behaviour.media_anchor,
    media_profile: realm.behaviour.media_profile,
    qualify_interval: realm.behaviour.qualify_interval === null ? '' : String(realm.behaviour.qualify_interval),
    rewrite_contact: realm.behaviour.rewrite_contact,
  };
}

/**
 * The server's value for each setting, which every realm carries as
 * `behaviour_default`. Empty when there is no realm to read it from.
 */
export function serverDefaults(realm?: Pick<Realm, 'behaviour_default'>): Partial<BehaviourEffective> {
  return realm ? { ...realm.behaviour_default } : {};
}

/** A realm's media settings in words, and whether each is inherited from the server. */
export function describeBehaviour(realm: Realm): {
  relayed: boolean;
  anchor: { label: string; inherited: boolean };
  profile: { label: string; detail: string; inherited: boolean };
} {
  const { media_anchor: relayed, media_profile: profile } = realm.behaviour_effective;
  return {
    relayed,
    anchor: { label: relayed ? 'Relayed' : 'Direct', inherited: realm.behaviour.media_anchor === null },
    profile: { ...MEDIA_PROFILE_TEXT[profile], inherited: realm.behaviour.media_profile === null },
  };
}

/**
 * The form as the API wants it, or the reason it cannot be sent. The server
 * does not check the numbers against each other, so that is done here.
 */
export function policySettings(form: PolicyForm): { settings: RealmSettings } | { problem: string } {
  const seconds = (text: string, what: string): number | string => {
    const trimmed = text.trim();
    if (!/^\d+$/.test(trimmed)) return `${what} must be a whole number of seconds.`;
    return Number(trimmed);
  };
  const timeout = seconds(form.registration_timeout, 'The longest registration');
  if (typeof timeout === 'string') return { problem: timeout };
  const minimum = seconds(form.registration_minimum, 'The shortest registration');
  if (typeof minimum === 'string') return { problem: minimum };
  const nonce = seconds(form.nonce_expiry, 'The nonce lifetime');
  if (typeof nonce === 'string') return { problem: nonce };
  if (timeout === 0) return { problem: 'The longest registration has to be more than zero, or nothing can register.' };
  if (nonce === 0) return { problem: 'The nonce lifetime has to be more than zero, or every challenge is stale on arrival.' };
  if (minimum > timeout) {
    return { problem: 'The shortest registration is longer than the longest, so every phone would be refused.' };
  }
  const qualifyText = form.qualify_interval.trim();
  let qualify: number | null = null;
  if (qualifyText) {
    qualify = /^\d+$/.test(qualifyText) ? Number(qualifyText) : NaN;
    if (!Number.isInteger(qualify) || (qualify !== 0 && (qualify < QUALIFY_MIN || qualify > QUALIFY_MAX))) {
      return { problem: `Probing has to be 0, for never, or ${QUALIFY_MIN} to ${QUALIFY_MAX} seconds. Leave it empty for the server's.` };
    }
  }
  return {
    settings: {
      registration_timeout: timeout,
      registration_minimum: minimum,
      nonce_expiry: nonce,
      // Null makes a setting inherit the server's.
      behaviour: { media_anchor: form.media_anchor, media_profile: form.media_profile, qualify_interval: qualify, rewrite_contact: form.rewrite_contact },
    },
  };
}

/** A duration in words, for a realm row. */
export function describeSeconds(seconds: number): string {
  if (seconds % 3600 === 0 && seconds >= 3600) return `${seconds / 3600}h`;
  if (seconds % 60 === 0 && seconds >= 60) return `${seconds / 60}m`;
  return `${seconds}s`;
}

/** How often a realm's clients are probed, in words. */
export function describeQualify(seconds: number): string {
  return seconds === 0 ? 'Never' : `Every ${describeSeconds(seconds)}`;
}
