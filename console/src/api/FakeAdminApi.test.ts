import { describe, expect, it } from 'vitest';
import { FakeAdminApi } from './FakeAdminApi';
import { securedNode, signedInAs } from '../test/signedIn';

describe('FakeAdminApi', () => {
  it('rejects a duplicate realm as a conflict rather than creating two', async () => {
    const api = new FakeAdminApi();
    await expect(api.createRealm({ name: 'sip.athenasip.org' })).rejects.toMatchObject({ status: 409, code: 'conflict' });
  });

  it('gives a new realm the server defaults for what it was not given', async () => {
    const api = new FakeAdminApi();
    const realm = await api.createRealm({ name: 'sip.example.org', registration_minimum: 30 });
    expect(realm).toMatchObject({
      nonce_expiry: 3600, registration_timeout: 5000, registration_minimum: 30,
      behaviour: { media_anchor: null, media_profile: null, qualify_interval: null, rewrite_contact: null },
      behaviour_effective: { media_anchor: true, media_profile: 'mirror', qualify_interval: 0, rewrite_contact: false },
      behaviour_default: { media_anchor: true, media_profile: 'mirror', qualify_interval: 0, rewrite_contact: false },
    });
  });

  it('changes only the fields an update gives', async () => {
    const api = new FakeAdminApi();
    const before = (await api.listRealms()).find((realm) => realm.name === 'example.org')!;
    const after = await api.updateRealm('example.org', { behaviour: { media_anchor: false } });
    expect(after).toEqual({
      ...before,
      behaviour: { ...before.behaviour, media_anchor: false },
      behaviour_effective: { ...before.behaviour_effective, media_anchor: false },
    });
  });

  it('puts a setting back to the server’s on null, and leaves alone one it is not given', async () => {
    const api = new FakeAdminApi();
    const realm = await api.updateRealm('sip.athenasip.org', { behaviour: { media_profile: null } });
    expect(realm.behaviour).toEqual({ media_anchor: null, media_profile: null, qualify_interval: 60, rewrite_contact: null });
    expect(realm.behaviour_effective.media_profile).toBe('mirror');
  });

  it('refuses an unknown setting or value, and the old top-level fields, changing nothing', async () => {
    const api = new FakeAdminApi();
    const before = (await api.listRealms()).find((realm) => realm.name === 'sip.athenasip.org');
    await expect(api.updateRealm('sip.athenasip.org', { nonce_expiry: 60, behaviour: { media_profile: 'nonsense' as never } }))
      .rejects.toMatchObject({ status: 400 });
    await expect(api.updateRealm('sip.athenasip.org', { behaviour: { volume: 11 } as never })).rejects.toMatchObject({ status: 400 });
    await expect(api.updateRealm('sip.athenasip.org', { media_anchor: false } as never))
      .rejects.toMatchObject({ status: 400, message: 'media_anchor has moved into the behaviour section' });
    expect((await api.listRealms()).find((realm) => realm.name === 'sip.athenasip.org')).toEqual(before);
  });

  it('lists ended calls newest first, an unanswered one with no answer time, and refuses a limit out of range', async () => {
    const api = new FakeAdminApi();
    const records = await api.listCallRecords();
    expect(records.map((record) => record.id)).toEqual(['r1@192.168.1.24', 'r2@203.0.113.40', 'r3@192.168.1.24']);
    expect(records[1]).toMatchObject({ duration: 0, answered_at: null });
    expect(Date.parse(records[0].ended_at!) - Date.parse(records[0].answered_at!)).toBe(184_000);
    await expect(api.listCallRecords(1)).resolves.toHaveLength(1);
    await expect(api.listCallRecords(0)).rejects.toMatchObject({ status: 400 });
  });

  it('deletes a realm with its subscribers and their registrations, as the server does', async () => {
    const api = new FakeAdminApi();
    await api.deleteRealm('example.org');
    await expect(api.listSubscribers('example.org')).rejects.toMatchObject({ status: 404 });
    await api.createRealm({ name: 'example.org' });
    await expect(api.listSubscribers('example.org')).resolves.toHaveLength(0);
    await expect(api.listRegistrations('example.org')).resolves.toHaveLength(0);
  });

  it('changes a subscriber’s media profile without a password, and refuses a realm-only setting, changing nothing', async () => {
    const api = new FakeAdminApi();
    await expect(api.updateSubscriber('sip.athenasip.org', 'ada', { behaviour: { media_profile: 'srtp' } }))
      .resolves.toMatchObject({ behaviour: { media_profile: 'srtp' } });
    await expect(api.updateSubscriber('sip.athenasip.org', 'ada', { password: 'p', behaviour: { media_anchor: false } as never }))
      .rejects.toMatchObject({ status: 400 });
    await expect(api.updateSubscriber('sip.athenasip.org', 'ada', { behaviour: { media_profile: 'nonsense' as never } }))
      .rejects.toMatchObject({ status: 400 });
    expect((await api.listSubscribers('sip.athenasip.org')).find((subscriber) => subscriber.user === 'ada')?.behaviour.media_profile).toBe('srtp');
  });

  it('takes rewrite_contact as true, false or null, and refuses anything else', async () => {
    const api = new FakeAdminApi();
    await expect(api.updateRealm('example.org', { behaviour: { rewrite_contact: 'yes' as never } })).rejects.toMatchObject({ status: 400 });
    await expect(api.updateRealm('example.org', { behaviour: { rewrite_contact: null } }))
      .resolves.toMatchObject({ behaviour: { rewrite_contact: null }, behaviour_effective: { rewrite_contact: false } });
  });

  it('refuses a probe interval outside the node’s bounds, and takes 0 for never', async () => {
    const api = new FakeAdminApi();
    await expect(api.updateRealm('example.org', { behaviour: { qualify_interval: 4 } })).rejects.toMatchObject({ status: 400 });
    await expect(api.updateRealm('example.org', { behaviour: { qualify_interval: 86401 } })).rejects.toMatchObject({ status: 400 });
    await expect(api.updateRealm('example.org', { behaviour: { qualify_interval: 0 } }))
      .resolves.toMatchObject({ behaviour_effective: { qualify_interval: 0 } });
  });

  it('probes the clients of a realm that probes, and none once it stops', async () => {
    const api = new FakeAdminApi();
    expect((await api.listQualifiedClients()).map((client) => client.subscriber)).toEqual(['sip:ada@sip.athenasip.org', 'sip:adaweb@sip.athenasip.org']);
    await api.updateRealm('sip.athenasip.org', { behaviour: { qualify_interval: 0 } });
    await expect(api.listQualifiedClients()).resolves.toEqual([]);
  });

  it('takes a subscriber’s registrations with it', async () => {
    const api = new FakeAdminApi();
    await api.deleteSubscriber('sip.athenasip.org', 'ada');
    const subscribers = (await api.listRegistrations()).map((binding) => binding.subscriber);
    expect(subscribers).not.toContain('sip:ada@sip.athenasip.org');
  });

  it('reports registration times in Unix seconds', async () => {
    const api = new FakeAdminApi();
    const [binding] = await api.listRegistrations();
    const now = Date.now() / 1000;
    expect(binding.expires_at).toBeGreaterThan(now);
    expect(binding.expires_at).toBeLessThan(now + 3600);
  });

  it('never hands back its own objects, so a screen cannot mutate the store', async () => {
    const api = new FakeAdminApi();
    const first = await api.listRealms();
    first[0].name = 'tampered';
    expect((await api.listRealms())[0].name).not.toBe('tampered');
  });

  describe('secured', () => {
    it('refuses a request with no session with 401, and tells the session', async () => {
      const { api, session } = securedNode();
      session.signIn('nonsense', { username: 'x', display_name: '', roles: [] });
      await expect(api.listRealms()).rejects.toMatchObject({ status: 401, code: 'unauthorized' });
      expect(session.token).toBeUndefined();
    });

    it('gives one answer for a wrong password, an unknown name and a disabled user', async () => {
      const { api } = securedNode();
      for (const [username, password] of [['ops', 'wrong'], ['nobody', 'nobody'], ['former', 'former']]) {
        await expect(api.login(username, password)).rejects.toMatchObject({ status: 401, message: 'invalid username or password' });
      }
    });

    it('compares usernames without case', async () => {
      const { api } = securedNode();
      await expect(api.login('OPS', 'ops')).resolves.toMatchObject({ roles: ['view-cluster-status'] });
    });

    it('holds exactly the roles given: managing users does not let you read status', async () => {
      const { api } = await signedInAs('ops');
      await expect(api.nodes()).resolves.toHaveLength(2);
      await expect(api.listRealms()).rejects.toMatchObject({ status: 403, code: 'forbidden' });
      const managers = await signedInAs('helpdesk');
      await expect(managers.api.listRealms()).resolves.toHaveLength(2);
      await expect(managers.api.createRealm({ name: 'x.example.org' })).rejects.toMatchObject({ status: 403 });
      await expect(managers.api.nodes()).rejects.toMatchObject({ status: 403 });
    });

    it('re-reads roles on every request, so a role removed takes effect at once', async () => {
      const { api } = await signedInAs('ops');
      await expect(api.nodes()).resolves.toHaveLength(2);
      api.alterUserElsewhere('ops', { roles: [] });
      await expect(api.nodes()).rejects.toMatchObject({ status: 403 });
    });

    it('ends every session of a user who is disabled', async () => {
      const { api, session } = await signedInAs('ops');
      api.alterUserElsewhere('ops', { disabled: true });
      await expect(api.nodes()).rejects.toMatchObject({ status: 401 });
      expect(session.token).toBeUndefined();
    });

    it('knows no bearer but a session from a login', async () => {
      const { api } = securedNode();
      await expect(api.session('config-admin')).rejects.toMatchObject({ status: 401 });
    });

    it('leaves health open', async () => {
      await expect(securedNode().api.health()).resolves.toMatchObject({ status: 'ok' });
    });

    it('creates users with no roles by default, and refuses a taken name whatever its case', async () => {
      const { api } = await signedInAs('admin');
      await expect(api.createUser({ username: 'Ada', display_name: 'Ada', password: 'p', roles: [] })).resolves.toMatchObject({ roles: [], last_login_at: 0 });
      await expect(api.createUser({ username: 'ada', display_name: '', password: 'p', roles: [] })).rejects.toMatchObject({ status: 409 });
    });

    it('signs a user out everywhere whatever the case of the name, succeeds for one never signed in, and 404s for nobody', async () => {
      const { api } = await signedInAs('admin');
      const { token } = await api.login('ops', 'ops');
      await expect(api.revokeSessions('OPS')).resolves.toBeUndefined();
      await expect(api.session(token)).rejects.toMatchObject({ status: 401 });
      await expect(api.revokeSessions('helpdesk')).resolves.toBeUndefined();
      await expect(api.revokeSessions('nobody')).rejects.toMatchObject({ status: 404 });
    });

    it('will not let you disable, demote or delete yourself', async () => {
      const { api } = await signedInAs('admin');
      await expect(api.updateUser('admin', { disabled: true })).rejects.toMatchObject({ status: 409, code: 'would_lock_out' });
      await expect(api.updateUser('admin', { roles: ['manage-realms'] })).rejects.toMatchObject({ status: 409, code: 'would_lock_out' });
      await expect(api.deleteUser('admin')).rejects.toMatchObject({ status: 409, code: 'would_lock_out' });
      await expect(api.updateUser('ops', { disabled: true })).resolves.toMatchObject({ disabled: true });
    });

    it('lets you change your own password with the old one, and nobody else’s without the role', async () => {
      const { api } = await signedInAs('ops');
      await expect(api.changePassword('ops', { password: 'new', old_password: 'wrong' })).rejects.toMatchObject({ status: 403, code: 'wrong_password' });
      await expect(api.changePassword('helpdesk', { password: 'x' })).rejects.toMatchObject({ status: 403 });
      await expect(api.changePassword('ops', { password: 'new', old_password: 'ops' })).resolves.toBeUndefined();
      // The change ends every session the user holds, the one that asked included.
      await expect(api.session()).rejects.toMatchObject({ status: 401 });
      await expect(securedNode().api.login('ops', 'new')).rejects.toMatchObject({ status: 401 });
    });
  });
});
