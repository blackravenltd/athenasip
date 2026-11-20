import { useEffect, useRef, useState, type ReactNode } from 'react';
import { VolumeMeter } from '../components/VolumeMeter';
import { CallReadout } from '../softphone/CallReadout';
import { callInProgress, describeCallState } from '../softphone/words';
import { DialPad } from './DialPad';
import { canChooseSpeaker, useDevices, type Devices } from './devices';
import { callTimer, dialTarget, userOf } from './dial';
import { notificationPermission } from './ringing';
import { loadSettings, saveSettings, type PhoneSettings } from './settings';
import type { PhoneHandle } from './usePhone';

/**
 * The phone's screen. The phone registers as a subscriber, separately from
 * the console user's sign-in. The shell owns the phone itself (`handle`), so
 * a call carries on while other screens are shown.
 *
 * `aside` is rendered beside the dialler: history and the directory.
 */
export function PhoneScreen({ handle, settings, onSettings, aside }: {
  handle: PhoneHandle;
  settings: PhoneSettings;
  onSettings: (settings: PhoneSettings) => void;
  aside?: ReactNode;
}) {
  const { phone, state, stats, localStream, remoteStream, iceNotice, asking, lineNotice, silent } = handle;
  const { devices, named, ask, error: deviceError } = useDevices();
  const [uri, setUri] = useState(settings.uri ?? '');
  const [password, setPassword] = useState('');
  const [socket, setSocket] = useState(settings.socket ?? '');
  const [target, setTarget] = useState('');
  const [tones, setTones] = useState('');
  const [notifications, setNotifications] = useState(notificationPermission);
  const localVideo = useRef<HTMLVideoElement>(null);
  const remoteVideo = useRef<HTMLVideoElement>(null);

  const registered = state.registration === 'registered';
  const registering = asking || state.registration === 'connecting';
  const inCall = callInProgress(state.call);
  const connected = state.call === 'connected';
  const insecure = window.isSecureContext === false;
  const dialled = dialTarget(target, uri);
  const remoteHasVideo = !!remoteStream?.getVideoTracks?.().length;
  const localHasVideo = !!localStream?.getVideoTracks?.().length;

  useEffect(() => {
    if (remoteVideo.current) remoteVideo.current.srcObject = remoteHasVideo ? remoteStream ?? null : null;
  }, [remoteStream, remoteHasVideo]);
  useEffect(() => {
    if (localVideo.current) localVideo.current.srcObject = localStream ?? null;
  }, [localStream]);
  useEffect(() => {
    if (!inCall) setTones('');
  }, [inCall]);

  const register = () => {
    onSettings({ ...settings, uri, socket: socket.trim() || undefined });
    handle.register({ socket: socket.trim(), uri, password });
  };

  const media = (video: boolean) => ({ video, microphone: settings.microphone, camera: settings.camera });

  const press = (key: string) => {
    if (connected) {
      phone.sendDtmf(key);
      setTones((current) => current + key);
    } else if (!inCall) {
      setTarget((current) => current + key);
    }
  };

  // Keys typed while connected go to the far end as tones, unless a field has focus.
  useEffect(() => {
    if (!connected) return;
    const onKey = (event: KeyboardEvent) => {
      const field = event.target instanceof HTMLElement && /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName);
      if (field || event.metaKey || event.ctrlKey || event.altKey) return;
      if (/^[0-9*#]$/.test(event.key)) press(event.key);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
    // `press` reads only what changes with `connected`.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [connected]);

  const notice = state.notice ?? iceNotice;

  return (
    <>
      <h1>Phone</h1>

      <section className="panel" aria-labelledby="phone-line-heading">
        <div className="panel-heading">
          <div>
            <h2 id="phone-line-heading">{registered ? `Registered as ${userOf(uri)}` : 'Sign in to a line'}</h2>
            <p>
              This browser becomes a phone by registering as one of a realm&apos;s subscribers, the
              way a desk phone does. That is separate from your console sign-in. The address and
              connection are remembered here; the password never is.
            </p>
          </div>
          {registered && (
            <button className="secondary-button" type="button" onClick={handle.unregister}>Sign out of the line</button>
          )}
        </div>

        {insecure && (
          <p className="error-message" role="alert">
            This page is not a secure context, so the browser will not give it a microphone and no
            call can start. Open the console over https.
          </p>
        )}

        {!registered && (
          <form
            className="phone-line-form"
            onSubmit={(event) => { event.preventDefault(); register(); }}
          >
            <label className="field">
              <span>SIP address</span>
              <input value={uri} onChange={(event) => setUri(event.target.value)} placeholder="sip:1001@sip.example.org" autoComplete="username" />
            </label>
            <label className="field">
              <span>Password</span>
              <input type="password" value={password} onChange={(event) => setPassword(event.target.value)} autoComplete="current-password" />
            </label>
            <details className="phone-connection">
              <summary>Connection</summary>
              <label className="field">
                <span>WebSocket</span>
                <input value={socket} onChange={(event) => setSocket(event.target.value)} placeholder="Ask the node" />
              </label>
              <p className="field-hint">Leave empty to ask the node, with the address and password above.</p>
            </details>
            <div className="dialog-actions">
              <button className="primary-button" type="submit" disabled={insecure || registering || !uri}>
                {registering ? 'Registering...' : 'Register'}
              </button>
            </div>
          </form>
        )}
        {!registered && lineNotice && <p className="error-message" role="alert">{lineNotice}</p>}
        {!registered && state.registration === 'failed' && state.notice && (
          <p className="error-message" role="alert">{state.notice}</p>
        )}
      </section>

      <section className="panel" aria-labelledby="phone-devices-heading">
        <div className="panel-heading">
          <div>
            <h2 id="phone-devices-heading">Devices</h2>
            <p>
              Which microphone, camera and speaker calls use. The browser names its devices only once
              this page may use one; asking now, before signing in to a line, means a change of
              permission never interrupts a call.
            </p>
          </div>
        </div>
        {!named ? (
          <div className="phone-actions">
            <button className="secondary-button" type="button" disabled={insecure} onClick={() => { void ask(false); }}>Allow the microphone</button>
            <button className="secondary-button" type="button" disabled={insecure} onClick={() => { void ask(true); }}>Allow the microphone and camera</button>
          </div>
        ) : (
          <div className="field-row">
            <DeviceSelect label="Microphone" devices={devices.microphones} value={settings.microphone} onChange={(microphone) => onSettings({ ...settings, microphone })} />
            {devices.cameras.length > 0 && (
              <DeviceSelect label="Camera" devices={devices.cameras} value={settings.camera} onChange={(camera) => onSettings({ ...settings, camera })} />
            )}
            {canChooseSpeaker() && devices.speakers.length > 0 && (
              <DeviceSelect label="Speaker" devices={devices.speakers} value={settings.speaker} onChange={(speaker) => onSettings({ ...settings, speaker })} />
            )}
          </div>
        )}
        {deviceError && <p className="error-message" role="alert">{deviceError}</p>}
        {named && <p className="field-hint">A change applies from the next call.</p>}
      </section>

      {registered && (
        <div className="phone-layout">
          <section className="panel phone-dialler" aria-label="Dialler">
            {state.call !== 'idle' && (
              <div className={`phone-call phone-call-${state.call}`} role="status" aria-live="polite">
                <span className="phone-call-who">{userOf(state.remoteIdentity)}</span>
                <span className="phone-call-state">
                  {describeCallState(state.call)}
                  {connected && state.connectedAt !== undefined ? `, ${callTimer(Date.now() - state.connectedAt)}` : ''}
                  {state.held ? ', on hold' : ''}
                  {state.heldByFarEnd ? ', held by the far end' : ''}
                  {state.muted ? ', muted' : ''}
                </span>
                {state.remoteIdentity && <code className="phone-call-uri">{state.remoteIdentity}</code>}
              </div>
            )}

            {state.call === 'incoming' && (
              <div className="phone-actions">
                <button className="primary-button" type="button" onClick={() => handle.answer(media(false))}>Answer</button>
                <button className="secondary-button" type="button" onClick={() => handle.answer(media(true))}>Answer with video</button>
                <button className="secondary-button danger-button" type="button" onClick={() => phone.decline()}>Decline</button>
              </div>
            )}

            {!inCall && (
              <label className="field">
                <span>Number or address</span>
                <input
                  value={target}
                  onChange={(event) => setTarget(event.target.value)}
                  onKeyDown={(event) => { if (event.key === 'Enter' && dialled) handle.call(dialled, media(false)); }}
                  placeholder="1002, or bob@example.org"
                  aria-label="Number or address"
                />
              </label>
            )}
            {connected && tones && <p className="phone-tones" aria-label="Tones sent">{tones}</p>}

            {(!inCall || connected) && <DialPad label={connected ? 'Send tones' : 'Dial pad'} onKey={press} />}

            {!inCall && (
              <div className="phone-actions">
                <button className="primary-button" type="button" disabled={!dialled} onClick={() => dialled && handle.call(dialled, media(false))}>Call</button>
                <button className="secondary-button" type="button" disabled={!dialled} onClick={() => dialled && handle.call(dialled, media(true))}>Video call</button>
                {target && <button className="secondary-button" type="button" onClick={() => setTarget('')}>Clear</button>}
              </div>
            )}

            {inCall && state.call !== 'incoming' && (
              <div className="phone-actions">
                <button className="secondary-button" type="button" aria-pressed={!!state.muted} disabled={!connected} onClick={() => phone.mute(!state.muted)}>
                  {state.muted ? 'Unmute' : 'Mute'}
                </button>
                <button className="secondary-button" type="button" aria-pressed={!!state.held} disabled={!connected} onClick={() => phone.hold(!state.held)}>
                  {state.held ? 'Resume' : 'Hold'}
                </button>
                <button className="primary-button danger-button" type="button" onClick={() => phone.hangUp()}>Hang up</button>
              </div>
            )}

            {connected && (
              <div className="phone-meters">
                <VolumeMeter label="Your microphone" source={{ kind: 'stream', stream: localStream }} active={connected && !state.muted} />
                <VolumeMeter label="Far end" source={{ kind: 'stream', stream: remoteStream }} active={connected} />
              </div>
            )}
            {silent && (
              <p className="error-message" role="alert">
                Nothing but silence is leaving this browser. Check the microphone chosen under Devices:
                a muted input, or a virtual one such as BlackHole, sends nothing the far end can hear.
              </p>
            )}

            {(localHasVideo || remoteHasVideo) && (
              <div className="softphone-video">
                <figure>
                  <video ref={remoteVideo} autoPlay playsInline muted aria-label="Far end video" />
                  <figcaption>{userOf(state.remoteIdentity)}</figcaption>
                </figure>
                <figure>
                  <video ref={localVideo} autoPlay playsInline muted aria-label="Your video" />
                  <figcaption>You</figcaption>
                </figure>
              </div>
            )}

            {notice && state.registration === 'registered' && <p className="error-message" role="alert">{notice}</p>}

            {notifications === 'default' && (
              <p className="field-hint">
                A call rings here while this tab is open.{' '}
                <button
                  className="link-button"
                  type="button"
                  onClick={() => { void Notification.requestPermission().then(setNotifications).catch(() => undefined); }}
                >
                  Notify me when this tab is hidden
                </button>
              </p>
            )}
            {notifications === 'denied' && (
              <p className="field-hint">This browser blocks notifications from the console, so a call only rings while you can hear this tab.</p>
            )}
          </section>

          {aside}
        </div>
      )}

      {state.call !== 'idle' && (
        <section className="panel softphone">
          <details>
            <summary>Call details</summary>
            <CallReadout state={state} stats={stats} />
          </details>
        </section>
      )}
    </>
  );
}

/** One device choice. The first option is the browser's default, stored as no value. */
function DeviceSelect({ label, devices, value, onChange }: {
  label: string;
  devices: Devices[keyof Devices];
  value?: string;
  onChange: (deviceId: string | undefined) => void;
}) {
  // A remembered device that is absent shows as the default, and stays remembered.
  const present = devices.some((device) => device.deviceId === value);
  return (
    <label className="field">
      <span>{label}</span>
      <select value={present ? value : ''} onChange={(event) => onChange(event.target.value || undefined)}>
        <option value="">Browser default</option>
        {devices.filter((device) => device.deviceId && device.deviceId !== 'default').map((device, index) => (
          <option key={device.deviceId} value={device.deviceId}>{device.label || `${label} ${index + 1}`}</option>
        ))}
      </select>
    </label>
  );
}

/** The phone's settings, and a setter that also stores them. */
export function useRememberedSettings(): [PhoneSettings, (settings: PhoneSettings) => void] {
  const [settings, setSettings] = useState<PhoneSettings>(() => loadSettings());
  const update = (next: PhoneSettings) => {
    setSettings(next);
    saveSettings(next);
  };
  return [settings, update];
}
