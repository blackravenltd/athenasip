// @vitest-environment jsdom
import { render } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { VolumeMeter } from './VolumeMeter';

/** Enough of Web Audio to count how often a meter starts and what it reads from. */
function stubAudio() {
  const contexts: Array<{ sources: unknown[]; closed: boolean }> = [];
  class FakeAudioContext {
    readonly record = { sources: [] as unknown[], closed: false };
    constructor() { contexts.push(this.record); }
    createAnalyser() { return { fftSize: 0, frequencyBinCount: 4, getByteFrequencyData: () => undefined, connect: () => undefined }; }
    createMediaStreamSource(stream: unknown) { this.record.sources.push(stream); return { connect: () => undefined, disconnect: () => undefined }; }
    async close() { this.record.closed = true; }
  }
  vi.stubGlobal('AudioContext', FakeAudioContext);
  vi.stubGlobal('requestAnimationFrame', () => 1);
  vi.stubGlobal('cancelAnimationFrame', () => undefined);
  return contexts;
}

describe('VolumeMeter', () => {
  afterEach(() => { vi.unstubAllGlobals(); });

  it('reads the far end from its stream, and does not restart for a new object naming the same stream', async () => {
    const contexts = stubAudio();
    const first = {} as MediaStream;
    const { rerender } = render(<VolumeMeter label="Far end" source={{ kind: 'stream', stream: first }} active />);
    rerender(<VolumeMeter label="Far end" source={{ kind: 'stream', stream: first }} active />);
    await Promise.resolve();
    expect(contexts).toHaveLength(1);
    expect(contexts[0].sources).toEqual([first]);

    const second = {} as MediaStream;
    rerender(<VolumeMeter label="Far end" source={{ kind: 'stream', stream: second }} active />);
    await Promise.resolve();
    expect(contexts).toHaveLength(2);
    expect(contexts[0].closed).toBe(true);
    expect(contexts[1].sources).toEqual([second]);
  });

  it('waits for a stream before starting', () => {
    const contexts = stubAudio();
    render(<VolumeMeter label="Far end" source={{ kind: 'stream' }} active />);
    expect(contexts).toHaveLength(0);
  });
});
