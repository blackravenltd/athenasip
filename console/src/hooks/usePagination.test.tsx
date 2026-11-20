// @vitest-environment jsdom
import { act, render } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { usePagination, type Pagination } from './usePagination';

/** Renders the hook in a bare component and exposes its latest value. */
function harness<T>(items: readonly T[], pageSize: number) {
  const seen: { current?: Pagination<T> } = {};
  function Probe({ list }: { list: readonly T[] }) {
    seen.current = usePagination(list, pageSize);
    return null;
  }
  const view = render(<Probe list={items} />);
  return { seen, rerender: (list: readonly T[]) => view.rerender(<Probe list={list} />) };
}

const rows = (count: number) => Array.from({ length: count }, (_, index) => index);

describe('usePagination', () => {
  it('reports a single page when everything fits', () => {
    const { seen } = harness(rows(4), 10);
    expect(seen.current?.pageCount).toBe(1);
    expect(seen.current?.page).toHaveLength(4);
  });

  it('reports one page, not zero, when there is nothing at all', () => {
    const { seen } = harness(rows(0), 10);
    expect(seen.current?.pageCount).toBe(1);
  });

  it('does not run past the last page', () => {
    const { seen } = harness(rows(12), 10);
    act(() => seen.current?.next());
    act(() => seen.current?.next());
    expect(seen.current?.pageIndex).toBe(1);
  });

  it('lands on a page that exists when the list shrinks under it', () => {
    const { seen, rerender } = harness(rows(11), 10);
    act(() => seen.current?.next());
    expect(seen.current?.pageIndex).toBe(1);
    rerender(rows(10));
    expect(seen.current?.pageIndex).toBe(0);
    expect(seen.current?.page).toHaveLength(10);
  });
});
