import { useEffect, useRef } from 'react';
import { createPortal } from 'react-dom';
import { Link } from 'react-router-dom';
import type { AdminApi } from '../api/AdminApi';
import type { Role } from '../api/types';
import { routes } from '../app/routes';
import { jssipStack } from '../softphone/jssip';
import type { SipStack } from '../softphone/Softphone';
import { describeCallState } from '../softphone/words';
import { callTimer, userOf } from './dial';
import { PhoneAside } from './PhoneAside';
import { PhoneScreen, useRememberedSettings } from './PhoneScreen';
import { useIncomingNotification, useRinging } from './ringing';
import { usePhone } from './usePhone';

/**
 * The phone, held by the shell.
 *
 * Mounted when the Phone section is first opened and kept until sign-out, so
 * a call survives a change of screen. For the same reason the far end's audio
 * plays from here, and a call indicator is rendered into `indicator`, a slot
 * in the top bar.
 */
export default function PhoneHost({ api, roles, visible, indicator, stack = jssipStack }: {
  api: AdminApi;
  /** The console user's roles, which decide whether history and the directory can be read. */
  roles: readonly Role[];
  visible: boolean;
  indicator: HTMLElement | null;
  stack?: SipStack;
}) {
  const handle = usePhone(stack, api);
  const [settings, setSettings] = useRememberedSettings();
  const audio = useRef<HTMLAudioElement>(null);
  const { state, remoteStream } = handle;
  useRinging(state.call);
  useIncomingNotification(state.call, userOf(state.remoteIdentity));

  useEffect(() => {
    const element = audio.current;
    if (!element) return;
    element.srcObject = remoteStream ?? null;
    if (remoteStream) void element.play().catch(() => undefined);
  }, [remoteStream]);

  // Use the chosen speaker where the browser supports `setSinkId`.
  useEffect(() => {
    const element = audio.current as (HTMLAudioElement & { setSinkId?: (id: string) => Promise<void> }) | null;
    if (!element?.setSinkId) return;
    void element.setSinkId(settings.speaker ?? '').catch(() => undefined);
  }, [settings.speaker]);

  const call = state.call === 'idle' || state.call === 'ended' || state.call === 'failed' ? undefined : state.call;

  return (
    <>
      <audio ref={audio} autoPlay hidden playsInline />
      {visible && (
        <PhoneScreen
          handle={handle}
          settings={settings}
          onSettings={setSettings}
          aside={(
            <PhoneAside
              api={api}
              roles={roles}
              ownUri={settings.uri ?? ''}
              call={state.call}
              onDial={(uri) => handle.call(uri, { microphone: settings.microphone })}
            />
          )}
        />
      )}
      {indicator && call && createPortal(
        <Link className={`topbar-call topbar-call-${call}`} to={routes.phone}>
          <span className={`state-dot state-${call === 'connected' ? 'ok' : 'warn'}`} aria-hidden="true" />
          {call === 'incoming' ? `Incoming call from ${userOf(state.remoteIdentity)}` : `${describeCallState(call)} ${userOf(state.remoteIdentity)}`}
          {call === 'connected' && state.connectedAt !== undefined ? ` ${callTimer(Date.now() - state.connectedAt)}` : ''}
        </Link>,
        indicator,
      )}
    </>
  );
}
