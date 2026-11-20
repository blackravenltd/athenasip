import { describe, expect, it } from 'vitest';
import { expose, pageOptions, READOUT_KEY, videoNegotiation, withoutSecrets } from './page';
import type { Softphone } from './Softphone';

describe('pageOptions', () => {
  it('reads the fields and the two flags from a query string', () => {
    expect(pageOptions('?ws=ws%3A%2F%2F10.0.0.2%3A8088&uri=sip%3A1001%4010.0.0.2&password=pw&target=sip%3A1002%4010.0.0.2&register=1&answer=true'))
      .toEqual({
        socket: 'ws://10.0.0.2:8088',
        uri: 'sip:1001@10.0.0.2',
        password: 'pw',
        target: 'sip:1002@10.0.0.2',
        register: true,
        answer: true,
        ice: undefined,
        relay: false,
        video: false,
      });
    expect(pageOptions('?video=1').video).toBe(true);
  });

  it('reads the ICE servers as JSON and the relay flag, keeping only entries with a urls', () => {
    const ice = JSON.stringify([{ urls: 'turn:h:3478', username: '1:token', credential: 'c', expires_at: 1 }, { nope: true }]);
    expect(pageOptions(`?ice=${encodeURIComponent(ice)}&relay=1`)).toMatchObject({
      ice: [{ urls: 'turn:h:3478', username: '1:token', credential: 'c', expires_at: 1 }],
      relay: true,
    });
    expect(pageOptions('?ice=not-json').ice).toBeUndefined();
    expect(pageOptions('?ice=%7B%7D').ice).toBeUndefined();
  });

  it('treats an absent or empty value as not given, and anything but 1 or true as off', () => {
    expect(pageOptions('?uri=&register=0&answer=yes')).toEqual({
      socket: undefined, uri: undefined, password: undefined, target: undefined, register: false, answer: false, ice: undefined, relay: false, video: false,
    });
    expect(pageOptions('')).toMatchObject({ register: false, answer: false });
  });
});

describe('withoutSecrets', () => {
  it('removes the password and the ICE servers and nothing else', () => {
    expect(withoutSecrets('http://127.0.0.1:8080/softphone.html?uri=sip%3A1001%40x&password=pw&ice=%5B%5D&relay=1&register=1'))
      .toBe('http://127.0.0.1:8080/softphone.html?uri=sip%3A1001%40x&relay=1&register=1');
  });

  it('says so when there was nothing to remove, so the history is left alone', () => {
    expect(withoutSecrets('http://127.0.0.1:8080/softphone.html?uri=sip%3A1001%40x')).toBeUndefined();
  });
});

describe('videoNegotiation', () => {
  const sdp = (video: string) => ['v=0', 'a=group:BUNDLE 0 1', 'm=audio 9 UDP/TLS/RTP/SAVPF 111', 'a=mid:0', video].join('\r\n');
  const state = (remoteSdp?: string) => ({ registration: 'registered', call: 'connected', localSdp: sdp('m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:1'), remoteSdp }) as const;

  it('says what the far end did with the video line', () => {
    expect(videoNegotiation(state(sdp('m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:1'))).outcome).toBe('bundled');
    expect(videoNegotiation(state(sdp('m=video 30000 UDP/TLS/RTP/SAVPF 96\r\na=mid:2'))).outcome).toBe('accepted');
    expect(videoNegotiation(state(sdp('m=video 0 UDP/TLS/RTP/SAVPF 96\r\na=mid:1'))).outcome).toBe('declined');
    expect(videoNegotiation(state(sdp(''))).outcome).toBe('absent');
    expect(videoNegotiation(state(undefined))).toMatchObject({ outcome: 'absent', local: { port: 9, bundled: true } });
  });
});

describe('expose', () => {
  it('hangs a readout on the window that reads through to the phone, and takes it down again', async () => {
    const calls: string[] = [];
    const phone = {
      state: { registration: 'registered', call: 'idle' },
      history: [{ at: 1, registration: 'registered', call: 'idle' }],
      stats: () => Promise.resolve({ packetsSent: 3 }),
      call: (target: string, _ice: unknown, media: { video?: boolean }) => { calls.push(`${target}${media.video ? ' with video' : ''}`); },
      answer: (_ice: unknown, media: { video?: boolean }) => { calls.push(`answer${media.video ? ' with video' : ''}`); },
      hangUp: () => { calls.push('hangUp'); },
    } as unknown as Softphone;
    const target = {} as Window;

    const remove = expose(target, phone);
    const readout = target[READOUT_KEY]!;
    expect(readout.version).toBe(1);
    expect(readout.state()).toEqual({ registration: 'registered', call: 'idle' });
    expect(readout.history()).toHaveLength(1);
    expect(await readout.stats()).toEqual({ packetsSent: 3 });
    readout.call('sip:1002@x');
    readout.answer();
    readout.hangUp();
    expect(calls).toEqual(['sip:1002@x', 'answer', 'hangUp']);

    expect(readout.video().outcome).toBe('absent');

    remove();
    expect(target[READOUT_KEY]).toBeUndefined();

    expose(target, phone, { video: true });
    target[READOUT_KEY]!.call('sip:1003@x');
    target[READOUT_KEY]!.answer();
    expect(calls.slice(-2)).toEqual(['sip:1003@x with video', 'answer with video']);
  });
});
