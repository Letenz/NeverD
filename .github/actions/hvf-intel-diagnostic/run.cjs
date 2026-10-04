// Temporary diagnostic orchestration. No third-party implementation is vendored.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {spawn, spawnSync, execFile} = require('node:child_process');
const {captureActive, observeExecution, preserveActive, waitForNativeSample,
  validateSampling} = require('./active-sample.cjs');
const {captureRuntime} = require('./runtime.cjs');
const UPLOAD_REVISION = '043fb46d1a93c77aae656e7c1c64a875d1fc6a0a';

function testEnvironment(environment) {
  return Object.fromEntries(Object.entries(environment).filter(([name]) =>
    !name.startsWith('ACTIONS_') && !name.startsWith('INPUT_') &&
    !['GITHUB_TOKEN', 'GH_TOKEN', 'SSH_AUTH_SOCK'].includes(name)));
}

async function captureHostState(destination, environment, signal) {
  // These read-only commands contain no arguments or environment of other
  // processes. Record collection failures too; absence is not a healthy host.
  const commands = [
    ['/usr/sbin/sysctl', ['hw.memsize', 'vm.swapusage', 'vm.loadavg']],
    ['/usr/bin/vm_stat', []],
    ['/bin/df', ['-k', destination]],
    ['/bin/ps', ['-axo', 'pid=,ppid=,pgid=,state=,pcpu=,rss=,comm=']],
  ];
  const started = new Date().toISOString();
  const samples = await Promise.all(commands.map(([binary, args]) =>
    new Promise(resolve => {
      execFile(binary, args, {env: environment, signal, encoding: 'utf8', timeout: 2000,
        killSignal: 'SIGKILL', maxBuffer: 1024 * 1024}, (error, stdout, stderr) => {
        resolve({command: [binary, ...args], stdout, stderr,
          status: error ? (error.code ?? null) : 0,
          signal: error?.signal ?? null, killed: error?.killed ?? false});
      });
    })));
  return {kind: 'diagnostic-host-snapshot', started_at: started,
    completed_at: new Date().toISOString(), samples};
}

async function runSequence(plan, upload, execute) {
  await upload('plan');
  for (const method of plan.methods) {
    // Completion of this upload is a prerequisite for any guest execution.
    await upload('start', method.index);
    let failure;
    try {
      const status = await execute(method.index);
      if (status !== 0) failure = new Error(`method ${method.index} failed: ${status}`);
    } catch (error) {
      failure = error;
    }
    await upload('finish', method.index);
    if (failure) throw failure;
  }
}

function startCommand(binary, args, environment, options = {}) {
  const child = spawn(binary, args, {env: environment, stdio: options.stdio || 'inherit'});
  const result = {kind: 'diagnostic-controller-child', started_at: new Date().toISOString(),
    completed_at: null, exit_status: null, signal: null, termination_reason: null,
    spawn_error: null};
  let terminated = false;
  let forced;
  const cancel = (reason = 'external-cancellation') => {
    if (terminated || result.completed_at) return;
    terminated = true;
    result.termination_reason = reason;
    child.kill('SIGTERM');
    // Give Python time to retire the native child in its independent session.
    forced = setTimeout(() => child.kill('SIGKILL'), options.graceMs ?? 10000);
  };
  const completion = new Promise((resolve, reject) => {
    const timer = setTimeout(() => cancel('deadline'), options.timeoutMs ?? 180000);
    const cleanup = () => { clearTimeout(timer); clearTimeout(forced); };
    child.once('error', error => {
      result.spawn_error = error.code;
      result.completed_at = new Date().toISOString();
      cleanup();
      reject(error);
    });
    child.once('close', (status, signal) => {
      result.exit_status = status;
      result.signal = signal;
      result.completed_at = new Date().toISOString();
      cleanup();
      if (terminated) reject(new Error('diagnostic subprocess cancelled or exceeded its deadline'));
      else resolve(status);
    });
  });
  return {child, cancel, completion, result};
}

