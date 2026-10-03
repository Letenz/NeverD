'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {execFile} = require('node:child_process');

const LIMIT = 1024 * 1024;

function boundedCommand(binary, args, environment, signal, timeout = 2000) {
  const started = new Date().toISOString();
  return new Promise(resolve => {
    execFile(binary, args, {env: environment, signal, encoding: 'utf8',
      timeout, killSignal: 'SIGKILL', maxBuffer: LIMIT}, (error, stdout, stderr) => {
      resolve({command: [binary, ...args], started_at: started,
        completed_at: new Date().toISOString(), stdout, stderr,
        status: error ? (error.code ?? null) : 0,
        signal: error?.signal ?? null, killed: error?.killed ?? false});
    });
  });
}

function readTail(source, destination, limit = LIMIT) {
  const fd = fs.openSync(source, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  try {
    const stat = fs.fstatSync(fd);
    if (!stat.isFile()) throw new Error('sample source is not a regular file');
    const offset = Math.max(0, stat.size - limit);
    const buffer = Buffer.alloc(Math.min(stat.size, limit));
    const length = fs.readSync(fd, buffer, 0, buffer.length, offset);
    fs.writeFileSync(destination, buffer.subarray(0, length), {flag: 'wx'});
    return {observed_at: new Date().toISOString(), size_at_open: stat.size,
      first_byte: offset, copied_bytes: length, truncated: offset !== 0};
  } finally {
    fs.closeSync(fd);
  }
}

function verifyIdentity(text, pid, pythonPid, binary) {
  const match = /^\s*(\d+)\s+(\d+)\s+(\d+)\s+([^\r\n]+?)\s*$/.exec(text);
  return Boolean(match && Number(match[1]) === pid && Number(match[2]) === pythonPid &&
    Number(match[3]) === pid && match[4] === binary);
}

async function captureActive({directory, pythonPid, binary, environment, signal,
  isRunning, captureHostState, collect = boundedCommand}) {
  const destination = path.join(directory, 'active-sample');
  fs.mkdirSync(destination);
  const metadata = {kind: 'instrumented-partial-hvf-active-sample',
    complete_inventory: false, started_at: new Date().toISOString(),
    python_pid: pythonPid, expected_binary: binary, errors: []};
  const allowed = () => isRunning() && !signal.aborted;
  const save = (name, value) => fs.writeFileSync(path.join(destination, name),
    JSON.stringify(value, null, 2) + '\n', {flag: 'wx'});
  try {
    // Read a bounded copy: children.json may be between writes or the native
    // process may have exited. Never fall back to selecting by process name.
    readTail(path.join(directory, 'children.json'), path.join(destination, 'children.json'), 4096);
    const registry = JSON.parse(fs.readFileSync(path.join(destination, 'children.json'), 'utf8'));
    if (!Array.isArray(registry.process_groups) || registry.process_groups.length !== 1 ||
        !Number.isSafeInteger(registry.process_groups[0]) || registry.process_groups[0] <= 1) {
      throw new Error('native process identity is not unique');
    }
    metadata.native_pid = registry.process_groups[0];
    if (!allowed()) throw new Error('execution completed or was cancelled before identity check');
    const identity = await collect('/bin/ps', ['-ww', '-p', String(metadata.native_pid),
      '-o', 'pid=,ppid=,pgid=,comm='], environment, signal);
    save('identity.json', identity);
    if (identity.status !== 0 || !verifyIdentity(identity.stdout, metadata.native_pid, pythonPid, binary)) {
      throw new Error('native process identity changed or is unavailable');
    }
    metadata.identity_verified = true;
    if (!allowed()) throw new Error('execution completed or was cancelled after identity check');
    metadata.output = readTail(path.join(directory, 'execution/methods/0000/output.log'),
      path.join(destination, 'output-tail.log'));
    // /dev/stdout avoids sample's otherwise unbounded report file in /tmp.
    // execFile caps the actual report stream and kills collection after 5s.
    const sample = await collect('/usr/bin/sample', [String(metadata.native_pid),
      '1', '10', '-mayDie', '-file', '/dev/stdout'], environment, signal, 5000);
    save('sample.json', sample);
    if (sample.status !== 0) metadata.errors.push('stack sampling failed; inspect sample.json');
    if (allowed()) save('host.json', await captureHostState(destination, environment, signal));
  } catch (error) {
    metadata.errors.push(error.message);
  }
  metadata.completed_at = new Date().toISOString();
  metadata.execution_running_at_end = isRunning();
  metadata.cancelled = signal.aborted;
  save('metadata.json', metadata);
  return destination;
}

async function observeExecution(operation, sample, delayMs = 5000) {
  let running = true;
  let timer;
  const completion = operation.completion.finally(() => {
    running = false;
    clearTimeout(timer);
  });
  // Attach rejection handlers immediately, including while the timer waits.
  const delayed = new Promise(resolve => {
    timer = setTimeout(() => resolve(true), delayMs);
    completion.then(() => resolve(false), () => resolve(false));
  });
  const observation = (async () => {
    if (await delayed && running) {
      try {
        await sample(() => running);
      } catch (error) {
        operation.cancel('active-evidence-failure');
        throw error;
      }
    }
  })();
  const [execution, observed] = await Promise.allSettled([completion, observation]);
  if (observed.status === 'rejected') throw observed.reason;
  if (execution.status === 'rejected') throw execution.reason;
  return execution.value;
}

async function preserveActive(capture, upload, signal) {
  if (signal.aborted) throw new Error('diagnostic interrupted before active sampling');
  await capture();
  if (signal.aborted) throw new Error('diagnostic interrupted before active upload');
  await upload();
}

function validateSampling(value, methodCount) {
  if (!['true', 'false'].includes(value)) throw new Error('invalid sample-active-child input');
  // upload-artifact allows 500 artifacts/job: plan + 3/method + final.
  if (value === 'true' && methodCount > 166) throw new Error('sampling supports at most 166 methods');
  return value === 'true';
}

module.exports = {captureActive, observeExecution, preserveActive,
  validateSampling, verifyIdentity, readTail};
