import type { IceServer } from '../api/types';

/**
 * The node's `ice_servers` in the form `RTCPeerConnection` takes.
 *
 * A `turn:` or `turns:` entry is dropped when it has no credential (the node
 * has no shared secret) or its credential has expired. `expires_at` is the
 * node's field, not WebRTC's, and is not passed on.
 */
export function usableIceServers(servers: readonly IceServer[], now: number = Date.now()): RTCIceServer[] {
  const usable: RTCIceServer[] = [];
  for (const { urls, username, credential, expires_at: expiresAt } of servers) {
    if (/^turns?:/i.test(urls)) {
      if (!username || !credential) continue;
      if (expiresAt !== undefined && expiresAt * 1000 <= now) continue;
      usable.push({ urls, username, credential });
    } else {
      usable.push({ urls });
    }
  }
  return usable;
}
