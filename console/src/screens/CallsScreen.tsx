import { useEffect, useState } from 'react';
import type { AdminApi } from '../api/AdminApi';
import { retryAfter } from '../api/errors';
import type { Call, CallLeg, CallState } from '../api/types';
import { useRefreshableAsync } from '../hooks/useRefreshableAsync';
import { Empty, ErrorMessage, Loading } from '../components/Status';

/** How often the list is read again. The counters are cumulative, so this is also the window a rate is taken over. */
export const POLL_MS = 2000;

const STATE_TONE: Record<CallState, 'ok' | 'warn' | 'down'> = {
  Initial: 'warn',
  Trying: 'warn',
  Ringing: 'warn',
  Connected: 'ok',
  Closing: 'down',
  Closed: 'down',
};

/** Who called whom: the originator first, everybody else after. */
export function callParties(call: Call): { from: string; to: string } {
  const from = call.participants.find((participant) => participant.originator);
  const others = call.participants.filter((participant) => participant !== from);
  return {
    from: from?.identity ?? 'unknown caller',
    to: others.map((participant) => participant.identity).join(', ') || 'unknown callee',
  };
}

/** How long a call has been talking, or ringing when it has not been answered. */
export function callDuration(call: Call, now: number): string {
  const since = call.answered_at ?? call.created_at;
  if (!since) return '-';
  const seconds = Math.max(0, Math.floor((now - Date.parse(since)) / 1000));
  const clock = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  return call.answered_at ? clock : `${clock} unanswered`;
}

export type Flow = 'moving' | 'still' | 'unreported' | 'unknown';

/**
 * What one direction of a leg did since the last poll.
 *
 * `unreported` is the engine not counting that direction, which rtpengine
 * usually does not for what it sends, and is not the same as silence.
 * `unknown` is a first sample, with nothing to compare against yet.
 */
export function flow(previous: number | undefined, current: number | undefined, sampled: boolean): Flow {
  if (current === undefined) return 'unreported';
  if (!sampled || previous === undefined) return 'unknown';
  return current > previous ? 'moving' : 'still';
}

/**
 * Whether a connected call looks like one-way audio: one direction still
 * while another moved. Every direction still is a quiet call, not a fault.
 */
export function oneWay(call: Call, previous: Call | undefined): boolean {
  if (call.state !== 'Connected' || !call.media || !previous?.media) return false;
  const flows = call.media.legs.flatMap((leg, index) => {
    const before = previous.media!.legs[index];
    return [flow(before?.packets_in, leg.packets_in, Boolean(before)), flow(before?.packets_out, leg.packets_out, Boolean(before))];
  });
  return flows.includes('still') && flows.includes('moving');
}

function count(value: number | undefined): string {
  return value === undefined ? 'not reported' : value.toLocaleString('en');
}

const FLOW_TEXT: Record<Flow, { label: string; tone?: 'ok' | 'down' }> = {
  moving: { label: 'flowing', tone: 'ok' },
  still: { label: 'nothing new', tone: 'down' },
  unreported: { label: 'not counted' },
  unknown: { label: 'first reading' },
};

function Direction({ name, packets, bytes, before }: { name: string; packets?: number; bytes?: number; before?: number | 'none' }) {
  const state = flow(before === 'none' ? undefined : before, packets, before !== undefined);
  const text = FLOW_TEXT[state];
  return (
    <span className="record-detail">
      {name}: {count(packets)}{packets !== undefined && ' packets'}
      {bytes !== undefined && `, ${bytes.toLocaleString('en')} bytes`}{' '}
      <span className={`record-tag${text.tone ? ` state-tag-${text.tone}` : ''}`}>{text.label}</span>
    </span>
  );
}

