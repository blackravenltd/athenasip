#!/usr/bin/env node
/**
 * `npm run test:athenasip`: everything this console can test unattended, for
 * the server's suite runner (`../test/suite/run.sh`), which calls it
 * once per phase with the fixture up and its environment exported.
 *
 * Runs the unit and contract tests (in the direct phase only), then the
 * Playwright specs against the node, and writes `$ATHENA_SUITE_RESULTS/admin/summary.json`. Exits 0 only
 * when nothing failed. Never starts or stops a container, installs or builds.
 */
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const phase = process.env.ATHENA_SUITE_PHASE;
const results = process.env.ATHENA_SUITE_RESULTS;
if (phase !== 'direct' && phase !== 'relay') fail(`ATHENA_SUITE_PHASE is ${phase ?? 'unset'}, not direct or relay`);
if (!results) fail('ATHENA_SUITE_RESULTS is unset');
if (!existsSync('build/index.html')) fail('build/ has no index.html; building is the runner\'s precondition');

// The runner gives each phase its own ATHENA_SUITE_RESULTS.
const dir = join(results, 'admin');
mkdirSync(dir, { recursive: true });

const summary = { phase, passed: 0, failed: 0, skipped: 0, failures: [] };

// Unit and contract tests need no node, so they run once, in the direct phase.
if (phase === 'direct') {
  const vitestJson = join(dir, 'vitest.json');
  const vitest = run('npx', ['vitest', 'run', '--maxWorkers=4', '--reporter=default', '--reporter=json', `--outputFile.json=${vitestJson}`]);
  countVitest(vitestJson, vitest);
}

const playwrightJson = join(dir, 'playwright.json');
const playwright = run('npx', ['playwright', 'test', '--reporter=list,json'], { PLAYWRIGHT_JSON_OUTPUT_NAME: playwrightJson, CI: '1' });
countPlaywright(playwrightJson, playwright);

writeFileSync(join(dir, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
console.log(`\nathenasip-admin ${phase}: ${summary.passed} passed, ${summary.failed} failed, ${summary.skipped} skipped`);
for (const line of summary.failures) console.log(`  ${line}`);
process.exit(summary.failed === 0 ? 0 : 1);

function run(command, args, env = {}) {
  const result = spawnSync(command, args, { stdio: 'inherit', env: { ...process.env, ...env } });
  return result.status ?? 1;
}

function failed(name, reason) {
  summary.failed += 1;
  summary.failures.push(`${phase}: ${name}: ${firstLine(reason)}`);
}

function firstLine(text) {
  // Assertion messages carry terminal colour codes.
  // eslint-disable-next-line no-control-regex
  return String(text ?? 'failed').replace(/\u001b\[[0-9;]*m/g, '').split('\n').find((line) => line.trim())?.trim() ?? 'failed';
}

function countVitest(file, status) {
  if (!existsSync(file)) return failed('vitest', `exited ${status} without a report`);
  const report = JSON.parse(readFileSync(file, 'utf8'));
  for (const suite of report.testResults ?? []) {
    if ((suite.assertionResults ?? []).length === 0 && suite.status === 'failed') failed(suite.name, suite.message);
    for (const test of suite.assertionResults ?? []) {
      if (test.status === 'passed') summary.passed += 1;
      else if (test.status === 'failed') failed(test.fullName, test.failureMessages?.[0]);
      else summary.skipped += 1;
    }
  }
  if (status !== 0 && summary.failed === 0) failed('vitest', `exited ${status}`);
}

function countPlaywright(file, status) {
  if (!existsSync(file)) return failed('playwright', `exited ${status} without a report`);
  const report = JSON.parse(readFileSync(file, 'utf8'));
  const before = summary.failed;
  const walk = (suite) => {
    for (const child of suite.suites ?? []) walk(child);
    for (const spec of suite.specs ?? []) {
      for (const test of spec.tests ?? []) {
        if (test.status === 'expected' || test.status === 'flaky') summary.passed += 1;
        // Skipped with no skip annotation: it did not run, because an earlier hook or test failed.
        else if (test.status === 'skipped' && (test.annotations ?? []).some((note) => note.type === 'skip')) summary.skipped += 1;
        else if (test.status === 'skipped') failed(`${spec.file} › ${spec.title}`, 'did not run, after an earlier failure');
        else failed(`${spec.file} › ${spec.title}`, test.results?.at(-1)?.error?.message);
      }
    }
  };
  for (const suite of report.suites ?? []) walk(suite);
  for (const error of report.errors ?? []) failed('playwright', error.message);
  if (status !== 0 && summary.failed === before) failed('playwright', `exited ${status}`);
}

function fail(message) {
  console.error(`test:athenasip: ${message}`);
  process.exit(2);
}
