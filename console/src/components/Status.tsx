import { useEffect, useState } from 'react';
import { errorMessage } from '../api/errors';

/** A spinner that appears only after `delayMs`, so a fast request never flashes one. */
export function Loading({ delayMs = 180 }: { delayMs?: number } = {}) {
  const [visible, setVisible] = useState(false);

  useEffect(() => {
    const timer = window.setTimeout(() => setVisible(true), Math.max(0, delayMs));
    return () => window.clearTimeout(timer);
  }, [delayMs]);

  if (!visible) return null;
  return (
    <div className="loading-overlay" role="status" aria-live="polite" aria-label="Loading">
      <div className="loading-spinner" aria-hidden="true" />
    </div>
  );
}

/** A failure, in one line. */
export function ErrorMessage({ error }: { error: unknown }) {
  return <p className="error-message" role="alert">{errorMessage(error)}</p>;
}

/** The message for a list or panel with nothing in it. */
export function Empty({ children }: { children: React.ReactNode }) {
  return <p className="empty-message">{children}</p>;
}
