import { useEffect, useRef, useState } from 'react';
import type { AdminApi } from '../api/AdminApi';
import { errorMessage } from '../api/errors';
import { VolumeMeter } from '../components/VolumeMeter';
import { usableIceServers } from '../softphone/iceServers';
import { CallReadout } from '../softphone/CallReadout';
import type { IceOptions, SipStack } from '../softphone/Softphone';
import { jssipStack } from '../softphone/jssip';
import type { PageOptions } from '../softphone/page';
import { useSoftphone } from '../softphone/useSoftphone';
import { callInProgress, describeCallState, describeRegistration, lineOf, signallingUri } from '../softphone/words';

export { callInProgress, describeCallState, describeRegistration, describeVideoLine, signallingUri } from '../softphone/words';

interface Connection {
  socket: string;
  uri: string;
  password: string;
  target: string;
}

/** The WebSocket port a node is assumed to serve SIP on, when nothing says. */
const DEFAULT_WS_PORT = 8088;

/**
 * Connection defaults: the query string first, which is how a harness opens
 * the page, then the environment, then the page's host. Nothing is persisted,
 * so no credential is left in the browser.
 */
export function defaultConnection(options: PageOptions): Connection {
  const socket = options.socket
    ?? import.meta.env.VITE_SIP_WS_URL
    ?? (typeof window === 'undefined'
      ? `ws://localhost:${DEFAULT_WS_PORT}`
      : `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.hostname}:${DEFAULT_WS_PORT}`);
  return {
    socket,
    uri: options.uri ?? import.meta.env.VITE_SIP_URI ?? '',
    password: options.password ?? '',
    target: options.target ?? import.meta.env.VITE_SIP_TARGET ?? '',
  };
}

const NO_OPTIONS: PageOptions = { register: false, answer: false };

/**
 * A diagnostic WebRTC endpoint. It registers over WebSocket, places or
 * answers one call, and shows what was negotiated and whether media moved, so
 * a connected but silent call is visible.
 *
 * It is also served on its own page as the browser end of AthenaSIP's
 * end-to-end run; see `docs/softphone.md`.
 */
