// @vitest-environment jsdom
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { signedInAs } from '../test/signedIn';
import { UsersScreen, describeLastLogin } from './UsersScreen';

function dialogue(): HTMLElement {
  return screen.getByRole('dialog');
}

describe('describeLastLogin', () => {
  it('says never, rather than 1970', () => {
    expect(describeLastLogin(0, 1_000_000)).toBe('Never signed in');
    expect(describeLastLogin(1_000_000 - 7200, 1_000_000)).toBe('Signed in 2h ago');
  });
});

describe('UsersScreen', () => {
  it('lists users with their roles, and marks you and the disabled', async () => {
    const { api } = await signedInAs('admin');
    render(<UsersScreen api={api} username="admin" />);
    expect((await screen.findByRole('group', { name: 'ops' })).textContent).toContain('View cluster status');
    expect((await screen.findByRole('group', { name: 'newhire' })).textContent).toContain('No roles');
    expect(screen.getByText('You')).toBeTruthy();
    expect(screen.getByText('Disabled')).toBeTruthy();
    expect(screen.getByRole('button', { name: 'Delete admin' })).toHaveProperty('disabled', true);
  });

  it('adds a user with exactly the roles ticked, none by default', async () => {
    const { api } = await signedInAs('admin');
    render(<UsersScreen api={api} username="admin" />);
    fireEvent.click(await screen.findByRole('button', { name: 'Add a user' }));
    fireEvent.change(within(dialogue()).getByLabelText('Username'), { target: { value: 'auditor' } });
    fireEvent.change(within(dialogue()).getByLabelText('Password'), { target: { value: 'pw' } });
    expect(within(dialogue()).getByText(/can sign in and do nothing/)).toBeTruthy();
    fireEvent.click(within(dialogue()).getByLabelText('View cluster status'));
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Add user' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    const created = (await api.listUsers()).find((user) => user.username === 'auditor');
    expect(created?.roles).toEqual(['view-cluster-status']);
  });

  it('will not offer to disable you or remove your own right to manage users', async () => {
    const { api } = await signedInAs('admin');
    render(<UsersScreen api={api} username="admin" />);
    fireEvent.click(await screen.findByRole('button', { name: 'Edit admin' }));
    expect(within(dialogue()).getByLabelText('Manage users')).toHaveProperty('disabled', true);
    expect(within(dialogue()).getByLabelText('Disabled')).toHaveProperty('disabled', true);
  });

  it('disables another user', async () => {
    const { api } = await signedInAs('admin');
    render(<UsersScreen api={api} username="admin" />);
    fireEvent.click(await screen.findByRole('button', { name: 'Edit ops' }));
    fireEvent.click(within(dialogue()).getByLabelText('Disabled'));
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Save user' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect((await api.listUsers()).find((user) => user.username === 'ops')?.disabled).toBe(true);
  });

  it('keeps Manage trunks when another role is ticked', async () => {
    const { api } = await signedInAs('admin');
    await api.updateUser('ops', { roles: ['view-cluster-status', 'manage-trunks'] });
    render(<UsersScreen api={api} username="admin" />);
    fireEvent.click(await screen.findByRole('button', { name: 'Edit ops' }));
    expect(within(dialogue()).getByLabelText('Manage trunks')).toHaveProperty('checked', true);
    fireEvent.click(within(dialogue()).getByLabelText('Manage realms'));
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Save user' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect((await api.listUsers()).find((user) => user.username === 'ops')?.roles)
      .toEqual(['view-cluster-status', 'manage-realms', 'manage-trunks']);
  });
});
