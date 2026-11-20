import type { Role } from '../api/types';
import { can } from '../auth/roles';
import { routes } from './routes';

export interface NavItem {
  to: string;
  label: string;
  /** Match this path exactly rather than as a prefix. */
  end: boolean;
  /** Any one of these roles shows the link. Omitted: every signed-in user sees it. */
  requires?: readonly Role[];
}

/** The top-level sections, in the order they appear in the bar. */
export const topNav: readonly NavItem[] = [
  { to: routes.home, label: 'Overview', end: true },
  // No role needed: the phone registers as a subscriber, not with the user's roles.
  { to: routes.phone, label: 'Phone', end: false },
  { to: routes.sip, label: 'SIP', end: false, requires: ['manage-realms', 'manage-realm-subscribers', 'view-cluster-status'] },
  { to: routes.calls, label: 'Calls', end: false, requires: ['view-cluster-status'] },
  { to: routes.media, label: 'Media', end: false, requires: ['manage-realms', 'view-cluster-status'] },
  { to: routes.security, label: 'Security', end: false, requires: ['view-cluster-status'] },
  { to: routes.users, label: 'Users', end: false, requires: ['manage-admin-users'] },
  { to: routes.diagnostics, label: 'Diagnostics', end: false },
];

/** The sub-navigation of each section, keyed by the section root. */
const sectionNavs: ReadonlyArray<{ prefix: string; ariaLabel: string; items: readonly NavItem[] }> = [
  {
    prefix: routes.sip,
    ariaLabel: 'SIP navigation',
    items: [
      { to: routes.realms, label: 'Realms', end: true, requires: ['manage-realms'] },
      { to: routes.subscribers, label: 'Subscribers', end: true, requires: ['manage-realm-subscribers'] },
      { to: routes.registrations, label: 'Registrations', end: true, requires: ['view-cluster-status'] },
    ],
  },
  {
    prefix: routes.security,
    ariaLabel: 'Security navigation',
    items: [
      { to: routes.security, label: 'Overview', end: true },
      { to: routes.securityTls, label: 'TLS', end: true },
    ],
  },
  {
    prefix: routes.diagnostics,
    ariaLabel: 'Diagnostics navigation',
    items: [
      { to: routes.diagnostics, label: 'Overview', end: true },
      { to: routes.softphone, label: 'Softphone', end: true },
    ],
  },
];

/** The section bar for a path, or `undefined` when the path is in no section, so the shell omits the row. */
export function sectionNavFor(pathname: string, roles?: readonly Role[]): { ariaLabel: string; items: readonly NavItem[] } | undefined {
  // Match on a path boundary: `/mediaserver` is not inside `/media`.
  const found = sectionNavs.find(({ prefix }) => pathname === prefix || pathname.startsWith(`${prefix}/`));
  return found && { ariaLabel: found.ariaLabel, items: visible(found.items, roles) };
}

/** The items the given roles may use. With no roles given, all of them. */
export function visible(items: readonly NavItem[], roles?: readonly Role[]): readonly NavItem[] {
  if (!roles) return items;
  return items.filter((item) => !item.requires || can(roles, ...item.requires));
}
