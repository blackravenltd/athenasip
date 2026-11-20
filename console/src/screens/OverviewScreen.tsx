import { Link } from 'react-router-dom';
import type { AdminApi } from '../api/AdminApi';
import { ApiError } from '../api/errors';
import type { ClusterNode, Health, NodeTransport } from '../api/types';
import { isEncrypted } from '../api/types';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { ErrorMessage, Loading } from '../components/Status';
import { routes } from '../app/routes';

/** A node's health as a tone and a word. A degraded node is serving nothing: its datastore is gone. */
export function healthState(health: Health): { label: string; tone: 'ok' | 'down' } {
  return health.status === 'ok'
    ? { label: 'Serving', tone: 'ok' }
    : { label: 'Degraded: the datastore is not connected', tone: 'down' };
}

/** A 403 means the user lacks the role, which is not a fault. */
function notForThisLogin(error: Error | undefined): boolean {
  return error instanceof ApiError && error.isForbidden;
}

/** A node's state as a tone and a word. A stale report is not trusted, whatever status it carries. */
export function nodeState(node: ClusterNode): { label: string; tone: 'ok' | 'warn' | 'down' } {
  if (node.stale) return { label: 'No recent report', tone: 'warn' };
  switch (node.status) {
    case 'ok': return { label: 'Serving', tone: 'ok' };
    case 'degraded': return { label: 'Degraded', tone: 'warn' };
    case 'stopped': return { label: 'Stopped', tone: 'down' };
    case 'down': return { label: 'Down', tone: 'down' };
    default: return { label: 'Unknown', tone: 'warn' };
  }
}

/** The node that answered; each node marks itself in its own list. */
export function selfNode(nodes: readonly ClusterNode[]): ClusterNode | undefined {
  return nodes.find((node) => node.self) ?? nodes[0];
}

/**
 * What this node is and whether it is serving.
 *
 * Health needs no role; the node list and registrations need Status, and the
 * realm count Manage users. Each panel loads on its own, so a user lacking a
 * role still sees the rest.
 */
export function OverviewScreen({ api }: { api: AdminApi }) {
  const health = useRefreshableAsync((signal) => api.health(signal), [api]);
  const nodes = useRefreshableAsync((signal) => api.nodes(signal), [api]);
  const registrations = useRefreshableAsync((signal) => api.listRegistrations(undefined, signal), [api]);
  const realms = useRefreshableAsync((signal) => api.listRealms(signal), [api]);

  const refresh = () => { health.refresh(); nodes.refresh(); registrations.refresh(); realms.refresh(); };
  const refreshing = health.refreshing || nodes.refreshing || registrations.refreshing || realms.refreshing;
  const self = nodes.value && selfNode(nodes.value);

  return (
    <>
      <h1>Overview</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>This node</h2>
            <p>What this AthenaSIP process is, and whether it can serve a registration.</p>
          </div>
          <button className="secondary-button" type="button" onClick={refresh} disabled={refreshing}>
            {refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {health.error && <ErrorMessage error={health.error} />}
        {!health.value ? (health.loading && <Loading />) : (
          <dl className="readout">
            <dt>State</dt>
            <dd>
              <span className={`state-dot state-${healthState(health.value).tone}`} aria-hidden="true" />{' '}
              {healthState(health.value).label}
            </dd>
            <dt>Node</dt><dd>{health.value.node}</dd>
            <dt>Version</dt><dd>{health.value.version}</dd>
            <dt>Datastore</dt><dd><code>{health.value.datastore}</code></dd>
            <dt>Realms</dt>
            <dd>{realms.value ? realms.value.length : notForThisLogin(realms.error) ? 'Not available to this login' : '-'}</dd>
            <dt>Registrations</dt>
            <dd>
              {registrations.value
                ? <Link to={routes.registrations}>{registrations.value.length}</Link>
                : notForThisLogin(registrations.error) ? 'Not available to this login' : '-'}
            </dd>
          </dl>
        )}
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Cluster</h2>
            <p>
              Every node serving these realms, as each last described itself. This node first;
              the others as they reported on the event bus. A node whose reports stopped is still
              listed, so it can be told from one never heard of.
            </p>
          </div>
        </div>

        {notForThisLogin(nodes.error)
          ? <p className="field-hint">This login&apos;s roles do not include the node&apos;s status.</p>
          : nodes.error && <ErrorMessage error={nodes.error} />}
        {!nodes.value ? (nodes.loading && <Loading />) : (
          <ul className="record-list">
            {nodes.value.map((node) => {
              const state = nodeState(node);
              return (
                <li className="record-row" key={node.id}>
                  <div className="record-main" role="group" aria-label={`Node ${node.id}`}>
                    <span className="record-name">
                      <span className={`state-dot state-${state.tone}`} aria-hidden="true" />
                      {node.id}
                    </span>
                    {node.at && <span className="record-detail">Reported {new Date(node.at).toLocaleString()}</span>}
                  </div>
                  {node.self && <span className="record-tag">This node</span>}
                  {node.version && <span className="record-tag">{node.version}</span>}
                  <span className={`record-tag state-tag-${state.tone}`}>{state.label}</span>
                </li>
              );
            })}
          </ul>
        )}
      </section>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Transports</h2>
            <p>
              The listeners this node advertises, with the URI a client would use for each. A
              transport that is switched off is not listed. The address is{' '}
              <code>sip.public_address</code> when it is set; a node that shows{' '}
              <code>0.0.0.0</code> here has not been told the address clients reach it on.
            </p>
          </div>
        </div>

        {notForThisLogin(nodes.error)
          ? <p className="field-hint">This login&apos;s roles do not include the node&apos;s status.</p>
          : nodes.error && <ErrorMessage error={nodes.error} />}
        {!self ? (nodes.loading && <Loading />) : (
          <ul className="record-list">
            {self.transports.map((transport) => <TransportRow key={transport.transport} transport={transport} />)}
          </ul>
        )}
      </section>
    </>
  );
}

function TransportRow({ transport }: { transport: NodeTransport }) {
  const encrypted = isEncrypted(transport.transport);
  return (
    <li className="record-row">
      <div className="record-main" role="group" aria-label={transport.transport.toUpperCase()}>
        <span className="record-name">
          <span className={`state-dot state-${encrypted ? 'ok' : 'warn'}`} aria-hidden="true" />
          {transport.transport.toUpperCase()}
        </span>
        <span className="record-detail"><code>{transport.uri}</code></span>
      </div>
      <span className={`record-tag state-tag-${encrypted ? 'ok' : 'warn'}`}>{encrypted ? 'Encrypted' : 'Unencrypted'}</span>
    </li>
  );
}
