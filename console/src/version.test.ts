import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

/** The version line under the README header must match `package.json`. */
describe('the README version line', () => {
  it('matches package.json', () => {
    const { version } = JSON.parse(readFileSync('package.json', 'utf8')) as { version: string };
    const readme = readFileSync('README.md', 'utf8');
    expect(readme).toContain(`# AthenaSIP Admin\n\n*v${version}*\n`);
  });
});
