import { useEffect, useState } from 'react';

/** How long before expiry the top bar starts counting down. */
export const WARN_SECONDS = 5 * 60;

/** Minutes and seconds, e.g. `4:05`. */
export function formatRemaining(seconds: number): string {
  const whole = Math.max(0, Math.ceil(seconds));
  return `${Math.floor(whole / 60)}:${String(whole % 60).padStart(2, '0')}`;
}

/**
 * Seconds until `expiresAt` (Unix seconds) once it is within `within` seconds
 * of now; undefined before that or with no expiry. One timer waits for the
 * window to open, and it ticks each second only inside it.
 */
export function useSecondsLeft(expiresAt: number | undefined, within = WARN_SECONDS): number | undefined {
  const [now, setNow] = useState(() => Date.now());
  const warning = expiresAt !== undefined && (expiresAt - within) * 1000 <= now;

  useEffect(() => {
    if (expiresAt === undefined) return;
    if (warning) {
      const timer = setInterval(() => setNow(Date.now()), 1000);
      return () => clearInterval(timer);
    }
    const untilWarning = (expiresAt - within) * 1000 - Date.now();
    // setTimeout holds 32 bits of milliseconds; a session that long is re-read on the next render.
    if (untilWarning >= 2 ** 31) return;
    const timer = setTimeout(() => setNow(Date.now()), Math.max(0, untilWarning));
    return () => clearTimeout(timer);
  }, [expiresAt, within, warning]);

  return warning ? Math.max(0, expiresAt - now / 1000) : undefined;
}

/**
 * The top bar's countdown in a session's last minutes. A session's expiry is
 * absolute and nothing extends it.
 *
 * The live region announces once, as the countdown appears; the ticking figure
 * is hidden from screen readers.
 */
export function SessionExpiry({ expiresAt }: { expiresAt?: number }) {
  const left = useSecondsLeft(expiresAt);
  return (
    <span className="session-expiry" role="status">
      {left !== undefined && (
        <>
          <span className="sr-only">Your session ends within five minutes. Finish what you are doing, then sign in again.</span>
          <span aria-hidden="true" title="Sessions have a fixed lifetime. Sign in again when this one ends.">
            Session ends in {formatRemaining(left)}
          </span>
        </>
      )}
    </span>
  );
}
