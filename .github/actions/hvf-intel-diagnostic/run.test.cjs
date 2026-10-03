'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const {once} = require('node:events');
const {runSequence, startCommand, testEnvironment} = require('./run.cjs');
const plan = {methods: [{index: 3}, {index: 7}]};

test('each immutable start marker completes before execution and result preservation', async () => {
  const events = [];
  await runSequence(plan,
    async (phase, index) => { events.push([phase, index]); },
    async index => { events.push(['execute', index]); return 0; });
  assert.deepEqual(events, [['plan', undefined], ['start', 3], ['execute', 3],
    ['finish', 3], ['start', 7], ['execute', 7], ['finish', 7]]);
});

test('failed pre-execution upload prevents every guest entry', async () => {
  for (const failure of ['plan', 'start']) {
    let entries = 0;
    await assert.rejects(runSequence(plan,
      async phase => { if (phase === failure) throw new Error('upload unavailable'); },
      async () => { ++entries; return 0; }), /upload unavailable/);
    assert.equal(entries, 0);
  }
});

test('failed, signalled and exceptional execution preserve results but stop the suffix', async () => {
  for (const status of [1, null, 'throws']) {
    const events = [];
    await assert.rejects(runSequence(plan,
      async (phase, index) => { events.push([phase, index]); },
      async index => { events.push(['execute', index]);
        if (status === 'throws') throw new Error('interrupted');
        return status;
      }));
    assert.deepEqual(events, [['plan', undefined], ['start', 3], ['execute', 3], ['finish', 3]]);
  }
});

test('failed result preservation cannot enter the next method', async () => {
  const entries = [];
  await assert.rejects(runSequence(plan,
    async phase => { if (phase === 'finish') throw new Error('result upload unavailable'); },
    async index => { entries.push(index); return 0; }), /result upload unavailable/);
  assert.deepEqual(entries, [3]);
});

test('test environment excludes upload credentials while retaining orphan cleanup identity', () => {
  assert.deepEqual(testEnvironment({ACTIONS_RUNTIME_TOKEN: 'secret', GITHUB_TOKEN: 'secret',
    INPUT_SOURCE: 'source', RUNNER_TRACKING_ID: 'job', PATH: '/bin', NEVERD_THREADS: '2'}),
  {RUNNER_TRACKING_ID: 'job', PATH: '/bin', NEVERD_THREADS: '2'});
});

test('external cancellation forcibly retires a child that ignores SIGTERM', async () => {
  const operation = startCommand(process.execPath, ['-e',
    "process.on('SIGTERM', () => {}); process.stdout.write('ready'); setInterval(() => {}, 1000)"],
  testEnvironment(process.env), {stdio: 'pipe', timeoutMs: 5000, graceMs: 100});
  const rejected = assert.rejects(operation.completion, /cancelled/);
  await once(operation.child.stdout, 'data');
  operation.cancel();
  await rejected;
  assert.equal(operation.child.signalCode, 'SIGKILL');
  assert.equal(operation.result.signal, 'SIGKILL');
  assert.equal(operation.result.termination_reason, 'external-cancellation');
  assert.equal(operation.result.exit_status, null);
  assert.ok(operation.result.completed_at);
});

test('a failed spawn rejects without leaving a live command', async () => {
  const operation = startCommand('/never-existing-hvf-command', [], {}, {stdio: 'pipe'});
  await assert.rejects(operation.completion, /ENOENT/);
  assert.equal(operation.result.spawn_error, 'ENOENT');
});
