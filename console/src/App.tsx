import { Suspense, lazy, useState, type ReactNode } from 'react';
import { Link, Navigate, Route, Routes, useLocation } from 'react-router-dom';
import type { AdminApi } from './api/AdminApi';
import type { Role } from './api/types';
import { SectionNav } from './components/SectionNav';
import { sectionNavFor, topNav, visible } from './app/navigation';
import { routes } from './app/routes';
import logoUrl from './assets/athenasip_small_white.svg?url';
import type { Session } from './auth/Session';
import { useSession } from './auth/useSession';
import { LoginScreen } from './auth/LoginScreen';
import { SessionExpiry } from './auth/SessionExpiry';
import { NoPermissionsScreen } from './auth/NoPermissionsScreen';
import { ROLE_TEXT, can, describeWho } from './auth/roles';
import { OverviewScreen } from './screens/OverviewScreen';
import { RealmsScreen } from './screens/RealmsScreen';
import { SubscribersScreen } from './screens/SubscribersScreen';
import { UsersScreen } from './screens/UsersScreen';
import { MeScreen } from './screens/MeScreen';
import { RegistrationsScreen } from './screens/RegistrationsScreen';
import { MediaScreen } from './screens/MediaScreen';
import { CallsScreen } from './screens/CallsScreen';
import { SecurityScreen, TlsScreen } from './screens/SecurityScreen';
import { SettingsScreen } from './screens/SettingsScreen';
import { DiagnosticsScreen } from './screens/DiagnosticsScreen';
import { NotFoundScreen } from './screens/NotFoundScreen';
import { Loading } from './components/Status';
import { pageOptions } from './softphone/page';

/** Loaded on demand: it pulls in JsSIP, which is most of the bundle. */
const SoftphoneScreen = lazy(async () => ({ default: (await import('./screens/SoftphoneScreen')).SoftphoneScreen }));

/** Loaded on demand for the same reason. */
const PhoneHost = lazy(() => import('./phone/PhoneHost'));

/** Renders `children` if any of `roles` is held, and otherwise says which role is needed. */
function Require({ roles, held, children }: { roles: readonly Role[]; held: readonly Role[]; children: ReactNode }) {
  if (can(held, ...roles)) return <>{children}</>;
  const names = roles.map((role) => ROLE_TEXT[role].label).join(' or ');
  return (
    <section className="panel">
      <p className="error-message" role="alert">
        This needs the {names} role, and you do not have it. Ask somebody with the Manage users role
        to give it to you.
      </p>
    </section>
  );
}

/**
 * The application shell: top bar, section bar, and the routed screen, or the
 * sign-in screen while there is no session.
 *
 * Screens take their `AdminApi` as a prop, so a test can render one against a
 * `FakeAdminApi`.
 */
export default function App({ api, session, loginHint }: { api: AdminApi; session: Session; loginHint?: string }) {
  const location = useLocation();
  const { token, info, expiresAt, ended } = useSession(session);
  const roles = info?.roles ?? [];
  const signedIn = Boolean(token && info);
  const section = signedIn ? sectionNavFor(location.pathname, roles) : undefined;
  const guard = (needed: readonly Role[], element: ReactNode) => <Require roles={needed} held={roles}>{element}</Require>;
  // The SIP section root redirects to the first screen in it the user may use.
  const sipHome = can(roles, 'manage-realms') ? routes.realms : can(roles, 'manage-realm-subscribers') ? routes.subscribers : routes.registrations;
  // Once opened, the phone stays mounted until sign-out, so a call outlives a change of screen.
  const onPhone = location.pathname === routes.phone;
  const [phoneOpened, setPhoneOpened] = useState(false);
  if (signedIn && onPhone && !phoneOpened) setPhoneOpened(true);
  if (!signedIn && phoneOpened) setPhoneOpened(false);
  const [callSlot, setCallSlot] = useState<HTMLElement | null>(null);

  const logOut = () => {
    // The token is forgotten here even if the node cannot be reached.
    void api.logout().catch(() => undefined).finally(() => session.signOut());
  };

  return (
    <div className="app-shell">
      <img className="app-watermark" src={logoUrl} alt="" aria-hidden="true" />

      <header className="topbar">
        <Link className="brand-link" to={routes.home}>
          <img className="app-logo" src={logoUrl} alt="" aria-hidden="true" />
          <span className="brand-name">AthenaSIP</span>
        </Link>
        {signedIn && roles.length > 0 ? <SectionNav ariaLabel="Sections" items={visible(topNav, roles)} className="" /> : <span />}
        <div className="topbar-trailing">
          <span className="topbar-call-slot" ref={setCallSlot} />
          {signedIn && info && (
            <>
              <SessionExpiry expiresAt={expiresAt} />
              <Link className="session-who" to={routes.me}>{describeWho(info)}</Link>
              <button className="secondary-button" type="button" onClick={logOut}>Log out</button>
            </>
          )}
        </div>
      </header>

      {section && section.items.length > 0 && (
        <div className="section-nav-slot">
          <SectionNav ariaLabel={section.ariaLabel} items={section.items} className="section-subnav" />
        </div>
      )}

      <main>
        <div className="admin-screen">
          {!signedIn || !info ? (
            // Rendered in place of the requested screen, so signing in lands there.
            <LoginScreen api={api} session={session} ended={ended} hint={loginHint} />
          ) : roles.length === 0 && location.pathname !== routes.me && !onPhone ? (
            <NoPermissionsScreen info={info} />
          ) : (
            <Routes>
              <Route path={routes.home} element={<OverviewScreen api={api} />} />

              <Route path={routes.sip} element={<Navigate to={sipHome} replace />} />
              <Route path={routes.realms} element={guard(['manage-realms'], <RealmsScreen api={api} />)} />
              <Route path={routes.subscribers} element={guard(['manage-realm-subscribers'], <SubscribersScreen api={api} />)} />
              <Route path={routes.registrations} element={guard(['view-cluster-status'], <RegistrationsScreen api={api} />)} />

              <Route path={routes.calls} element={guard(['view-cluster-status'], <CallsScreen api={api} />)} />

              {/* Drawn by PhoneHost, below. */}
              <Route path={routes.phone} element={null} />

              <Route path={routes.media} element={guard(['manage-realms', 'view-cluster-status'], <MediaScreen api={api} />)} />

              <Route path={routes.security} element={guard(['view-cluster-status'], <SecurityScreen api={api} />)} />
              <Route path={routes.securityTls} element={guard(['view-cluster-status'], <TlsScreen api={api} />)} />

              <Route path={routes.users} element={guard(['manage-admin-users'], <UsersScreen api={api} username={info.username} />)} />
              <Route path={routes.me} element={<MeScreen api={api} info={info} expiresAt={expiresAt} onPasswordChanged={() => session.signOut('Your password was changed. Sign in with the new one.')} />} />

              <Route path={routes.settings} element={<SettingsScreen />} />

              <Route path={routes.diagnostics} element={<DiagnosticsScreen />} />
              <Route
                path={routes.softphone}
                element={<Suspense fallback={<Loading />}><SoftphoneScreen api={api} options={pageOptions(location.search)} /></Suspense>}
              />

              <Route path="*" element={<NotFoundScreen />} />
            </Routes>
          )}
          {signedIn && phoneOpened && (
            <Suspense fallback={onPhone ? <Loading /> : null}>
              <PhoneHost api={api} roles={roles} visible={onPhone} indicator={callSlot} />
            </Suspense>
          )}
        </div>
      </main>
    </div>
  );
}
