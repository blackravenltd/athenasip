/** The real JsSIP behind `SipStack`. Only this module imports the library. */
import JsSIP from 'jssip';
import type { SipStack } from './Softphone';

export const jssipStack: SipStack = {
  createUserAgent: (configuration) => new JsSIP.UA(configuration),
  createSocket: (url) => new JsSIP.WebSocketInterface(url),
};
