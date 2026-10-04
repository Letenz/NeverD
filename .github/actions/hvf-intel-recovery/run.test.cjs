'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {once} = require('node:events');
const {setTimeout: delay} = require('node:timers/promises');
const {startCommand, testEnvironment} = require('../hvf-intel-diagnostic/run.cjs');
const {readProgress, observeRecovery, commandGroup, finishUpload, LIMIT, MAX_PROGRESS} = require('./run.cjs');
const NAME = 'HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry';
const environment = testEnvironment(process.env);
const round = n => `Repeating all tests (iteration ${n}) . . .\n[ RUN      ] ${NAME}\n[       OK ] ${NAME} (150 ms)\n[  PASSED  ] 1 test.\n`;
const plan = {native_name: NAME, command: [process.execPath], commit: 'a'.repeat(40), controller_commit: 'b'.repeat(40)};
const collectIdentity = async () => ({verified: true});

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-recovery-'));
  fs.mkdirSync(path.join(directory, 'execution'));
  fs.writeFileSync(path.join(directory, 'execution/output.log'), '');
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  return directory;
}

async function producer(directory, count = 100, options = {}) {
  const script = `const fs = require('node:fs');
    const root = ${JSON.stringify(directory)};
    const round = ${round.toString()}; const NAME = ${JSON.stringify(NAME)};
    fs.writeFileSync(root + '/children.json', JSON.stringify({process_groups: [process.pid]}));
    process.stdout.write('ready'); let n = 0;
    const interval = setInterval(() => {
      fs.appendFileSync(root + '/execution/output.log', round(++n));
      if (n === ${count}) clearInterval(interval);
    }, 5);`;
  const operation = startCommand(process.execPath, ['-e', script], environment,
    {stdio: 'pipe', timeoutMs: 5000, ...options});
  // Register an early rejection handler while awaiting the ready message.
  operation.completion.catch(() => {});
  await once(operation.child.stdout, 'data');
  return operation;
}

function observed(operation, directory, upload, options = {}) {
  return observeRecovery(operation, {directory, plan, upload, environment,
    signal: new AbortController().signal, pollMs: 5, stallMs: 40, collectIdentity, ...options});
}

test('native completes unchanged while its sealed initial upload is still pending', async t => {
  const directory = fixture(t), operation = await producer(directory);
  const saved = [];
  const result = await observed(operation, directory, async destination => {
    const before = fs.readFileSync(path.join(destination, 'output-tail.log'));
    saved.push(destination);
    assert.equal(await operation.completion, 0);
    assert.deepEqual(fs.readFileSync(path.join(destination, 'output-tail.log')), before);
  });
  assert.equal(result, 0);
  assert.equal(saved.length, 1); // Intermediate progress is coalesced, never queued.
  assert.equal(fs.readFileSync(path.join(directory, 'execution/output.log'), 'utf8'),
    Array.from({length: 100}, (_, i) => round(i + 1)).join(''));
});

test('completion waits for an already started immutable upload', async t => {
  const directory = fixture(t), operation = await producer(directory, 10);
  let uploaded = false;
  await observed(operation, directory, async () => {
    await operation.completion;
    await delay(30);
    uploaded = true;
  });
  assert.equal(uploaded, true);
});

test('bounded copies carry exact byte ranges and never read through symlinks', t => {
  const directory = fixture(t), file = path.join(directory, 'execution/output.log');
  fs.writeFileSync(file, 'x'.repeat(LIMIT + 100) + '\n' + round(300));
  const captured = readProgress(directory, NAME);
  assert.equal(captured.buffer.length, LIMIT);
  assert.equal(captured.output.first_byte, fs.statSync(file).size - LIMIT);
  assert.equal(captured.output.truncated, true);
  assert.equal(captured.progress.last_completed_iteration, 300);
  fs.unlinkSync(file);
  fs.symlinkSync(path.join(directory, 'unrelated'), file);
  assert.throws(() => readProgress(directory, NAME), /ELOOP/);
});

test('one stalled snapshot preserves new output before the twenty-fifth completion', async t => {
  const directory = fixture(t), operation = await producer(directory, 8);
  // A separate synthetic operation keeps observing after the producer stops writing.
  await operation.completion;
  let finish;
  const controlled = {child: operation.child, completion: new Promise(resolve => { finish = resolve; }),
    cancel: () => finish(1)};
  const reasons = [];
  await observed(controlled, directory, async destination => {
    reasons.push(JSON.parse(fs.readFileSync(path.join(destination, 'metadata.json'))).reason);
    if (reasons.length === 1) {
      fs.appendFileSync(path.join(directory, 'execution/output.log'),
        `Repeating all tests (iteration 9) . . .\n[ RUN      ] ${NAME}\n`);
    } else setTimeout(() => finish(0), 60);
  });
  assert.deepEqual(reasons, ['registered', 'stalled']);
});

