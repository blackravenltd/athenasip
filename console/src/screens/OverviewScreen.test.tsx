// @vitest-environment jsdom
import { render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { describe, expect, it } from 'vitest';
import { FakeAdminApi } from '../api/FakeAdminApi';
import type { AdminApi } from '../api/AdminApi';
import { signedInAs } from '../test/signedIn';
import { OverviewScreen, healthState, nodeState, selfNode } from './OverviewScreen';

describe('healthState', () => {
  it('says a degraded node is not serving, and why', () => {
    expect(healthState({ status: 'degraded', node: 'n', version: 'v', datastore: 'd' }).tone).toBe('down');
    expect(healthState({ status: 'ok', node: 'n', version: 'v', datastore: 'd' }).label).toBe('Serving');
  });
});

describe('selfNode', () => {
  it('picks the node that answered out of the list', () => {
    expect(selfNode([{ id: 'a', self: false, transports: [] }, { id: 'b', self: true, transports: [] }])?.id).toBe('b');
  });
});

describe('nodeState', () => {
  it('does not trust a stale report, whatever it says', () => {
    expect(nodeState({ id: 'a', self: false, status: 'ok', stale: true, transports: [] })).toEqual({ label: 'No recent report', tone: 'warn' });
    expect(nodeState({ id: 'a', self: true, status: 'ok', transports: [] }).tone).toBe('ok');
    expect(nodeState({ id: 'a', self: false, status: 'down', transports: [] }).tone).toBe('down');
  });
});

describe('OverviewScreen', () => {
  it('shows the node, its counts and its transports once it answers', async () => {
    render(<MemoryRouter><OverviewScreen api={new FakeAdminApi()} /></MemoryRouter>);
    expect((await screen.findAllByText('sip-0001')).length).toBeGreaterThan(0);
    expect(await screen.findByRole('group', { name: 'UDP' })).toBeTruthy();
    expect((await screen.findByRole('group', { name: 'TLS' })).textContent).toContain('sips:203.0.113.5:5061');
  });

  it('shows a subscriber manager what it can see, and says quietly what it cannot', async () => {
    const { api } = await signedInAs('helpdesk');
    render(<MemoryRouter><OverviewScreen api={api} /></MemoryRouter>);
    expect(await screen.findByText('sip-0001')).toBeTruthy();
    expect(await screen.findByText('Not available to this login')).toBeTruthy();
    expect(await screen.findAllByText(/roles do not include the node/)).toHaveLength(2);
    expect(await screen.findByText('2')).toBeTruthy();
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('lists the cluster, marking this node and one whose reports stopped', async () => {
    render(<MemoryRouter><OverviewScreen api={new FakeAdminApi()} /></MemoryRouter>);
    const self = await screen.findByRole('group', { name: 'Node sip-0001' });
    expect(self.parentElement!.textContent).toContain('This node');
    const quiet = screen.getByRole('group', { name: 'Node sip-0002' });
    expect(quiet.parentElement!.textContent).toContain('No recent report');
  });

  it('shows the failure rather than an empty panel when the node is unreachable', async () => {
    const unreachable = new Proxy({}, { get: () => () => Promise.reject(new TypeError('Failed to fetch')) }) as AdminApi;
    render(<MemoryRouter><OverviewScreen api={unreachable} /></MemoryRouter>);
    expect((await screen.findAllByRole('alert'))[0]).toHaveProperty('textContent', 'Failed to fetch');
  });
});