async function main() {
  const active = new Set();
  const samplingAbort = new AbortController();
  let interrupted = false;
  const interrupt = () => {
    interrupted = true;
    samplingAbort.abort();
    for (const operation of active) operation.cancel();
  };
  process.on('SIGINT', interrupt);
  process.on('SIGTERM', interrupt);
  const command = async (binary, args, environment, timeout = 180000, statusPath,
    observer, preserveAfterInterrupt = false) => {
    if (interrupted && !preserveAfterInterrupt) throw new Error('diagnostic interrupted');
    const operation = startCommand(binary, args, environment, {timeoutMs: timeout});
    active.add(operation);
    try {
      return await (observer ? observeExecution(operation,
        isRunning => observer(operation.child.pid, isRunning), 0) : operation.completion);
    } finally {
      active.delete(operation);
      if (statusPath) fs.writeFileSync(statusPath, JSON.stringify(operation.result, null, 2) + '\n', {flag: 'wx'});
    }
  };
  const input = name => {
    const value = process.env[`INPUT_${name.toUpperCase()}`];
    if (!value) throw new Error(`missing action input: ${name}`);
    return value;
  };
  const source = path.resolve(input('source'));
  const evidence = path.resolve(input('evidence'));
  const uploader = path.resolve(input('upload-action'));
  const helper = path.resolve(__dirname, '../../../scripts/diagnose_hvf_methods.py');
  const environment = testEnvironment(process.env);
  const revision = spawnSync('git', ['-C', uploader, 'rev-parse', 'HEAD'], {
    env: environment, encoding: 'utf8', timeout: 10000,
  });
  if (revision.status !== 0 || revision.stdout.trim() !== UPLOAD_REVISION) {
    throw new Error('artifact uploader is not the pinned official revision');
  }
  const shard = input('shard');
  const attempt = process.env.GITHUB_RUN_ATTEMPT;
  if (!/^\d+\/\d+$/.test(shard) || !/^\d+$/.test(attempt || '')) {
    throw new Error('invalid shard or workflow attempt');
  }
  const common = ['--source', source, '--evidence', evidence];
  const status = await command('python3', [helper, 'prepare', ...common,
    '--build', path.resolve(input('build')), '--shard', shard,
    '--first', input('first-method'), '--count', input('method-count'),
    '--case-index', input('case-index')], environment);
  if (status !== 0) throw new Error(`diagnostic preparation failed: ${status}`);
  const plan = JSON.parse(fs.readFileSync(path.join(evidence, 'plan.json'), 'utf8'));
  fs.writeFileSync(path.join(evidence, 'observer-runtime.json'),
    JSON.stringify(captureRuntime(), null, 2) + '\n', {flag: 'wx'});
  const sampling = validateSampling(input('sample-active-child'), plan.methods.length);
  fs.writeFileSync(path.join(evidence, 'controller-options.json'), JSON.stringify({
    kind: 'partial-hvf-diagnostic-options', complete_inventory: false,
    sample_active_child: sampling, sample_after_ms: sampling ? 5000 : null,
    sample_clock: sampling ? 'observed-native-registration' : null,
  }, null, 2) + '\n', {flag: 'wx'});
  const prefix = `hvf-intel-diagnostic-shard-${shard.split('/')[0]}-attempt-${attempt}`;
  const upload = async (phase, index) => {
    if (interrupted && phase !== 'finish') throw new Error('diagnostic interrupted');
    const suffix = index === undefined ? 'plan' : `method-${String(index).padStart(4, '0')}-${phase}`;
    const methodPath = index === undefined ? evidence : path.join(evidence, `method-${String(index).padStart(4, '0')}`);
    const artifactPath = phase === 'active' ? path.join(methodPath, 'active-sample') : methodPath;
    if (index !== undefined && phase !== 'active') {
      const snapshot = await captureHostState(artifactPath, environment);
      fs.writeFileSync(path.join(artifactPath, `host-${phase}.json`),
        JSON.stringify(snapshot, null, 2) + '\n', {flag: 'wx'});
    }
    // Node actions receive the runtime artifact token; native test children do
    // not. Direct invocation needs all action.yml defaults set explicitly.
    const uploadEnvironment = {...process.env,
      INPUT_NAME: `${prefix}-${suffix}`, INPUT_PATH: artifactPath,
      'INPUT_IF-NO-FILES-FOUND': 'error', 'INPUT_RETENTION-DAYS': '7',
      'INPUT_COMPRESSION-LEVEL': '6', INPUT_OVERWRITE: 'false',
      'INPUT_INCLUDE-HIDDEN-FILES': 'false', INPUT_ARCHIVE: 'true',
    };
    if (await command(process.execPath, [path.join(uploader, 'dist/upload/index.js')],
                uploadEnvironment, 120000, undefined, undefined, phase === 'finish') !== 0) {
      throw new Error(`diagnostic ${suffix} upload failed`);
    }
  };
  await runSequence(plan, upload, index => {
    if (interrupted) throw new Error('diagnostic interrupted before guest execution');
    const directory = path.join(evidence, `method-${String(index).padStart(4, '0')}`);
    const observer = sampling ? async (pythonPid, isRunning) => {
      const registration = await waitForNativeSample(directory, isRunning, samplingAbort.signal);
      if (!registration) return;
      const marker = JSON.parse(fs.readFileSync(path.join(directory, 'prepared.json'), 'utf8'));
      await preserveActive(() => captureActive({directory, pythonPid, binary: marker.method[0].binary,
        environment, signal: samplingAbort.signal, isRunning, captureHostState, registration}),
      () => upload('active', index), samplingAbort.signal);
    } : undefined;
    return command('python3', [helper, 'execute', ...common,
      '--index', String(index)], environment, 180000,
    path.join(directory, 'controller-status.json'), observer);
  });
}

module.exports = {runSequence, startCommand, testEnvironment, captureHostState, UPLOAD_REVISION};
if (require.main === module) {
  main().catch(error => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
