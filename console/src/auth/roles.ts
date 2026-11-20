import type { Role, SessionInfo } from '../api/types';
import { ROLES } from '../api/types';

/**
 * Role labels and descriptions, following the server's `docs/authentication.md`.
 *
 * No role implies another, so the console never infers one from another. The
 * node enforces roles with a 403; the console only hides what they do not permit.
 */
export const ROLE_TEXT: Record<Role, { label: string; detail: string }> = {
  'view-cluster-status': {
    label: 'View cluster status',
    detail: 'Read the node, its transports and who is registered. Changes nothing.',
  },
  'manage-realms': {
    label: 'Manage realms',
    detail: 'Create, change and remove realms, with their registration and media policy.',
  },
  'manage-realm-subscribers': {
    label: 'Manage subscribers',
    detail: 'Create, change and remove the subscribers in a realm.',
  },
  'manage-admin-users': {
    label: 'Manage users',
    detail: 'Create, change and remove the users who can sign in here, and their roles. A user with this can give themselves every other role.',
  },
  'manage-cluster': {
    label: 'Manage cluster',
    detail: 'End live calls and reload the routing scripts. Nothing in this console uses it yet.',
  },
  'manage-trunks': {
    label: 'Manage trunks',
    detail: 'Create, change and remove trunks and set their passwords, which the node keeps as given. Give it to as few users as possible. Nothing in this console uses it yet.',
  },
};

/** Whether any of `needed` is held. */
export function can(roles: readonly Role[], ...needed: Role[]): boolean {
  return needed.some((role) => roles.includes(role));
}

/** The role labels, comma-separated, in `ROLES` order. */
export function describeRoles(roles: readonly Role[]): string {
  return ROLES.filter((role) => roles.includes(role)).map((role) => ROLE_TEXT[role].label).join(', ');
}

/** The signed-in user's display name, falling back to the username. */
export function describeWho(info: SessionInfo): string {
  return info.display_name || info.username || 'Signed in';
}
