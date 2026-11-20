import ReactDOM from 'react-dom/client';
import '@fontsource-variable/roboto/wght.css';
import { SoftphoneScreen } from '../screens/SoftphoneScreen';
import { pageOptions, withoutSecrets } from './page';
import '../styles.css';

/**
 * The softphone on a page of its own: the component the console shows under
 * Diagnostics, served by the node as a static file and driven by its query
 * string (see `page.ts`). It needs no router.
 */
const options = pageOptions(window.location.search);

// Take the password and any TURN credential out of the address bar.
const cleaned = withoutSecrets(window.location.href);
if (cleaned) window.history.replaceState(window.history.state, '', cleaned);

const rootElement = document.getElementById('root');
if (!rootElement) throw new Error('Missing #root element');

ReactDOM.createRoot(rootElement).render(
  <div className="app-shell softphone-page">
    <main>
      <div className="admin-screen">
        <SoftphoneScreen options={options} />
      </div>
    </main>
  </div>,
);
