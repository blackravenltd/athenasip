import { Link } from 'react-router-dom';
import { routes } from '../app/routes';

/**
 * Tools for proving the server works, as distinct from configuring it. The
 * softphone is one: it registers over WebSocket and places a call through the
 * RTP relay.
 */
export function DiagnosticsScreen() {
  return (
    <>
      <h1>Diagnostics</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Tools</h2>
            <p>Ways to exercise this server from the browser, rather than to configure it.</p>
          </div>
        </div>
        <div className="panel-actions">
          <Link className="secondary-button" to={routes.softphone}>WebRTC softphone</Link>
        </div>
      </section>
    </>
  );
}
