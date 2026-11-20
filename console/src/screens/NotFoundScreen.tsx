import { Link, useLocation } from 'react-router-dom';
import { routes } from '../app/routes';

export function NotFoundScreen() {
  const location = useLocation();

  return (
    <>
      <h1>Not found</h1>
      <section className="panel">
        <p className="muted">
          There is nothing at <code>{location.pathname}</code>.
        </p>
        <div className="panel-actions">
          <Link className="secondary-button" to={routes.home}>Back to the overview</Link>
        </div>
      </section>
    </>
  );
}
