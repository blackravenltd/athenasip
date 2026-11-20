import { useEffect } from 'react';
import type { AdminApi } from '../api/AdminApi';
import type { CallRecord, Role } from '../api/types';
import { can } from '../auth/roles';
import { ErrorMessage, Loading } from '../components/Status';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import type { CallState } from '../softphone/Softphone';
import { callTimer, userOf } from './dial';

/** `user@host` of a SIP URI, lower case, without scheme, port or parameters, for comparing addresses. */
export function addressOf(uri: string | null | undefined): string | undefined {
  const match = /^(?:sips?:)?([^@;>?]+)@([^:;>?]+)/i.exec(uri ?? '');
  return match ? `${match[1]}@${match[2]}`.toLowerCase() : undefined;
}

export interface HistoryEntry {
  id: string;
  direction: 'outgoing' | 'incoming';
  /** The other party's URI, as the record gives it. */
  other: string;
  endedAt?: number;
  answered: boolean;
  duration: number;
}

/** The node's call records that involve this line, from this line's point of view. */
export function historyFor(records: readonly CallRecord[], ownUri: string): HistoryEntry[] {
  const own = addressOf(ownUri);
  if (!own) return [];
  return records.flatMap((record): HistoryEntry[] => {
    const outgoing = addressOf(record.caller) === own;
    if (!outgoing && addressOf(record.callee) !== own) return [];
    const other = (outgoing ? record.callee : record.caller) ?? '';
    return [{
      id: record.id,
      direction: outgoing ? 'outgoing' : 'incoming',
      other,
      endedAt: record.ended_at ? Date.parse(record.ended_at) : undefined,
      answered: record.answered_at !== null,
      duration: record.duration,
    }];
  });
}

/** A relative time: minutes or hours ago for today, the date before that. */
export function when(at: number | undefined, now = Date.now()): string {
  if (at === undefined || Number.isNaN(at)) return '';
  const minutes = Math.round((now - at) / 60_000);
  if (minutes < 1) return 'just now';
  if (minutes < 60) return `${minutes} min ago`;
  if (minutes < 24 * 60) return `${Math.round(minutes / 60)} h ago`;
  return new Date(at).toLocaleDateString(undefined, { day: 'numeric', month: 'short' });
}

/**
 * Beside the dialler: this line's recent calls and the realm's subscribers.
 * Each is read with the console user's roles, not the line's, and says so
 * where those roles do not allow it.
 */
