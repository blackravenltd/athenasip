// @vitest-environment jsdom
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { describe, expect, it } from 'vitest';
import App from './App';
import { routes } from './app/routes';
import { securedNode, signedInAs } from './test/signedIn';
import type { FakeAdminApi } from './api/FakeAdminApi';
import type { Session } from './auth/Session';

function show(at: string, node: { api: FakeAdminApi; session: Session }) {
  render(
    <MemoryRouter initialEntries={[at]}>
      <App api={node.api} session={node.session} />
    </MemoryRouter>,
  );
  return node;
}

function signIn(username: string, password = username) {
  fireEvent.change(screen.getByLabelText('Username'), { target: { value: username } });
  fireEvent.change(screen.getByLabelText('Password'), { target: { value: password } });
  fireEvent.click(screen.getByRole('button', { name: 'Sign in' }));
}

describe('App', () => {
  it('marks the section you are in', async () => {
    show(routes.realms, await signedInAs('admin'));
    await waitFor(() => expect(screen.getByRole('link', { name: 'SIP' }).className).toContain('active'));
    const topbar = screen.getByRole('navigation', { name: 'Sections' });
    expect(within(topbar).getByRole('link', { name: 'Overview' }).className).not.toContain('active');
  });

  it('opens the phone for a user with no roles, since it signs in to a line of its own', async () => {
    show(routes.phone, await signedInAs('newhire'));
    expect(await screen.findByRole('heading', { name: 'Phone', level: 1 })).toBeTruthy();
    expect(screen.queryByRole('heading', { name: 'No permissions' })).toBeNull();
  });

  it('counts down in the top bar when the session is in its last minutes', async () => {
    show(routes.home, await signedInAs('ops', { sessionSeconds: 120 }));
    expect(screen.getByRole('status').textContent).toContain('Your session ends within five minutes');
    expect(screen.getByText(/Session ends in/).textContent).toMatch(/^Session ends in [12]:\d\d$/);
  });

  it('says on the user’s own screen when the session ends', async () => {
    show(routes.me, await signedInAs('ops'));
    expect(screen.queryByText(/Session ends in/)).toBeNull();
    expect(screen.getByText('Session ends').nextElementSibling!.textContent).not.toMatch(/log out/);
  });

  it('signs you out after changing your own password, and says so', async () => {
    show(routes.me, await signedInAs('ops'));
    fireEvent.change(screen.getByLabelText('Current password'), { target: { value: 'wrong' } });
    fireEvent.change(screen.getByLabelText('New password'), { target: { value: 'fresh' } });
    fireEvent.change(screen.getByLabelText('New password again'), { target: { value: 'fresh' } });
    fireEvent.click(screen.getByRole('button', { name: 'Change password' }));
    expect((await screen.findByRole('alert')).textContent).toBe('the old password is not right');

    fireEvent.change(screen.getByLabelText('Current password'), { target: { value: 'ops' } });
    fireEvent.click(screen.getByRole('button', { name: 'Change password' }));
    expect(await screen.findByText('Your password was changed. Sign in with the new one.')).toBeTruthy();
    signIn('ops', 'fresh');
    expect(await screen.findByRole('heading', { name: 'You', level: 1 })).toBeTruthy();
  });

  it('has no section bar on the overview', async () => {
    show(routes.home, await signedInAs('admin'));
    expect(screen.queryByRole('navigation', { name: 'SIP navigation' })).toBeNull();
  });

  it('resolves the SIP section root to the first screen the roles allow', async () => {
    show(routes.sip, await signedInAs('admin'));
    expect(await screen.findByRole('heading', { name: 'Realms', level: 1 })).toBeTruthy();
  });

  it('sends a status viewer to registrations, and hides what it cannot use', async () => {
    show(routes.sip, await signedInAs('ops'));
    expect(await screen.findByRole('heading', { name: 'Registrations', level: 1 })).toBeTruthy();
    expect(screen.queryByRole('link', { name: 'Realms' })).toBeNull();
    expect(screen.queryByRole('link', { name: 'Subscribers' })).toBeNull();
    expect(screen.queryByRole('link', { name: 'Users' })).toBeNull();
    expect(screen.getByRole('link', { name: 'Calls' })).toBeTruthy();
  });

  it('shows a status viewer the media engine, and says the realm policy is not theirs', async () => {
    show(routes.media, await signedInAs('ops'));
    expect(await screen.findByText('builtin, connected')).toBeTruthy();
    expect(await screen.findByText(/do not include managing realms/)).toBeTruthy();
  });

  it('sends a subscriber manager to subscribers, with the realms to choose from', async () => {
    show(routes.sip, await signedInAs('helpdesk'));
    expect(await screen.findByRole('heading', { name: 'Subscribers', level: 1 })).toBeTruthy();
    expect(await screen.findByRole('option', { name: 'sip.athenasip.org' })).toBeTruthy();
    expect(screen.queryByRole('link', { name: 'Realms' })).toBeNull();
  });

  it('says a screen needs a role rather than rendering a 403, when reached directly', async () => {
    show(routes.users, await signedInAs('helpdesk'));
    expect(await screen.findByText(/needs the Manage users role/)).toBeTruthy();
  });

  it('says an unknown path is unknown rather than showing the front page', async () => {
    show('/nowhere', await signedInAs('admin'));
    expect(await screen.findByRole('heading', { name: 'Not found', level: 1 })).toBeTruthy();
  });

  it('signs in with a username and password, landing on the screen that was asked for', async () => {
    show(routes.registrations, securedNode());
    signIn('ops');
    expect(await screen.findByRole('heading', { name: 'Registrations', level: 1 })).toBeTruthy();
    expect(screen.getByRole('link', { name: 'Operations' })).toBeTruthy();
  });

  it('gives one answer for a wrong password and a disabled user', async () => {
    show(routes.home, securedNode());
    signIn('ops', 'wrong');
    expect(await screen.findByText('That username and password were not accepted.')).toBeTruthy();
    signIn('former');
    await waitFor(() => expect(screen.getAllByText('That username and password were not accepted.')).toHaveLength(1));
    expect(screen.queryByRole('heading', { name: 'Overview', level: 1 })).toBeNull();
  });

  it('offers no way in but a username and password', async () => {
    show(routes.home, securedNode());
    expect(screen.queryByRole('button', { name: /configuration token/i })).toBeNull();
    expect(screen.queryByLabelText(/token/i)).toBeNull();
  });

  it('tells a user with no roles so, rather than showing an empty console', async () => {
    show(routes.realms, securedNode());
    signIn('newhire');
    expect(await screen.findByRole('heading', { name: 'No permissions', level: 1 })).toBeTruthy();
    expect(screen.queryByRole('navigation', { name: 'Sections' })).toBeNull();
    expect(screen.getByRole('button', { name: 'Log out' })).toBeTruthy();
  });

  it('takes away a screen when its role is removed mid-session', async () => {
    const node = show(routes.registrations, await signedInAs('ops'));
    expect(await screen.findByRole('group', { name: 'sip:ada@sip.athenasip.org' })).toBeTruthy();
    node.api.alterUserElsewhere('ops', { roles: [] });
    fireEvent.click(screen.getAllByRole('button', { name: 'Refresh' })[0]);
    expect(await screen.findByRole('heading', { name: 'No permissions', level: 1 })).toBeTruthy();
  });

  it('logs out on the node as well as here, and a 401 anywhere does the same and says why', async () => {
    const node = show(routes.home, await signedInAs('admin'));
    const token = node.session.token!;
    fireEvent.click(await screen.findByRole('button', { name: 'Log out' }));
    expect(await screen.findByRole('heading', { name: 'Sign in', level: 1 })).toBeTruthy();
    await expect(node.api.session(token)).rejects.toMatchObject({ status: 401 });

    act(() => node.session.signIn('revoked', { username: 'admin', display_name: 'Administrator', roles: ['view-cluster-status'] }));
    expect(await screen.findByText(/Signed out: a valid session is required/)).toBeTruthy();
  });
});
