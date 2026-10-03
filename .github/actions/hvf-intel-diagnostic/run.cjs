// Temporary diagnostic orchestration. No third-party implementation is vendored.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {spawn, spawnSync} = require('node:child_process');
const UPLOAD_REVISION = '043fb46d1a93c77aae656e7c1c64a875d1fc6a0a';

function testEnvironment(environment) {
  return Object.fromEntries(Object.entries(environment).filter(([name]) =>
    !name.startsWith('ACTIONS_') && !name.startsWith('INPUT_') &&
    !['GITHUB_TOKEN', 'GH_TOKEN', 'SSH_AUTH_SOCK'].includes(name)));
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
    if (terminated) return;
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
  let active;
  let interrupted = false;
  const interrupt = () => {
    interrupted = true;
    if (active) active.cancel();
  };
  process.on('SIGINT', interrupt);
  process.on('SIGTERM', interrupt);
  const command = async (binary, args, environment, timeout = 180000, statusPath) => {
    const operation = startCommand(binary, args, environment, {timeoutMs: timeout});
    active = operation;
    try {
      return await operation.completion;
    } finally {
      if (active === operation) active = undefined;
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
  const prefix = `hvf-intel-diagnostic-shard-${shard.split('/')[0]}-attempt-${attempt}`;
  await runSequence(plan, async (phase, index) => {
    if (interrupted && phase !== 'finish') throw new Error('diagnostic interrupted');
    const suffix = index === undefined ? 'plan' : `method-${String(index).padStart(4, '0')}-${phase}`;
    const artifactPath = index === undefined ? evidence : path.join(evidence, `method-${String(index).padStart(4, '0')}`);
    // Node actions receive the runtime artifact token; native test children do
    // not. Direct invocation needs all action.yml defaults set explicitly.
    const uploadEnvironment = {...process.env,
      INPUT_NAME: `${prefix}-${suffix}`, INPUT_PATH: artifactPath,
      'INPUT_IF-NO-FILES-FOUND': 'error', 'INPUT_RETENTION-DAYS': '7',
      'INPUT_COMPRESSION-LEVEL': '6', INPUT_OVERWRITE: 'false',
      'INPUT_INCLUDE-HIDDEN-FILES': 'false', INPUT_ARCHIVE: 'true',
    };
    if (await command(process.execPath, [path.join(uploader, 'dist/upload/index.js')],
                uploadEnvironment, 120000) !== 0) {
      throw new Error(`diagnostic ${suffix} upload failed`);
    }
  }, index => {
    if (interrupted) throw new Error('diagnostic interrupted before guest execution');
    return command('python3', [helper, 'execute', ...common,
      '--index', String(index)], environment, 180000,
    path.join(evidence, `method-${String(index).padStart(4, '0')}`, 'controller-status.json'));
  });
}

module.exports = {runSequence, startCommand, testEnvironment};
if (require.main === module) {
  main().catch(error => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
