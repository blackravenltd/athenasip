// @vitest-environment jsdom
import { act, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { SessionExpiry, formatRemaining } from './SessionExpiry';

afterEach(() => { vi.useRealTimers(); });

describe('formatRemaining', () => {
  it('reads as a countdown, rounding a part second up', () => {
    expect(formatRemaining(300)).toBe('5:00');
    expect(formatRemaining(65)).toBe('1:05');
    expect(formatRemaining(0.2)).toBe('0:01');
    expect(formatRemaining(-3)).toBe('0:00');
  });
});

describe('SessionExpiry', () => {
  it('stays quiet until the last five minutes, then counts down', () => {
    vi.useFakeTimers({ now: 1_000_000 });
    render(<SessionExpiry expiresAt={1_000 + 600} />);
    expect(screen.getByRole('status').textContent).toBe('');

    act(() => { vi.advanceTimersByTime(300_000); });
    expect(screen.getByRole('status').textContent).toContain('Your session ends within five minutes');
    expect(screen.getByText(/Session ends in/).textContent).toBe('Session ends in 5:00');

    act(() => { vi.advanceTimersByTime(61_000); });
    expect(screen.getByText(/Session ends in/).textContent).toBe('Session ends in 3:59');
  });

  it('shows nothing when no expiry was given', () => {
    render(<SessionExpiry />);
    expect(screen.getByRole('status').textContent).toBe('');
  });
});