test('hard artifact cap bounds even unexpectedly fast or excess native output', async t => {
  const directory = fixture(t);
  fs.writeFileSync(path.join(directory, 'children.json'), JSON.stringify({process_groups: [1234]}));
  let finish, count = 0;
  const operation = {child: {pid: 123}, completion: new Promise(resolve => { finish = resolve; }),
    cancel: () => finish(1)};
  await observed(operation, directory, async () => {
    ++count;
    fs.appendFileSync(path.join(directory, 'execution/output.log'),
      Array.from({length: 25}, (_, i) => round((count - 1) * 25 + i + 1)).join(''));
    if (count === MAX_PROGRESS) setTimeout(() => finish(0), 30);
    if (count > MAX_PROGRESS) finish(1);
  });
  assert.equal(count, MAX_PROGRESS);
});

test('progress and pending uploads do not reset the original command deadline', async t => {
  const directory = fixture(t), operation = await producer(directory, 10000, {timeoutMs: 300, graceMs: 50});
  await assert.rejects(observed(operation, directory, async () => { await delay(450); }), /deadline/);
  assert.equal(operation.result.termination_reason, 'deadline');
  assert.ok(Date.parse(operation.result.completed_at) - Date.parse(operation.result.started_at) < 1500);
  assert.notEqual(operation.child.exitCode, 0);
});

test('failed upload cancels and retires the still-running producer', async t => {
  const directory = fixture(t), operation = await producer(directory, 10000);
  await assert.rejects(observed(operation, directory, async () => { throw new Error('upload unavailable'); }),
    /upload unavailable/);
  assert.equal(operation.result.termination_reason, 'progress-evidence-failure');
  assert.ok(operation.result.completed_at);
  assert.equal(operation.child.signalCode, 'SIGTERM');
});

test('uploader exit failures and fatal signals retain distinct process evidence', async t => {
  const directory = fixture(t);
  for (const [suffix, script, exit, signal] of [
    ['success', 'process.exit(0)', 0, null],
    ['exit', 'process.exit(7)', 7, null],
    ['signal', "process.kill(process.pid, 'SIGTERM')", null, 'SIGTERM'],
  ]) {
    const operation = startCommand(process.execPath, ['-e', script], environment, {stdio: 'pipe'});
    const record = path.join(directory, `${suffix}.json`);
    const finished = finishUpload(operation, record, suffix);
    if (suffix === 'success') await finished;
    else await assert.rejects(finished, new RegExp(`exit=${exit}; signal=${signal}`));
    const data = JSON.parse(fs.readFileSync(record));
    assert.equal(data.pid, operation.child.pid);
    assert.equal(data.executable, process.execPath);
    assert.equal(data.exit_status, exit);
    assert.equal(data.signal, signal);
    assert.equal(data.termination_reason, null);
    assert.ok(data.completed_at);
  }
});

test('uploader deadline is preserved even when command completion rejects', async t => {
  const directory = fixture(t), record = path.join(directory, 'timeout.json');
  const operation = startCommand(process.execPath, ['-e', 'setInterval(() => {}, 1000)'],
    environment, {stdio: 'pipe', timeoutMs: 100, graceMs: 50});
  await assert.rejects(finishUpload(operation, record, 'timeout'), /deadline/);
  const data = JSON.parse(fs.readFileSync(record));
  assert.equal(data.termination_reason, 'deadline');
  assert.equal(data.pid, operation.child.pid);
  assert.ok(data.completed_at);
});

test('external cancellation retires both uploader and the Python-owned native process group', async t => {
  const directory = fixture(t);
  fs.rmSync(path.join(directory, 'execution'), {recursive: true});
  const group = commandGroup();
  const python = `import os, sys\nfrom pathlib import Path\nfrom scripts.diagnose_hvf_methods import NativeChildren\nfrom scripts.run_native_cpu_methods import execute\np=Path(sys.argv[1])\nwith NativeChildren(p):\n execute([sys.executable,'-c','import time; time.sleep(60)'],str(p),os.environ,60,p/'execution')\n`;
  const native = group.start('python3', ['-c', python, directory], environment,
    {stdio: 'pipe', timeoutMs: 10000});
  const upload = group.start(process.execPath, ['-e',
    "process.on('SIGTERM', () => {}); process.stdout.write('ready'); setInterval(() => {}, 1000)"],
  environment, {stdio: 'pipe', timeoutMs: 10000, graceMs: 50});
  const results = Promise.allSettled([native.completion, upload.completion]);
  try {
    await once(upload.child.stdout, 'data');
    for (let i = 0; i < 500 && !fs.existsSync(path.join(directory, 'children.json')); ++i) await delay(10);
    assert.ok(fs.existsSync(path.join(directory, 'children.json')));
    group.cancel();
    const states = await results;
    assert.ok(states.every(result => result.status === 'rejected'));
    assert.equal(upload.result.signal, 'SIGKILL');
    const retirement = JSON.parse(fs.readFileSync(path.join(directory, 'retirement.json')));
    assert.equal(retirement.length, 1);
    assert.equal(retirement[0].retired, true);
    assert.throws(() => process.kill(-retirement[0].pid, 0), /ESRCH/);
    assert.throws(() => group.start('python3', [], environment), /interrupted/);
  } finally { group.cancel(); await results; }
});
