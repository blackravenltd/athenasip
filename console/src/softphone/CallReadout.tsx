import type { ReactNode } from 'react';
import { videoLine, type MediaStats, type SoftphoneState } from './Softphone';
import { describeVideoLine } from './words';

/**
 * What was negotiated for one call and whether media is moving: the harness
 * view's Negotiation panel and the Phone's "Call details".
 *
 * The test ids are part of the harness contract. `extra` rows go after the
 * direction.
 */
export function CallReadout({ state, stats, extra }: { state: SoftphoneState; stats?: MediaStats; extra?: ReactNode }) {
  const pair = stats?.candidatePair;
  return (
    <>
      <dl className="readout" data-testid="softphone-negotiation">
        <dt>Direction</dt><dd>{state.direction ?? '-'}</dd>
        {extra}
        <dt>Signaling</dt><dd>{state.signalingState ?? '-'}</dd>
        <dt>ICE gathering</dt><dd>{state.iceGatheringState ?? '-'}</dd>
        <dt>ICE connection</dt><dd data-testid="softphone-ice">{state.iceConnectionState ?? '-'}</dd>
        <dt>Connection</dt><dd>{state.connectionState ?? '-'}</dd>
        <dt>DTLS</dt><dd>{stats?.dtlsState ?? '-'}</dd>
        <dt>Codec</dt><dd>{stats?.codec ?? '-'}</dd>
        {(videoLine(state.localSdp) || videoLine(state.remoteSdp)) && (
          <>
            <dt>Video</dt>
            <dd data-testid="softphone-video-negotiation">
              {`this end: ${describeVideoLine(videoLine(state.localSdp))}; far end: ${describeVideoLine(videoLine(state.remoteSdp))}`}
              {stats?.video ? `; ${stats.video.packetsSent} sent, ${stats.video.packetsReceived} received, ${stats.video.framesDecoded} frames decoded${stats.video.codec ? `, ${stats.video.codec}` : ''}` : ''}
            </dd>
          </>
        )}
        <dt>Candidate pair</dt>
        <dd>
          {pair
            ? `${pair.local.address}:${pair.local.port} (${pair.local.type}) to ${pair.remote.address}:${pair.remote.port} (${pair.remote.type}), ${pair.state}`
            : '-'}
        </dd>
        <dt>Packets</dt>
        <dd>
          {stats
            ? `${stats.packetsSent} sent, ${stats.packetsReceived} received, ${stats.packetsLost} lost`
            : '-'}
        </dd>
        {state.cause && (<><dt>Cause</dt><dd>{state.cause}</dd></>)}
      </dl>

      <details>
        <summary>Local description</summary>
        <pre className="sdp" data-testid="softphone-local-sdp">{state.localSdp ?? 'None yet.'}</pre>
      </details>
      <details>
        <summary>Remote description</summary>
        <pre className="sdp" data-testid="softphone-remote-sdp">{state.remoteSdp ?? 'None yet.'}</pre>
      </details>
    </>
  );
}
