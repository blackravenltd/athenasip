import type { AdminApi } from '../api/AdminApi';
import { isEncrypted } from '../api/types';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { ErrorMessage, Loading } from '../components/Status';
import { selfNode } from './OverviewScreen';

/**
 * Whether anything unencrypted is listening. AthenaSIP is TLS-first and
 * plaintext is opt-in; the node list says which transports are up.
 */
export function SecurityScreen({ api }: { api: AdminApi }) {
  const result = useRefreshableAsync((signal) => api.nodes(signal), [api]);
  const self = result.value && selfNode(result.value);
  const plaintext = (self?.transports ?? []).filter((transport) => !isEncrypted(transport.transport));

  return (
    <>
      <h1>Security</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Transport encryption</h2>
            <p>
              AthenaSIP is TLS-first: unencrypted SIP has to be turned on deliberately, with{' '}
              <code>allow_unencrypted</code>. Anything listed here is carrying signalling in the
              clear.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={result.refresh} disabled={result.refreshing}>
            {result.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {result.error && <ErrorMessage error={result.error} />}
        {result.loading ? <Loading /> : self && (
          plaintext.length === 0 ? (
            <p className="field-hint" role="status">
              <span className="state-dot state-ok" aria-hidden="true" /> No unencrypted transport is enabled.
            </p>
          ) : (
            <ul className="record-list">
              {plaintext.map((transport) => (
                <li className="record-row" key={transport.transport}>
                  <div className="record-main" role="group" aria-label={transport.transport.toUpperCase()}>
                    <span className="record-name">
                      <span className="state-dot state-warn" aria-hidden="true" />
                      {transport.transport.toUpperCase()}
                    </span>
                    <span className="record-detail">{transport.address}:{transport.port}</span>
                  </div>
                  <span className="record-tag state-tag-warn">Unencrypted</span>
                </li>
              ))}
            </ul>
          )
        )}
      </section>
    </>
  );
}

export function TlsScreen({ api }: { api: AdminApi }) {
  const result = useRefreshableAsync((signal) => api.nodes(signal), [api]);
  const self = result.value && selfNode(result.value);
  const secure = (self?.transports ?? []).filter((transport) => isEncrypted(transport.transport));

  return (
    <>
      <h1>TLS</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Encrypted listeners</h2>
            <p>
              The TLS and secure WebSocket listeners this node advertises. The certificate and
              cipher policy are read from the node's configuration; the API does not expose them
              yet, so they cannot be shown or changed from here.
            </p>
          </div>
        </div>

        {result.error && <ErrorMessage error={result.error} />}
        {result.loading ? <Loading /> : self && (
          secure.length === 0 ? (
            <p className="field-hint">This node advertises no TLS or WSS listener.</p>
          ) : (
            <dl className="readout">
              {secure.map((transport) => (
                <div key={transport.transport} style={{ display: 'contents' }}>
                  <dt>{transport.transport.toUpperCase()}</dt>
                  <dd><code>{transport.uri}</code></dd>
                </div>
              ))}
            </dl>
          )
        )}
      </section>
    </>
  );
}
