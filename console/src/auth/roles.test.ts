import { describe, expect, it } from 'vitest';
import { can, describeRoles, describeWho } from './roles';

describe('can', () => {
  it('holds exactly the roles given, and infers nothing from any of them', () => {
    expect(can(['manage-admin-users'], 'manage-realms')).toBe(false);
    expect(can(['manage-realms'], 'view-cluster-status')).toBe(false);
    expect(can(['manage-realms'], 'manage-realms', 'manage-realm-subscribers')).toBe(true);
    expect(can([], 'view-cluster-status')).toBe(false);
  });
});

describe('describeRoles', () => {
  it('names the roles in words, in the console order', () => {
    expect(describeRoles(['manage-admin-users', 'view-cluster-status'])).toBe('View cluster status, Manage users');
  });
});

describe('describeWho', () => {
  it('names a person by display name, or username when there is none', () => {
    expect(describeWho({ username: 'ada', display_name: 'Ada Admin', roles: [] })).toBe('Ada Admin');
    expect(describeWho({ username: 'ada', display_name: '', roles: [] })).toBe('ada');
  });
});
