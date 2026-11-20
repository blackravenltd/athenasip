// @vitest-environment jsdom
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { describe, expect, it } from 'vitest';
import { FakeAdminApi } from '../api/FakeAdminApi';
import { RealmsScreen } from './RealmsScreen';

function show(api = new FakeAdminApi()) {
  render(<MemoryRouter><RealmsScreen api={api} /></MemoryRouter>);
  return api;
}

/** The open dialogue; only one is ever open. */
function dialogue(): HTMLElement {
  return screen.getByRole('dialog');
}

describe('RealmsScreen', () => {
  it('lists what the node holds, with each realm’s registration and media policy', async () => {
    show();
    const row = await screen.findByRole('group', { name: 'example.org' });
    expect(row.textContent).toContain('Registrations 1m to 5000s, nonces last 1h');
    expect(await screen.findByText('WebRTC')).toBeTruthy();
  });

  it('adds a realm with the policy given and shows it without a reload', async () => {
    const api = show();
    fireEvent.click(await screen.findByRole('button', { name: 'Add a realm' }));
    fireEvent.change(within(dialogue()).getByLabelText('Domain'), { target: { value: 'sip.example.org' } });
    fireEvent.change(within(dialogue()).getByLabelText('Shortest registration'), { target: { value: '120' } });
    const profile = within(dialogue()).getByLabelText('Media profile');
    // A new realm has no settings of its own, and still names the server's.
    expect(within(profile).getByRole('option', { name: 'Server default (mirror the caller)' })).toBeTruthy();
    fireEvent.change(profile, { target: { value: 'srtp' } });
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Add realm' }));

    expect(await screen.findByRole('group', { name: 'sip.example.org' })).toBeTruthy();
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    const created = (await api.listRealms()).find((realm) => realm.name === 'sip.example.org');
    expect(created).toMatchObject({ registration_minimum: 120, behaviour: { media_anchor: null, media_profile: 'srtp', rewrite_contact: null } });
  });

  it('turns Contact rewriting on for a realm, naming the server’s default', async () => {
    const api = show();
    fireEvent.click(await screen.findByRole('button', { name: 'Edit sip.athenasip.org' }));
    const rewrite = within(dialogue()).getByLabelText('Rewrite Contact to source address (NAT)');
    expect(within(rewrite).getByRole('option', { name: 'Server default (off)' })).toBeTruthy();
    fireEvent.change(rewrite, { target: { value: 'on' } });
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Save realm' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect((await api.listRealms()).find((realm) => realm.name === 'sip.athenasip.org')?.behaviour.rewrite_contact).toBe(true);
    expect((await screen.findByRole('group', { name: 'sip.athenasip.org' })).textContent).toContain('Contact rewritten to source');
  });

  it('will not submit a realm with no name', async () => {
    show();
    fireEvent.click(await screen.findByRole('button', { name: 'Add a realm' }));
    expect(within(dialogue()).getByRole('button', { name: 'Add realm' })).toHaveProperty('disabled', true);
  });

  it('will not submit a shortest registration longer than the longest, and says why', async () => {
    // The server would accept it, and every phone would then be refused.
    show();
    fireEvent.click(await screen.findByRole('button', { name: 'Add a realm' }));
    fireEvent.change(within(dialogue()).getByLabelText('Domain'), { target: { value: 'sip.example.org' } });
    fireEvent.change(within(dialogue()).getByLabelText('Shortest registration'), { target: { value: '9000' } });
    expect(within(dialogue()).getByRole('button', { name: 'Add realm' })).toHaveProperty('disabled', true);
    expect(within(dialogue()).getByText(/every phone would be refused/)).toBeTruthy();
  });

  it('shows a taken name against the name field, and keeps the dialogue open', async () => {
    show();
    fireEvent.click(await screen.findByRole('button', { name: 'Add a realm' }));
    const name = within(dialogue()).getByLabelText('Domain');
    fireEvent.change(name, { target: { value: 'sip.athenasip.org' } });
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Add realm' }));

    expect(await within(dialogue()).findByRole('alert')).toHaveProperty('textContent', 'realm sip.athenasip.org already exists');
    expect(name.getAttribute('aria-invalid')).toBe('true');
    expect(name.closest('label')?.textContent).toContain('already exists');
  });

  it('edits a realm without offering to rename it', async () => {
    const api = show();
    fireEvent.click(await screen.findByRole('button', { name: 'Edit example.org' }));
    expect(within(dialogue()).queryByLabelText('Domain')).toBeNull();
    fireEvent.change(within(dialogue()).getByLabelText('Relaying'), { target: { value: 'off' } });
    expect(within(dialogue()).getByLabelText('Media profile')).toHaveProperty('disabled', true);
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Save realm' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect((await api.listRealms()).find((realm) => realm.name === 'example.org')?.behaviour.media_anchor).toBe(false);
  });

  it('warns that a realm’s subscribers go with it before deleting it', async () => {
    show();
    fireEvent.click(await screen.findByRole('button', { name: 'Delete sip.athenasip.org' }));
    expect(await within(dialogue()).findByText(/Its 2 subscribers go with it/)).toBeTruthy();
    fireEvent.click(within(dialogue()).getByRole('button', { name: 'Delete realm' }));
    await waitFor(() => expect(screen.queryByRole('group', { name: 'sip.athenasip.org' })).toBeNull());
  });

  it('pages once there are more realms than fit, and not before', async () => {
    const api = new FakeAdminApi();
    for (let index = 0; index < 12; index++) await api.createRealm({ name: `realm-${index}.example.org` });
    show(api);

    expect(await screen.findByText('Page 1 of 2')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    expect(await screen.findByText('Page 2 of 2')).toBeTruthy();
    expect(screen.getByRole('button', { name: 'Next' })).toHaveProperty('disabled', true);
  });

  it('says the list is empty rather than showing a bare panel', async () => {
    const api = new FakeAdminApi();
    for (const realm of await api.listRealms()) await api.deleteRealm(realm.name);
    show(api);
    expect(await screen.findByText(/No realms yet/)).toBeTruthy();
  });
});
