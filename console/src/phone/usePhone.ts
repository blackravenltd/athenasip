import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { AdminApi } from '../api/AdminApi';
import { ApiError, errorMessage } from '../api/errors';
import type { SubscriberLine } from '../api/types';
import { usableIceServers } from '../softphone/iceServers';
import { sendingSilence } from './devices';
import { Softphone, type CallMedia, type MediaStats, type SipStack, type SoftphoneState } from '../softphone/Softphone';
import { lineOf, signallingUri } from '../softphone/words';

export interface PhoneHandle {
  phone: Softphone;
  state: SoftphoneState;
  remoteStream?: MediaStream;
  localStream?: MediaStream;
  /** The browser's media counters, updated every second while a call is connected. */
  stats?: MediaStats;
  /** Why the node gave no ICE servers for the last call, when it did not. */
  iceNotice?: string;
  /** Asking the node where to connect, before registering. */
  asking: boolean;
  /** Why registering did not start, or why its WebSocket is a guess. */
  lineNotice?: string;
  /** The connected, unmuted call has sent nothing but silence for the last few seconds. */
  silent: boolean;
  /** Registers; an empty `socket` asks the node, signed with the line's own credentials. */
  register: (line: { uri: string; password: string; socket: string }) => void;
  unregister: () => void;
  call: (target: string, media?: CallMedia) => void;
  answer: (media?: CallMedia) => void;
}

/** The fallback SIP WebSocket: this host, port 8088. */
function guessedSocket(): string {
  return `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.hostname}:8088`;
}

/**
 * The console's phone: one `Softphone` for as long as the shell holds it.
 *
 * The node's client configuration is signed with the line's own SIP
 * credentials, which are held here, in memory, while the line is in use. It
 * says where to connect when no WebSocket was given, and TURN credentials
 * expire, so ICE servers are fetched as each call is placed or answered; if
 * the node does not answer, the call proceeds with none.
 */
export function usePhone(stack: SipStack, api: AdminApi): PhoneHandle {
  const phone = useMemo(() => new Softphone(stack), [stack]);
  const [state, setState] = useState<SoftphoneState>(phone.state);
  const [streams, setStreams] = useState<{ remote?: MediaStream; local?: MediaStream }>({});
  const [stats, setStats] = useState<MediaStats>();
  const [iceNotice, setIceNotice] = useState<string>();
  const [asking, setAsking] = useState(false);
  const [lineNotice, setLineNotice] = useState<string>();
  const line = useRef<SubscriberLine | undefined>(undefined);
  const [energies, setEnergies] = useState<Array<number | undefined>>([]);

  useEffect(() => {
    const unsubscribe = phone.subscribe((next) => {
      setState(next);
      setStreams((current) => (current.remote === phone.remoteStream && current.local === phone.localStream
        ? current
        : { remote: phone.remoteStream, local: phone.localStream }));
    });
    return () => {
      unsubscribe();
      phone.dispose();
    };
  }, [phone]);

  useEffect(() => {
    if (state.call !== 'connected') {
      setStats(undefined);
      setEnergies([]);
      return;
    }
    let cancelled = false;
    const read = async () => {
      const next = await phone.stats();
      if (cancelled) return;
      setStats(next);
      setEnergies((current) => [...current.slice(-9), next?.sentAudioEnergy]);
    };
    void read();
    const timer = setInterval(() => { void read(); }, 1000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [phone, state.call]);

  const register = useCallback(({ uri, password, socket }: { uri: string; password: string; socket: string }) => {
    setLineNotice(undefined);
    line.current = lineOf(uri, password);
    if (socket) {
      phone.register({ socket, uri, password });
      return;
    }
    if (!line.current) {
      setLineNotice('A SIP address is sip:user@realm.');
      return;
    }
    setAsking(true);
    const https = window.location.protocol === 'https:';
    api.subscriberConfig(line.current).then(
      (config) => {
        const found = signallingUri(config, https);
        if (!found) {
          setLineNotice(https
            ? 'This node has no secure WebSocket listener, so a page served over https cannot reach it.'
            : 'This node has no WebSocket listener.');
          return;
        }
        phone.register({ socket: found, uri, password });
      },
      (cause: unknown) => {
        if (cause instanceof ApiError && cause.status === 401) {
          setLineNotice('The node did not accept that SIP address and password.');
        } else if (cause instanceof ApiError && cause.status === 404) {
          setLineNotice(`This node serves no realm ${line.current?.realm}.`);
        } else {
          setLineNotice(`Could not ask the node where to connect, so this is a guess: ${errorMessage(cause)}`);
          phone.register({ socket: guessedSocket(), uri, password });
        }
      },
    ).finally(() => setAsking(false));
  }, [api, phone]);

  const unregister = useCallback(() => {
    line.current = undefined;
    phone.unregister();
  }, [phone]);

  const withIce = useCallback((place: (servers: RTCIceServer[]) => void) => {
    setIceNotice(undefined);
    if (!line.current) {
      place([]);
      return;
    }
    api.subscriberConfig(line.current).then(
      (config) => usableIceServers(config.ice_servers),
      (cause: unknown) => {
        setIceNotice(`No ICE servers from the node, calling without: ${errorMessage(cause)}`);
        return [];
      },
    ).then(place);
  }, [api]);

  const call = useCallback((target: string, media: CallMedia = {}) => {
    withIce((servers) => phone.call(target, { servers }, media));
  }, [phone, withIce]);

  const answer = useCallback((media: CallMedia = {}) => {
    withIce((servers) => phone.answer({ servers }, media));
  }, [phone, withIce]);

  const silent = state.call === 'connected' && !state.muted && !state.held && sendingSilence(energies);
  return {
    phone, state, remoteStream: streams.remote, localStream: streams.local, stats, iceNotice, asking, lineNotice, silent,
    register, unregister, call, answer,
  };
}
