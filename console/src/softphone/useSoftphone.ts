import { useEffect, useMemo, useState } from 'react';
import type { MediaStats, SipStack, SoftphoneState } from './Softphone';
import { Softphone } from './Softphone';
import { usableIceServers } from './iceServers';
import { expose, type PageOptions } from './page';

export interface SoftphoneHandle {
  phone: Softphone;
  state: SoftphoneState;
  /** The far end's audio and video, once any arrives. */
  remoteStream?: MediaStream;
  /** The browser's counters, refreshed every second while a call is up. */
  stats?: MediaStats;
}

/**
 * One `Softphone` for the life of a component. It is disposed on unmount, so
 * leaving the page leaves no registration behind. Page options apply here:
 * `register=1` registers on mount and `answer=1` answers incoming calls.
 */
export function useSoftphone(stack: SipStack, options: PageOptions): SoftphoneHandle {
  const phone = useMemo(() => {
    const created = new Softphone(stack);
    // The page's ICE options; the console passes its own per call.
    created.useIce({ servers: usableIceServers(options.ice ?? []), relayOnly: options.relay });
    return created;
    // `options` is read once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [stack]);
  const [state, setState] = useState<SoftphoneState>(phone.state);
  const [remoteStream, setRemoteStream] = useState<MediaStream>();
  const [stats, setStats] = useState<MediaStats>();

  useEffect(() => {
    const unsubscribe = phone.subscribe((next) => {
      setState(next);
      setRemoteStream(phone.remoteStream);
    });
    const unexpose = typeof window === 'undefined' ? () => {} : expose(window, phone, { video: options.video ?? false });
    return () => {
      unsubscribe();
      unexpose();
      phone.dispose();
    };
  }, [phone]);

  useEffect(() => {
    if (options.register && options.socket && options.uri) {
      phone.register({ socket: options.socket, uri: options.uri, password: options.password ?? '' });
    }
    // `options` is read once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [phone]);

  useEffect(() => {
    if (options.answer && state.call === 'incoming') phone.answer(undefined, { video: options.video ?? false });
  }, [options.answer, options.video, phone, state.call]);

  useEffect(() => {
    if (state.call !== 'connected') {
      setStats(undefined);
      return;
    }
    let cancelled = false;
    const read = async () => {
      const next = await phone.stats();
      if (!cancelled) setStats(next);
    };
    void read();
    const timer = setInterval(() => { void read(); }, 1000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [phone, state.call]);

  return { phone, state, remoteStream, stats };
}
