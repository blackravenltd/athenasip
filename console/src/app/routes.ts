/**
 * Every path this client knows. Links and routes both use these constants;
 * nothing builds a path by concatenation at a call site.
 */
export const routes = {
  home: '/',

  sip: '/sip',
  realms: '/sip/realms',
  subscribers: '/sip/subscribers',
  registrations: '/sip/registrations',

  calls: '/calls',

  phone: '/phone',

  media: '/media',

  security: '/security',
  securityTls: '/security/tls',

  users: '/users',
  me: '/me',

  settings: '/settings',

  diagnostics: '/diagnostics',
  softphone: '/diagnostics/softphone',
} as const;

/** The subscribers screen filtered to one realm. */
export function realmSubscribers(realm: string): string {
  return `${routes.subscribers}?realm=${encodeURIComponent(realm)}`;
}
