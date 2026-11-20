import { describe, expect, it } from 'vitest';
import { sendingSilence } from './devices';

describe('sendingSilence', () => {
  it('is silence when the sent energy has not grown over the last five readings', () => {
    expect(sendingSilence([0.2, 0.2, 0.2, 0.2, 0.2])).toBe(true);
    expect(sendingSilence([0, 0.1, 0.3, 0.3, 0.3, 0.3, 0.3])).toBe(true);
  });

  it('is not silence while it grows, before there are enough readings, or when the browser does not say', () => {
    expect(sendingSilence([0.2, 0.2, 0.2, 0.2, 0.21])).toBe(false);
    expect(sendingSilence([0, 0, 0, 0])).toBe(false);
    expect(sendingSilence([undefined, undefined, undefined, undefined, undefined])).toBe(false);
  });
});