export function PhoneAside({ api, roles, ownUri, call, onDial }: {
  api: AdminApi;
  roles: readonly Role[];
  ownUri: string;
  call: CallState;
  onDial: (uri: string) => void;
}) {
  const readsStatus = can(roles, 'view-cluster-status');
  const readsSubscribers = can(roles, 'manage-realm-subscribers');
  const realm = /@([^:;>?]+)/.exec(ownUri)?.[1];
  const own = addressOf(ownUri);

  const records = useRefreshableAsync(
    (signal) => (readsStatus ? api.listCallRecords(200, signal) : Promise.resolve([])),
    [api, readsStatus],
  );
  const directory = useRefreshableAsync(async (signal) => {
    if (!realm) return [];
    const [subscribers, registrations] = await Promise.all([
      readsSubscribers ? api.listSubscribers(realm, signal) : Promise.resolve(undefined),
      readsStatus ? api.listRegistrations(realm, signal) : Promise.resolve(undefined),
    ]);
    const online = new Set((registrations ?? []).map((registration) => addressOf(registration.subscriber)));
    const uris = subscribers ? subscribers.map((subscriber) => subscriber.uri) : [...new Set((registrations ?? []).map((registration) => registration.subscriber))];
    return uris
      .filter((uri) => addressOf(uri) !== own)
      .map((uri) => ({ uri, online: registrations ? online.has(addressOf(uri)) : undefined }))
      .sort((a, b) => userOf(a.uri).localeCompare(userOf(b.uri), undefined, { numeric: true }));
  }, [api, realm, own, readsSubscribers, readsStatus]);

  // The node writes a record as a call ends, so re-read shortly after.
  const refreshRecords = records.refresh;
  const refreshDirectory = directory.refresh;
  useEffect(() => {
    if (call !== 'ended' && call !== 'failed') return;
    const timer = window.setTimeout(() => { refreshRecords(); refreshDirectory(); }, 1500);
    return () => window.clearTimeout(timer);
  }, [call, refreshRecords, refreshDirectory]);

  const history = historyFor(records.value ?? [], ownUri).slice(0, 20);

  return (
    <div className="phone-aside">
      <section className="panel" aria-labelledby="phone-history-heading">
        <h2 id="phone-history-heading">Recent calls</h2>
        {!readsStatus ? (
          <p className="field-hint">Recent calls come from the node&apos;s call records, which need the View cluster status role.</p>
        ) : records.loading ? <Loading /> : records.error ? <ErrorMessage error={records.error} /> : history.length === 0 ? (
          <p className="field-hint">No calls to or from this line in the node&apos;s records.</p>
        ) : (
          <ul className="phone-list" aria-label="Recent calls">
            {history.map((entry) => (
              <li key={entry.id} className="phone-list-row">
                <div className="phone-list-main">
                  <span className="phone-list-name">{userOf(entry.other)}</span>
                  <span className="phone-list-detail">
                    {entry.direction === 'outgoing' ? 'Outgoing' : entry.answered ? 'Incoming' : 'Missed'}
                    {entry.answered ? `, ${callTimer(entry.duration * 1000)}` : entry.direction === 'outgoing' ? ', not answered' : ''}
                    {entry.endedAt !== undefined ? `, ${when(entry.endedAt)}` : ''}
                  </span>
                </div>
                {entry.other && (
                  <button className="secondary-button" type="button" disabled={call !== 'idle' && call !== 'ended' && call !== 'failed'} onClick={() => onDial(entry.other)} aria-label={`Call ${userOf(entry.other)}`}>
                    Call
                  </button>
                )}
              </li>
            ))}
          </ul>
        )}
      </section>

      <section className="panel" aria-labelledby="phone-directory-heading">
        <h2 id="phone-directory-heading">{realm ? `Directory: ${realm}` : 'Directory'}</h2>
        {!readsSubscribers && !readsStatus ? (
          <p className="field-hint">The directory lists this realm&apos;s subscribers, which needs the Manage subscribers or View cluster status role.</p>
        ) : directory.loading ? <Loading /> : directory.error ? <ErrorMessage error={directory.error} /> : (directory.value ?? []).length === 0 ? (
          <p className="field-hint">{readsSubscribers ? 'No other subscribers in this realm.' : 'Nobody else is registered in this realm.'}</p>
        ) : (
          <>
            {!readsSubscribers && <p className="field-hint">Only those registered now: listing every subscriber needs the Manage subscribers role.</p>}
            <ul className="phone-list" aria-label="Directory">
              {(directory.value ?? []).map((entry) => (
                <li key={entry.uri} className="phone-list-row">
                  <div className="phone-list-main">
                    <span className="phone-list-name">
                      {entry.online !== undefined && <span className={`state-dot state-${entry.online ? 'ok' : 'idle'}`} aria-hidden="true" />}
                      {userOf(entry.uri)}
                    </span>
                    <span className="phone-list-detail">
                      {entry.online === undefined ? entry.uri : entry.online ? 'Registered' : 'Not registered'}
                    </span>
                  </div>
                  <button className="secondary-button" type="button" disabled={call !== 'idle' && call !== 'ended' && call !== 'failed'} onClick={() => onDial(entry.uri)} aria-label={`Call ${userOf(entry.uri)}`}>
                    Call
                  </button>
                </li>
              ))}
            </ul>
          </>
        )}
      </section>
    </div>
  );
}