export function SoftphoneScreen({ options = NO_OPTIONS, stack = jssipStack, api }: {
  options?: PageOptions;
  stack?: SipStack;
  /** The console's node, which says where to signal and what to use for ICE. Absent on the harness page. */
  api?: AdminApi;
}) {
  const [connection, setConnection] = useState<Connection>(() => defaultConnection(options));
  const [configNotice, setConfigNotice] = useState<string>();
  const [iceServers, setIceServers] = useState<RTCIceServer[]>();
  const [relayOnly, setRelayOnly] = useState(false);
  // The harness page's `video=1`; the console's own box starts unticked.
  const [video, setVideo] = useState(options.video ?? false);
  const localVideoRef = useRef<HTMLVideoElement>(null);
  const remoteVideoRef = useRef<HTMLVideoElement>(null);
  const { phone, state, remoteStream, stats } = useSoftphone(stack, options);
  const audioRef = useRef<HTMLAudioElement>(null);
  const [playbackNotice, setPlaybackNotice] = useState<string>();
  // Undefined where the browser does not say, as in a test; only a plain no stops the phone.
  const insecure = window.isSecureContext === false;

  /**
   * Where to signal, from the node, unless the query string or the
   * environment said, or something was typed over the guess. The node's
   * configuration is signed with the typed SIP credentials, so it is asked
   * as the phone registers.
   */
  const register = () => {
    const { socket, uri, password } = connection;
    const line = lineOf(uri, password);
    const guessed = !options.socket && !import.meta.env.VITE_SIP_WS_URL && socket === defaultConnection(options).socket;
    if (!api || !guessed || !line) {
      phone.register({ socket, uri, password });
      return;
    }
    setConfigNotice(undefined);
    const https = window.location.protocol === 'https:';
    api.subscriberConfig(line).then((config) => {
      const found = signallingUri(config, https);
      if (!found && https) setConfigNotice('This node has no secure WebSocket listener, so a page served over https cannot reach it.');
      return found;
    }, (cause: unknown) => {
      setConfigNotice(`Could not read the node's client configuration: ${errorMessage(cause)}`);
      return undefined;
    }).then((found) => {
      const chosen = found ?? socket;
      setConnection((current) => ({ ...current, socket: chosen }));
      phone.register({ socket: chosen, uri, password });
    });
  };

  /**
   * TURN credentials are minted per request and expire, so they are fetched as
   * the call is placed or answered. If the node does not answer, the call goes
   * ahead without them.
   */
  const withIceServers = (place: (ice?: IceOptions) => void) => {
    if (!api) {
      // The page's own `ice` and `relay`, which the phone already holds.
      if (options.ice) setIceServers(usableIceServers(options.ice));
      place();
      return;
    }
    const line = lineOf(connection.uri, connection.password);
    (line ? api.subscriberConfig(line) : Promise.reject(new Error('the SIP URI is not sip:user@realm'))).then(
      (config) => usableIceServers(config.ice_servers),
      (cause: unknown) => {
        setConfigNotice(`No ICE servers from the node, calling without: ${errorMessage(cause)}`);
        return [];
      },
    ).then((servers) => {
      setIceServers(servers);
      place({ servers, relayOnly });
    });
  };
  // The remote picture plays muted: the <audio> element already carries its sound.
  const localStream = phone.localStream;
  const remoteHasVideo = !!remoteStream?.getVideoTracks?.().length;
  const localHasVideo = !!localStream?.getVideoTracks?.().length;
  useEffect(() => {
    if (remoteVideoRef.current) remoteVideoRef.current.srcObject = remoteHasVideo ? remoteStream ?? null : null;
  }, [remoteStream, remoteHasVideo]);
  useEffect(() => {
    if (localVideoRef.current) localVideoRef.current.srcObject = localStream ?? null;
  }, [localStream]);

  useEffect(() => {
    const element = audioRef.current;
    if (!element || !remoteStream) return;
    element.srcObject = remoteStream;
    void element.play().catch((cause: unknown) => setPlaybackNotice(`Audio playback refused: ${String(cause)}`));
  }, [remoteStream]);

  const busy = state.registration === 'connecting';
  const registered = state.registration === 'registered';
  const inCall = callInProgress(state.call);

  const field = (key: keyof Connection, label: string, type = 'text') => (
    <label className="field">
      <span>{label}</span>
      <input
        type={type}
        value={connection[key]}
        disabled={registered || busy}
        spellCheck={false}
        autoComplete={type === 'password' ? 'off' : 'on'}
        data-testid={`softphone-${key}`}
        onChange={(event) => setConnection((current) => ({ ...current, [key]: event.target.value }))}
      />
    </label>
  );

  const notice = state.notice ?? playbackNotice ?? configNotice;

  return (
    <>
      <h1>Softphone</h1>

      <section className="panel softphone">
        <div className="panel-heading">
          <div>
            <h2>WebRTC endpoint</h2>
            <p>
              Registers against this node over the WebSocket transport and places one call, so
              that signalling and media can be proven end to end from a browser. The negotiation
              readout and the levels below are what catch a call that connects and then carries
              no audio.
            </p>
          </div>
        </div>

        <div className="field-row">
          {field('socket', 'WebSocket URL')}
          {field('uri', 'SIP URI')}
          {field('password', 'Password', 'password')}
          {field('target', 'Call target')}
        </div>
        {api && (
          <label className="checkbox-field">
            <input
              type="checkbox"
              checked={relayOnly}
              disabled={inCall}
              data-testid="softphone-relay-only"
              onChange={(event) => setRelayOnly(event.target.checked)}
            />
            <span>Relay only: send media through the node's TURN server even when a direct path exists, to prove TURN works</span>
          </label>
        )}
        {api && (
          <label className="checkbox-field">
            <input
              type="checkbox"
              checked={video}
              disabled={inCall}
              data-testid="softphone-video"
              onChange={(event) => setVideo(event.target.checked)}
            />
            <span>Video: send the camera as well, on calls placed and answered from here</span>
          </label>
        )}
        <p className="field-hint">
          Nothing typed here is stored. The password is held in this page only, and is gone on
          reload.
        </p>

        <div className="button-row">
          {!registered ? (
            <button
              className="primary-button"
              type="button"
              data-testid="softphone-register"
              onClick={register}
              disabled={insecure || busy || !connection.socket || !connection.uri}
            >
              {busy ? 'Registering...' : 'Register'}
            </button>
          ) : (
            <>
              <button
                className="primary-button"
                type="button"
                data-testid="softphone-call"
                onClick={() => withIceServers((ice) => phone.call(connection.target, ice, { video }))}
                disabled={insecure || inCall || !connection.target}
              >
                Call
              </button>
              {state.call === 'incoming' && (
                <button className="primary-button" type="button" data-testid="softphone-answer" onClick={() => withIceServers((ice) => phone.answer(ice, { video }))}>
                  Answer
                </button>
              )}
              <button
                className="secondary-button danger-button"
                type="button"
                data-testid="softphone-hangup"
                onClick={() => phone.hangUp()}
                disabled={!inCall}
              >
                Hang up
              </button>
              <button className="secondary-button" type="button" data-testid="softphone-unregister" onClick={() => phone.unregister()}>
                Unregister
              </button>
            </>
          )}
        </div>

        <p className="call-state">
          <span className={`state-dot state-${registered ? 'ok' : state.registration === 'failed' ? 'down' : 'warn'}`} aria-hidden="true" />
          <span role="status" data-testid="softphone-registration" data-state={state.registration}>
            {describeRegistration(state.registration)}
          </span>
          {registered && (
            <span role="status" data-testid="softphone-call-state" data-state={state.call}>
              {describeCallState(state.call)}
              {state.remoteIdentity && state.call !== 'idle' ? ` (${state.remoteIdentity})` : ''}
            </span>
          )}
        </p>
        {insecure && (
          <p className="error-message" role="alert" data-testid="softphone-insecure">
            This page is not a secure context, so the browser will not give it a microphone and no
            call can start. Open the console over https, or through localhost.
          </p>
        )}
        {notice && <p className="error-message" role="alert">{notice}</p>}

        <audio ref={audioRef} autoPlay hidden playsInline />
        {(localHasVideo || remoteHasVideo) && (
          <div className="softphone-video">
            <figure>
              <video ref={remoteVideoRef} autoPlay playsInline muted data-testid="softphone-remote-video" />
              <figcaption>Far end</figcaption>
            </figure>
            <figure>
              <video ref={localVideoRef} autoPlay playsInline muted data-testid="softphone-local-video" />
              <figcaption>This browser</figcaption>
            </figure>
          </div>
        )}
        <VolumeMeter label="Far end" source={{ kind: 'stream', stream: remoteStream }} active={state.call === 'connected'} />
        <VolumeMeter label="Microphone" source={{ kind: 'microphone' }} active={registered} />
      </section>

      {state.call !== 'idle' && (
        <section className="panel softphone" aria-labelledby="negotiation-heading">
          <div className="panel-heading">
            <div>
              <h2 id="negotiation-heading">Negotiation</h2>
              <p>
                What the browser and the node agreed, and whether anything is moving. The
                candidate pair says where this browser is actually sending its media.
              </p>
            </div>
          </div>

          <CallReadout
            state={state}
            stats={stats}
            extra={iceServers && (<><dt>ICE servers</dt><dd data-testid="softphone-ice-servers">{iceServers.length ? iceServers.map((server) => server.urls).join(', ') : 'None'}{relayOnly || (!api && options.relay) ? ', relay only' : ''}</dd></>)}
          />
        </section>
      )}
    </>
  );
}
