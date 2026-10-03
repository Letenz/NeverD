'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {setTimeout: delay} = require('node:timers/promises');
const {once} = require('node:events');
const {captureActive, observeExecution, preserveActive, validateSampling,
  waitForNativeSample, verifyIdentity, readTail} = require('./active-sample.cjs');
const {startCommand, runSequence, testEnvironment} = require('./run.cjs');

test('sampling is opt-in and fits the 500 artifact allowance', () => {
  assert.equal(validateSampling('false', 200), false);
  assert.equal(validateSampling('true', 166), true);
  assert.throws(() => validateSampling('true', 167), /166/);
  for (const value of ['', 'yes', true]) assert.throws(() => validateSampling(value, 1), /invalid/);
});

test('identity requires the unique PID, parent, session and complete binary path', () => {
  assert.ok(verifyIdentity(' 42 41 42 /test/NeverD\n', 42, 41, '/test/NeverD'));
  for (const row of ['42 40 42 /test/NeverD', '42 41 41 /test/NeverD',
    '43 41 42 /test/NeverD', '42 41 42 /other/NeverD', '42 41 42 /test/NeverD\n43 41 43 /test/NeverD']) {
    assert.equal(verifyIdentity(row, 42, 41, '/test/NeverD'), false);
  }
});

test('log snapshots read a bounded tail and never overwrite evidence or follow symlinks', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  try {
    fs.writeFileSync(path.join(root, 'source'), '0123456789');
    const result = readTail(path.join(root, 'source'), path.join(root, 'copy'), 4);
    assert.equal(fs.readFileSync(path.join(root, 'copy'), 'utf8'), '6789');
    assert.equal(result.first_byte, 6);
    assert.equal(result.truncated, true);
    assert.throws(() => readTail(path.join(root, 'source'), path.join(root, 'copy')));
    fs.symlinkSync(path.join(root, 'source'), path.join(root, 'link'));
    assert.throws(() => readTail(path.join(root, 'link'), path.join(root, 'unsafe')));
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('short execution clears its observation timer without sampling', async () => {
  let samples = 0;
  assert.equal(await observeExecution({completion: Promise.resolve(0)}, async () => ++samples, 5), 0);
  await delay(15);
  assert.equal(samples, 0);
});

test('slow source validation cannot consume the native sampling delay', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  let started = false;
  try {
    const ready = waitForNativeSample(root, () => true, new AbortController().signal, 15, 2)
      .then(value => { started = true; return value; });
    await delay(25);
    assert.equal(started, false);
    const registered = Date.now();
    fs.writeFileSync(path.join(root, 'children.json'), '{"process_groups":[42]}');
    const result = await ready;
    assert.ok(Date.parse(result.observed_at) >= registered);
    assert.ok(Date.now() - registered >= 15);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('completion before native registration ends observation without a sample', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  let running = true;
  try {
    const ready = waitForNativeSample(root, () => running, new AbortController().signal, 15, 2);
    running = false;
    assert.equal(await ready, null);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('cancellation during the native sampling delay ends the timer', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  const abort = new AbortController();
  try {
    fs.writeFileSync(path.join(root, 'children.json'), '{"process_groups":[42]}');
    const ready = waitForNativeSample(root, () => true, abort.signal, 5000, 2);
    const rejected = assert.rejects(ready, /aborted/);
    abort.abort();
    await rejected;
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('waiting for registration preserves the original command deadline', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  const operation = startCommand(process.execPath, ['-e', 'setInterval(() => {}, 1000)'],
    testEnvironment(process.env), {stdio: 'ignore', timeoutMs: 30, graceMs: 10});
  try {
    await assert.rejects(observeExecution(operation, running =>
      waitForNativeSample(root, running, new AbortController().signal, 5000, 2), 0),
    /deadline/);
    assert.equal(operation.result.termination_reason, 'deadline');
    assert.ok(operation.result.completed_at);
    assert.notEqual(operation.child.signalCode, null);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('execution finishing during sampling waits for its one sealed upload', async () => {
  let complete;
  let samples = 0;
  let uploaded = false;
  const operation = {completion: new Promise(resolve => { complete = resolve; })};
  const result = await observeExecution(operation, async isRunning => {
    ++samples;
    assert.equal(isRunning(), true);
    complete(0);
    await delay(5);
    assert.equal(isRunning(), false);
    uploaded = true;
  }, 1);
  assert.equal(result, 0);
  assert.equal(samples, 1);
  assert.equal(uploaded, true);
});

test('active upload failure retires a real live child and prevents the next method', async () => {
  const operation = startCommand(process.execPath, ['-e',
    "process.on('SIGTERM', () => {}); process.stdout.write('ready'); setInterval(() => {}, 1000)"],
  testEnvironment(process.env), {stdio: 'pipe', timeoutMs: 5000, graceMs: 20});
  const events = [];
  await once(operation.child.stdout, 'data');
  await assert.rejects(runSequence({methods: [{index: 0}, {index: 1}]},
    async (phase, index) => { events.push([phase, index]); },
    () => observeExecution(operation, async () => { throw new Error('active upload unavailable'); }, 1)),
  /active upload unavailable/);
  assert.equal(operation.result.termination_reason, 'active-evidence-failure');
  assert.equal(operation.child.signalCode, 'SIGKILL');
  assert.deepEqual(events, [['plan', undefined], ['start', 0], ['finish', 0]]);
});

test('cancellation after collection prevents active upload', async () => {
  const abort = new AbortController();
  let uploaded = false;
  await assert.rejects(preserveActive(async () => { abort.abort(); },
    async () => { uploaded = true; }, abort.signal), /before active upload/);
  assert.equal(uploaded, false);
});

test('identity failure records absence without invoking sample or another process', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  const calls = [];
  try {
    fs.writeFileSync(path.join(root, 'children.json'), JSON.stringify({process_groups: [42]}));
    const destination = await captureActive({directory: root, pythonPid: 41, binary: '/test/NeverD',
      environment: {}, signal: new AbortController().signal, isRunning: () => true,
      collect: async binary => { calls.push(binary); return {status: 0, stdout: '42 1 42 /test/NeverD'}; }});
    const result = JSON.parse(fs.readFileSync(path.join(destination, 'metadata.json')));
    assert.match(result.errors[0], /identity changed/);
    assert.deepEqual(calls, ['/bin/ps']);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('cancellation during identity collection cannot begin stack sampling', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  const abort = new AbortController();
  const calls = [];
  try {
    fs.writeFileSync(path.join(root, 'children.json'), JSON.stringify({process_groups: [42]}));
    const destination = await captureActive({directory: root, pythonPid: 41, binary: '/test/NeverD',
      environment: {}, signal: abort.signal, isRunning: () => true,
      collect: async binary => {
        calls.push(binary); abort.abort(); return {status: 0, stdout: '42 41 42 /test/NeverD'};
      }});
    const result = JSON.parse(fs.readFileSync(path.join(destination, 'metadata.json')));
    assert.equal(result.cancelled, true);
    assert.deepEqual(calls, ['/bin/ps']);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('a zero sample status without a call graph remains a collection failure', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-sample-'));
  try {
    fs.writeFileSync(path.join(root, 'children.json'), '{"process_groups":[42]}');
    fs.mkdirSync(path.join(root, 'execution/methods/0000'), {recursive: true});
    fs.writeFileSync(path.join(root, 'execution/methods/0000/output.log'), 'original log');
    const destination = await captureActive({directory: root, pythonPid: 41, binary: '/test/NeverD',
      environment: {}, signal: new AbortController().signal, isRunning: () => true,
      captureHostState: async () => ({}), collect: async binary => ({status: 0,
        stdout: binary === '/bin/ps' ? '42 41 42 /test/NeverD' : '', stderr: 'target exited'})});
    const result = JSON.parse(fs.readFileSync(path.join(destination, 'metadata.json')));
    assert.match(result.errors[0], /no call graph/);
    assert.equal(result.identity_verified, true);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});
