import { afterEach } from 'vitest';

// Suites that render opt in to jsdom with an `@vitest-environment jsdom`
// comment; the rest run in the node environment and have nothing to unmount.
// `findBy` and `waitFor` wait 1 s by default, which a loaded machine overruns.
if (typeof document !== 'undefined') {
  const { configure } = await import('@testing-library/react');
  configure({ asyncUtilTimeout: 5000 });
}

afterEach(async () => {
  if (typeof document === 'undefined') return;
  const { cleanup } = await import('@testing-library/react');
  cleanup();
});