function Leg({ leg, before, index }: { leg: CallLeg; before?: CallLeg; index: number }) {
  // `undefined` is no earlier poll at all; 'none' an earlier poll without this direction.
  const prior = (value: number | undefined) => (before ? value ?? 'none' : undefined);
  return (
    <div className="record-main" role="group" aria-label={`Leg ${index + 1}`}>
      <span className="record-name">Leg {index + 1}</span>
      <Direction name="From this end" packets={leg.packets_in} bytes={leg.bytes_in} before={prior(before?.packets_in)} />
      <Direction name="To this end" packets={leg.packets_out} bytes={leg.bytes_out} before={prior(before?.packets_out)} />
    </div>
  );
}

function CallRow({ call, previous, now }: { call: Call; previous?: Call; now: number }) {
  const { from, to } = callParties(call);
  const tone = STATE_TONE[call.state] ?? 'warn';
  const faulty = oneWay(call, previous);
  return (
    <li className="record-row call-row">
      <div className="record-main" role="group" aria-label={call.id}>
        <span className="record-name">
          <span className={`state-dot state-${tone}`} aria-hidden="true" />
          {from} to {to}
        </span>
        <span className="record-detail">Call-ID <code>{call.id}</code></span>
        {call.media && call.media.idle_seconds !== null && call.media.idle_seconds >= 5 && (
          <span className="record-detail">No media in either direction for {call.media.idle_seconds}s.</span>
        )}
      </div>
      <span className={`record-tag state-tag-${tone}`}>{call.state}</span>
      <span className="record-tag">{callDuration(call, now)}</span>
      <span className="record-tag">{call.media ? `Relayed: ${call.media.engine}` : 'Not relayed'}</span>
      {faulty && <span className="record-tag state-tag-down">One-way audio</span>}
      {call.media && call.media.legs.length > 0 && (
        <div className="call-legs">
          {call.media.legs.map((leg, index) => (
            <Leg key={index} leg={leg} before={previous?.media?.legs[index]} index={index} />
          ))}
        </div>
      )}
    </li>
  );
}

/**
 * The calls this node is carrying, polled every `POLL_MS`.
 *
 * The node's counters are cumulative, so flow is the difference between two
 * readings. Legs are not matched to participants: the API does not say which
 * is whose.
 */
export function CallsScreen({ api }: { api: AdminApi }) {
  const result = useRefreshableAsync((signal) => api.listCalls(signal), [api]);
  const { refresh } = result;
  const [samples, setSamples] = useState<{ current?: Call[]; previous?: Call[] }>({});
  if (result.value !== samples.current) setSamples({ current: result.value, previous: samples.current });

  // A 429 holds the poll for as long as the node asked, then reads once and resumes.
  const holdFor = retryAfter(result.error);
  useEffect(() => {
    if (holdFor !== undefined) {
      const timer = window.setTimeout(refresh, holdFor * 1000);
      return () => window.clearTimeout(timer);
    }
    const timer = window.setInterval(refresh, POLL_MS);
    return () => window.clearInterval(timer);
  }, [refresh, holdFor]);

  const now = Date.now();
  const previous = new Map((samples.previous ?? []).map((call) => [call.id, call]));

  return (
    <>
      <h1>Calls</h1>

      <section className="panel">
        <div className="panel-heading">
          <div>
            <h2>Live calls</h2>
            <p>
              The calls this node is carrying now, updated every two seconds. A call that has
              ended drops off. For a relayed call, each leg is one end of the media as the engine
              sees it: what arrived from that end and what the engine sent to it. One-way audio
              is one of those standing still while the rest of the call moves.
            </p>
          </div>
          <button className="secondary-button" type="button" onClick={refresh} disabled={result.refreshing}>
            {result.refreshing ? 'Refreshing...' : 'Refresh'}
          </button>
        </div>

        {result.error && <ErrorMessage error={result.error} />}

        {result.loading ? <Loading /> : !result.value ? null : result.value.length === 0 ? (
          <Empty>No calls in progress.</Empty>
        ) : (
          <ul className="record-list">
            {result.value.map((call) => <CallRow key={call.id} call={call} previous={previous.get(call.id)} now={now} />)}
          </ul>
        )}
      </section>
    </>
  );
}
