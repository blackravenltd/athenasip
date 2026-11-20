import { useMemo, useState } from 'react';

export interface Pagination<T> {
  page: T[];
  pageIndex: number;
  pageCount: number;
  next: () => void;
  previous: () => void;
}

/**
 * Client-side paging; the server does not page.
 *
 * The page index is clamped, so deleting the last record on the last page
 * lands on a page that exists. There is always at least one page.
 */
export function usePagination<T>(items: readonly T[], pageSize: number): Pagination<T> {
  const [requested, setRequested] = useState(0);
  const pageCount = Math.max(1, Math.ceil(items.length / pageSize));
  const pageIndex = Math.min(requested, pageCount - 1);
  const page = useMemo(
    () => items.slice(pageIndex * pageSize, pageIndex * pageSize + pageSize),
    [items, pageIndex, pageSize],
  );

  return {
    page,
    pageIndex,
    pageCount,
    next: () => setRequested(Math.min(pageIndex + 1, pageCount - 1)),
    previous: () => setRequested(Math.max(pageIndex - 1, 0)),
  };
}
