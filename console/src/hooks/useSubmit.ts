import { useCallback, useState } from 'react';
import { errorMessage, isAbort } from '../api/errors';

export interface SubmitState {
  busy: boolean;
  error?: string;
  /** What was thrown, for a form that shows some failures beside a field. */
  failure?: unknown;
  clearError: () => void;
  /** Resolves to true when the call succeeded, so the caller can close a dialogue. */
  run: (action: () => Promise<unknown>) => Promise<boolean>;
}

/**
 * Runs one write with a busy flag and error capture. Every mutation goes
 * through here: `busy` is set while it is in flight, a failure is caught into
 * `error`, and an abort is ignored.
 */
export function useSubmit(): SubmitState {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const [failure, setFailure] = useState<unknown>();

  const run = useCallback(async (action: () => Promise<unknown>): Promise<boolean> => {
    setBusy(true);
    setError(undefined);
    setFailure(undefined);
    try {
      await action();
      return true;
    } catch (cause) {
      if (!isAbort(cause)) {
        setError(errorMessage(cause));
        setFailure(cause);
      }
      return false;
    } finally {
      setBusy(false);
    }
  }, []);

  const clearError = useCallback(() => { setError(undefined); setFailure(undefined); }, []);
  return { busy, error, failure, clearError, run };
}
