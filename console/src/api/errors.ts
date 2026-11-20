/**
 * A failed admin API call. The server answers every failure with
 * `{"error": {"code": "...", "message": "..."}}`: a stable code to branch on
 * and a message for a person. Only `HttpAdminApi` builds one from a response.
 *
 * `status` tells a 401, which is the session's problem and the shell's to
 * handle, from a failure that belongs to the screen, such as a `conflict`.
 */
export class ApiError extends Error {
  readonly status: number;
  readonly code?: ApiErrorCode | string;
  /** Seconds to wait, from `Retry-After`, when the node is rate limiting (429). */
  readonly retryAfter?: number;

  constructor(message: string, status: number, code?: ApiErrorCode | string, retryAfter?: number) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    this.retryAfter = retryAfter;
  }

  /** The token is missing or unknown: the session has ended. */
  get isUnauthorized(): boolean {
    return this.status === 401;
  }

  /** A known token without the role the route needs. The session stands. */
  get isForbidden(): boolean {
    return this.status === 403;
  }

  /** The node is rate limiting. Never a sign-out: the session stands. */
  get isRateLimited(): boolean {
    return this.status === 429 || this.code === 'rate_limited';
  }
}

/** Seconds to hold off after a 429 that named no `Retry-After`. */
export const DEFAULT_RETRY_AFTER_S = 10;

/** Seconds to wait before asking again when the node is rate limiting; undefined for any other failure. */
export function retryAfter(cause: unknown): number | undefined {
  if (!(cause instanceof ApiError) || !cause.isRateLimited) return undefined;
  return cause.retryAfter ?? DEFAULT_RETRY_AFTER_S;
}

/** The codes the server's OpenAPI document lists. */
export type ApiErrorCode =
  | 'invalid_json'
  | 'invalid_request'
  | 'unauthorized'
  | 'forbidden'
  | 'not_found'
  | 'method_not_allowed'
  | 'conflict'
  | 'datastore_error'
  | 'unavailable'
  | 'rate_limited'
  | 'would_lock_out'
  | 'unknown_role'
  | 'wrong_password';

/**
 * The message to show for any thrown value: an `ApiError`, a `TypeError` from
 * fetch, an abort, a thrown string. Never "[object Object]".
 */
export function errorMessage(cause: unknown): string {
  if (cause instanceof ApiError && cause.isRateLimited) {
    // The node's own message does not say how long to wait.
    return cause.retryAfter
      ? `The node is limiting requests. Try again in ${cause.retryAfter} second${cause.retryAfter === 1 ? '' : 's'}.`
      : 'The node is limiting requests. Wait a little and try again.';
  }
  if (cause instanceof Error) return cause.message;
  if (typeof cause === 'string') return cause;
  if (cause && typeof cause === 'object') {
    const message = (cause as { message?: unknown }).message;
    if (typeof message === 'string' && message) return message;
    return 'The server failed without saying why.';
  }
  return cause === undefined || cause === null
    ? 'The server failed without saying why.'
    : String(cause);
}

/** Whether a failure is the caller cancelling, which must never surface as an error. */
export function isAbort(cause: unknown): boolean {
  return cause instanceof DOMException && cause.name === 'AbortError';
}

/**
 * Whether the server refused a create because the thing already exists.
 * A 409 `would_lock_out` is not one: it is about the caller, not the name.
 */
export function isConflict(cause: unknown): boolean {
  return cause instanceof ApiError && (cause.code === 'conflict' || (cause.status === 409 && cause.code !== 'would_lock_out'));
}
