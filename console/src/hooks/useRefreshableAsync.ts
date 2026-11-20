import { useCallback, useEffect, useRef, useState } from 'react';
import { isAbort } from '../api/errors';

export interface RefreshableAsyncState<T> {
  value?: T;
  error?: Error;
  loading: boolean;
  refreshing: boolean;
  refresh: () => void;
}

/**
 * Loads a value when `dependencies` change, and keeps it across a refresh.
 *
 * `loading` is a first load with nothing to show; `refreshing` keeps `value`
 * on screen while it is fetched again. Requests are aborted on unmount and
 * superseded on refresh, and an abort is never reported as an error.
 */
export function useRefreshableAsync<T>(
  factory: (signal: AbortSignal) => Promise<T>,
  dependencies: readonly unknown[],
): RefreshableAsyncState<T> {
  const factoryRef = useRef(factory);
  factoryRef.current = factory;
  const activeRef = useRef<{ id: number; controller: AbortController } | undefined>(undefined);
  const nextIdRef = useRef(0);
  const [state, setState] = useState<Omit<RefreshableAsyncState<T>, 'refresh'>>({ loading: true, refreshing: false });

  const load = useCallback((refreshing: boolean) => {
    activeRef.current?.controller.abort(new DOMException('Refresh superseded', 'AbortError'));
    const controller = new AbortController();
    const id = ++nextIdRef.current;
    activeRef.current = { id, controller };
    setState((current) => (refreshing
      ? { ...current, error: undefined, loading: false, refreshing: true }
      : { loading: true, refreshing: false }));

    void factoryRef.current(controller.signal).then((value) => {
      if (controller.signal.aborted || activeRef.current?.id !== id) return;
      setState({ value, loading: false, refreshing: false });
    }).catch((cause: unknown) => {
      if (isAbort(cause) || controller.signal.aborted || activeRef.current?.id !== id) return;
      // Normalised so a screen can always read `error.message`.
      const error = cause instanceof Error ? cause : new Error(String(cause));
      setState((current) => ({ ...current, error, loading: false, refreshing: false }));
    });
  }, []);

  useEffect(() => {
    load(false);
    return () => activeRef.current?.controller.abort(new DOMException('Async page was replaced', 'AbortError'));
    // The caller's `dependencies` decide when to reload.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, dependencies);

  const refresh = useCallback(() => load(true), [load]);
  return { ...state, refresh };
}
