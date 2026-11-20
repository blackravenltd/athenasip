import type { AdminApi } from '../api/AdminApi';
import type { QualifiedClient, Registration } from '../api/types';
import { MEDIA_PROFILE_TEXT, describeSeconds } from '../realms/policy';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { Empty, ErrorMessage, Loading } from '../components/Status';

/**
 * How long a binding has left, in Unix seconds as the server gives them.
 * Close to expiry is only a warning, because phones refresh at half the
 * interval.
 */
export function describeExpiry(expiresAt: number, nowSeconds: number): { label: string; tone: 'ok' | 'warn' | 'down' } {
  const remaining = Math.round(expiresAt - nowSeconds);
  if (remaining <= 0) return { label: 'Expired', tone: 'down' };
  if (remaining < 60) return { label: `${remaining}s left`, tone: 'warn' };
  return { label: `${Math.round(remaining / 60)}m left`, tone: 'ok' };
}

/** The transport a contact names; the API gives it only in the URI. */
export function contactTransport(contact: string): string | undefined {
  return /;transport=([a-z]+)/i.exec(contact)?.[1]?.toUpperCase();
}

/** Whether a probed client is answering. One missed probe may be a lost packet; three is a client gone. */
export function describeProbe(client: QualifiedClient, now: number): { label: string; tone: 'ok' | 'warn' | 'down' } {
  if (client.unanswered >= 3) return { label: `${client.unanswered} unanswered`, tone: 'down' };
  if (client.unanswered > 0) return { label: `${client.unanswered} unanswered`, tone: 'warn' };
  if (!client.answered_at) return { label: 'Not yet answered', tone: 'warn' };
  const ago = Math.max(0, Math.round((now - Date.parse(client.answered_at)) / 1000));
  return { label: `Answered ${describeSeconds(ago)} ago`, tone: 'ok' };
}

function key(registration: Registration): string {
  return `${registration.subscriber} ${registration.contact}`;
}

export function RegistrationsScreen({ api }: { api: AdminApi }) {
  const result = useRefreshableAsync((signal) => api.listRegistrations(undefined, signal), [api]);
  const probed = useRefreshableAsync((signal) => api.listQualifiedClients(signal), [api]);
  const now = Date.now() / 1000;

  return (
    <>
      <h1>Registrations</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Live contact bindings</h2>
            <p>
              Where this node currently believes each subscriber can be reached. Bindings are made
              by phones registering, so there is nothing to edit here. A binding marked NAT is
              on a private address, so the node answers it down the connection it arrived on.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={result.refresh} disabled={result.refreshing}>
            {result.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {result.error && <ErrorMessage error={result.error} />}

        {result.loading ? <Loading /> : !result.value ? null : result.value.length === 0 ? (
          <Empty>Nothing is registered. A phone that registers successfully will appear here.</Empty>
        ) : (
          <ul className="record-list">
            {result.value.map((registration) => {
              const expiry = describeExpiry(registration.expires_at, now);
              const transport = contactTransport(registration.contact);
              return (
                <li className="record-row" key={key(registration)}>
                  <div className="record-main" role="group" aria-label={registration.subscriber}>
                    <span className="record-name">
                      <span className={`state-dot state-${expiry.tone}`} aria-hidden="true" />
                      {registration.subscriber}
                    </span>
                    <span className="record-detail"><code>{registration.contact}</code></span>
                    {registration.path && <span className="record-detail">Path <code>{registration.path}</code></span>}
                  </div>
                  {transport && <span className="record-tag">{transport}</span>}
                  {registration.nat && <span className="record-tag">NAT</span>}
                  {registration.node_id && <span className="record-tag">{registration.node_id}</span>}
                  <span className={`record-tag state-tag-${expiry.tone}`}>{expiry.label}</span>
                </li>
              );
            })}
          </ul>
        )}
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Probed clients</h2>
            <p>
              The registered clients this node sends OPTIONS to, in realms that probe. A client
              that stops answering has gone, or its connection has. One that answers with a
              session description says what media it takes, which is what a subscriber&apos;s
              media profile should be if its realm&apos;s gets it wrong.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={probed.refresh} disabled={probed.refreshing}>
            {probed.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {probed.error && <ErrorMessage error={probed.error} />}
        {!probed.value ? (probed.loading && <Loading />) : probed.value.length === 0 ? (
          <Empty>Nothing is being probed. A realm probes its clients when it is given an interval.</Empty>
        ) : (
          <ul className="record-list">
            {probed.value.map((client) => {
              const probe = describeProbe(client, now * 1000);
              return (
                <li className="record-row" key={`${client.subscriber} ${client.contact}`}>
                  <div className="record-main" role="group" aria-label={`Probe of ${client.subscriber}`}>
                    <span className="record-name">
                      <span className={`state-dot state-${probe.tone}`} aria-hidden="true" />
                      {client.subscriber}
                    </span>
                    <span className="record-detail"><code>{client.contact}</code></span>
                    <span className="record-detail">Every {describeSeconds(client.interval)}</span>
                  </div>
                  {client.said_media_profile && (
                    <span className="record-tag">Says {MEDIA_PROFILE_TEXT[client.said_media_profile].label}</span>
                  )}
                  <span className={`record-tag state-tag-${probe.tone}`}>{probe.label}</span>
                </li>
              );
            })}
          </ul>
        )}
      </section>
    </>
  );
}
